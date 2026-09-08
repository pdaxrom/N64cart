/**
 * Copyright (c) 2022-2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "romfs.h"
#include "romfs_flash.h"

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define to_lsb16(a) (a)
#define from_lsb16(a) (a)
#define to_lsb32(a) (a)
#define from_lsb32(a) (a)
#else
#define to_lsb16(a)   (((((a) >> 8) & 0xff) | ((a) << 8)) & 0xffff)
#define from_lsb16(a) (((((a) >> 8) & 0xff) | ((a) << 8)) & 0xffff)
#define to_lsb32(a)   ((((a) & 0xff000000) >> 24) | (((a) & 0x00ff0000) >> 8) | (((a) & 0x0000ff00) << 8) | (((a) & 0x000000ff) << 24))
#define from_lsb32(a) ((((a) & 0xff000000) >> 24) | (((a) & 0x00ff0000) >> 8) | (((a) & 0x0000ff00) << 8) | (((a) & 0x000000ff) << 24))
#endif

static const char *romfs_errlist[] = {
    "No error",
    "No io buffer",
    "No list entry",
    "No free list entries",
    "No free space",
    "File exists",
    "File data too long",
    "Buffer too small",
    "End of file",
    "Operation error",
    "Directory limit reached",
    "Invalid directory",
    "Directory not empty",
    "File busy",
    "Flash I/O error",
};

static uint32_t flash_start = 0;
static uint32_t flash_map_size = 0;
static uint32_t flash_list_size = 0;
/* Exclusive upper bound: map padding and the 0xffff sentinel are not data. */
static uint32_t flash_sector_limit = 0;
static uint32_t flash_data_start = 0;
/* Search starting point only: RAM map checks remain authoritative. */
static uint32_t romfs_alloc_hint;

static uint16_t *flash_map_int;
static uint8_t *flash_list_int;
static bool romfs_ready;

static uint32_t romfs_garbage_collect(bool *freed);
#define ROMFS_DIR_FILTER_ANY 0xff
#define ROMFS_LIST_INCLUDE_FILES 0x01
#define ROMFS_LIST_INCLUDE_DIRS 0x02
#define ROMFS_BUFFER_NONE 0xffffffffu

static uint16_t romfs_dir_entry_index[ROMFS_MAX_DIRS];
static uint16_t romfs_dir_used_mask = (1u << ROMFS_ROOT_DIR_ID);
static uint32_t romfs_dir_generation[ROMFS_MAX_DIRS];
static romfs_file *romfs_open_files;

static void romfs_dir_index_reset(void);
static void romfs_dir_index_rebuild(void);
static int romfs_dir_alloc_id(void);
static void romfs_dir_release_id(uint8_t id);
static bool romfs_dir_id_valid(uint8_t id);
static bool romfs_dir_is_empty_internal(uint8_t dir_id);
static uint32_t romfs_resolve_parent(const char *path, bool create_dirs, romfs_dir *parent_dir, char *leaf,
                                     size_t leaf_len);
static bool romfs_valid_entry_name(const char *name, size_t len);
static bool romfs_type_is_service(uint16_t type);
static bool romfs_entry_is_protected(const romfs_entry *entry);
static uint32_t romfs_first_data_sector(void);
static void romfs_link_sector_range(uint32_t start, uint32_t count);
static void romfs_reserve_service_sectors(void);
static int romfs_dir_parent_id(uint8_t id);
static uint32_t romfs_flush(void);
static uint32_t romfs_sync_write_file(romfs_file *file);
static void romfs_operation_enter(void);
static uint32_t romfs_operation_leave(uint32_t status);

static uint32_t romfs_flush_depth;
/* At most 32 map sectors and 4 catalog sectors for the supported geometry. */
static uint32_t romfs_map_dirty;
static uint32_t romfs_list_dirty;

static void romfs_store_map_link(uint32_t sector, uint16_t next)
{
    uint16_t raw = to_lsb16(next);
    if (flash_map_int[sector] != raw) {
        flash_map_int[sector] = raw;
        romfs_map_dirty |= UINT32_C(1) << (sector / (ROMFS_FLASH_SECTOR / sizeof(uint16_t)));
    }
}

static void romfs_mark_list_entry(uint32_t index)
{
    romfs_list_dirty |= UINT32_C(1) << (index / (ROMFS_FLASH_SECTOR / sizeof(romfs_entry)));
}

static uint32_t romfs_metadata_mask(uint32_t size)
{
    uint32_t count = size / ROMFS_FLASH_SECTOR;
    return count == 32 ? UINT32_MAX : (UINT32_C(1) << count) - 1;
}

static void romfs_mark_all_metadata(void)
{
    romfs_map_dirty = romfs_metadata_mask(flash_map_size);
    romfs_list_dirty = romfs_metadata_mask(flash_list_size);
}

static bool romfs_file_is_open(const romfs_file *file)
{
    for (romfs_file *opened = romfs_open_files; opened; opened = opened->next_open) {
        if (opened == file) {
            return true;
        }
    }
    return false;
}

static void romfs_register_file(romfs_file *file)
{
    file->next_open = romfs_open_files;
    romfs_open_files = file;
}

static void romfs_unregister_file(romfs_file *file)
{
    for (romfs_file **link = &romfs_open_files; *link; link = &(*link)->next_open) {
        if (*link == file) {
            *link = file->next_open;
            file->next_open = NULL;
            return;
        }
    }
}

static bool romfs_slot_busy(uint32_t entry_index, const romfs_file *except, bool writers_only)
{
    for (romfs_file *file = romfs_open_files; file; file = file->next_open) {
        if (file != except && file->nentry == entry_index &&
                (!writers_only || file->op == ROMFS_OP_WRITE)) {
            return true;
        }
    }
    return false;
}

static bool romfs_name_pending(uint8_t parent, const char *name)
{
    for (romfs_file *file = romfs_open_files; file; file = file->next_open) {
        if (file->entry_pending && file->entry.attr.names.parent == parent &&
                strncmp(file->entry.name, name, ROMFS_MAX_NAME_LEN) == 0) {
            return true;
        }
    }
    return false;
}

static void romfs_dir_index_reset(void)
{
    for (uint32_t i = 0; i < ROMFS_MAX_DIRS; i++) {
        romfs_dir_entry_index[i] = ROMFS_INVALID_ENTRY_ID;
        romfs_dir_generation[i]++;
    }
    romfs_dir_used_mask = (1u << ROMFS_ROOT_DIR_ID);
}

static void romfs_dir_index_rebuild(void)
{
    romfs_dir_index_reset();
    if (!flash_list_int) {
        return;
    }

    romfs_entry *entries = (romfs_entry *) flash_list_int;
    uint32_t total = flash_list_size / sizeof(romfs_entry);
    for (uint32_t i = 0; i < total; i++) {
        if (entries[i].name[0] == ROMFS_EMPTY_ENTRY ||
                entries[i].name[0] == ROMFS_DELETED_ENTRY) {
            continue;
        }
        romfs_entry copy = entries[i];
        copy.attr.raw = from_lsb16(copy.attr.raw);
        if (copy.attr.names.type == ROMFS_TYPE_DIR) {
            uint8_t id = copy.attr.names.current;
            if (id < ROMFS_MAX_DIRS) {
                romfs_dir_entry_index[id] = i;
                romfs_dir_used_mask |= (1u << id);
            }
        }
    }
}

static int romfs_dir_alloc_id(void)
{
    for (int i = 1; i < ROMFS_MAX_DIRS; i++) {
        if ((romfs_dir_used_mask & (1u << i)) == 0) {
            romfs_dir_used_mask |= (1u << i);
            romfs_dir_generation[i]++;
            return i;
        }
    }
    return -1;
}

static void romfs_dir_release_id(uint8_t id)
{
    if (id == ROMFS_ROOT_DIR_ID || id >= ROMFS_MAX_DIRS) {
        return;
    }
    romfs_dir_used_mask &= (uint16_t) ~(1u << id);
    romfs_dir_entry_index[id] = ROMFS_INVALID_ENTRY_ID;
}

static bool romfs_dir_id_valid(uint8_t id)
{
    return id < ROMFS_MAX_DIRS && (romfs_dir_used_mask & (1u << id));
}

static bool romfs_dir_valid(const romfs_dir *dir)
{
    if (!romfs_ready || !dir || !romfs_dir_id_valid(dir->id)) {
        return false;
    }
    return dir->id == ROMFS_ROOT_DIR_ID ||
           (dir->generation == romfs_dir_generation[dir->id] &&
            dir->entry_index == romfs_dir_entry_index[dir->id]);
}

static bool romfs_valid_entry_name(const char *name, size_t len)
{
    if (!name || len == 0 || len >= ROMFS_MAX_NAME_LEN) {
        return false;
    }
    if ((len == 1 && name[0] == '.') ||
            (len == 2 && name[0] == '.' && name[1] == '.')) {
        return false;
    }
    if (name[0] == ROMFS_EMPTY_ENTRY || name[0] == ROMFS_DELETED_ENTRY || memchr(name, '/', len)) {
        return false;
    }
    return true;
}

static bool romfs_type_is_service(uint16_t type)
{
    type &= 0x1f;
    return type <= ROMFS_TYPE_FLASHMAP;
}

static bool romfs_entry_is_protected(const romfs_entry *entry)
{
    uint16_t protected_mode = ROMFS_MODE_SYSTEM | ROMFS_MODE_RESERVED;

    return (entry->attr.names.mode & protected_mode) != 0 ||
           romfs_type_is_service(entry->attr.names.type);
}

static uint32_t romfs_first_data_sector(void)
{
    return flash_data_start;
}

static uint32_t romfs_sector_count(uint32_t size)
{
    return size / ROMFS_FLASH_SECTOR + (size % ROMFS_FLASH_SECTOR != 0);
}

/* A position used only during one public write/truncate operation. Never cache
 * validation across calls: the caller owns the RAM map and can change it. */
typedef struct {
    uint32_t index;
    uint32_t sector;
} romfs_write_cursor;

/* Validate the entire chain before exposing data or changing any of its links.
 * Exactly count links, with a self-link only at the end, also rejects cycles.
 * Also locate the buffer's sector and an optional write position in this walk.
 * A write beyond EOF starts its cursor at the last existing sector. */
static bool romfs_validate_chain_with_cursor(const romfs_entry *entry, uint32_t index, uint32_t *sector_out,
                                              romfs_write_cursor *cursor)
{
    uint32_t first = flash_data_start;
    uint32_t limit = flash_sector_limit;
    uint32_t count = romfs_sector_count(entry->size);
    uint32_t type = entry->attr.names.type;

    if (!romfs_ready || !flash_map_int || limit == 0 || type == ROMFS_TYPE_DIR) {
        return false;
    }
    if (romfs_type_is_service(type)) {
        uint32_t size;
        if (type == ROMFS_TYPE_FIRMWARE) {
            first = 0;
            size = flash_start;
        } else if (type == ROMFS_TYPE_FLASHLIST) {
            first = flash_start / ROMFS_FLASH_SECTOR;
            size = flash_list_size;
        } else {
            first = (flash_start + flash_list_size) / ROMFS_FLASH_SECTOR;
            size = flash_map_size;
        }
        limit = first + size / ROMFS_FLASH_SECTOR;
        if (entry->start != first || entry->size != size) {
            return false;
        }
    } else if (count == 0) {
        if (entry->start != 0xffff || sector_out) {
            return false;
        }
        if (cursor) {
            *cursor = (romfs_write_cursor) {0, 0xffff};
        }
        return true;
    }
    if (count > limit - first || (sector_out && index >= count)) {
        return false;
    }

    uint32_t sector = entry->start;
    uint32_t selected = 0xffff;
    uint32_t cursor_index = cursor && count ? (cursor->index < count ? cursor->index : count - 1) : 0;
    uint32_t cursor_sector = 0xffff;
    for (uint32_t i = 0; i < count; i++) {
        if (sector < first || sector >= limit) {
            return false;
        }
        uint32_t next = from_lsb16(flash_map_int[sector]);
        if (i + 1 == count) {
            if (next != sector) {
                return false;
            }
        } else if (next == sector) {
            return false;
        }
        if (i == index) {
            selected = sector;
        }
        if (cursor && i == cursor_index) {
            cursor_sector = sector;
        }
        sector = next;
    }
    if (sector_out) {
        *sector_out = selected;
    }
    if (cursor) {
        *cursor = (romfs_write_cursor) {cursor_index, cursor_sector};
    }
    return true;
}

static bool romfs_validate_chain(const romfs_entry *entry, uint32_t index, uint32_t *sector_out)
{
    return romfs_validate_chain_with_cursor(entry, index, sector_out, NULL);
}

static bool romfs_validate_write_file(const romfs_file *file, romfs_write_cursor *cursor)
{
    if (romfs_entry_is_protected(&file->entry) ||
            (file->entry.attr.names.mode & ROMFS_MODE_READONLY) != 0) {
        return false;
    }
    if (file->buffer_base == ROMFS_BUFFER_NONE) {
        return !file->buffer_dirty && romfs_validate_chain_with_cursor(&file->entry, 0, NULL, cursor);
    }
    if (!file->io_buffer || file->buffer_base >= file->entry.size ||
            file->buffer_base % ROMFS_FLASH_SECTOR != 0) {
        return false;
    }
    uint32_t sector;
    return romfs_validate_chain_with_cursor(&file->entry, file->buffer_base / ROMFS_FLASH_SECTOR, &sector, cursor) &&
           file->pos == sector;
}

static void romfs_link_sector_range(uint32_t start, uint32_t count)
{
    uint32_t total_sectors = flash_sector_limit;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t sector = start + i;
        if (sector >= total_sectors) {
            break;
        }

        uint32_t next = (i + 1 < count) ? (sector + 1) : sector;
        if (next >= total_sectors) {
            next = sector;
        }
        romfs_store_map_link(sector, next);
    }
}

static void romfs_reserve_service_sectors(void)
{
    uint32_t firmware_count = flash_start / ROMFS_FLASH_SECTOR;
    uint32_t list_start = flash_start / ROMFS_FLASH_SECTOR;
    uint32_t list_count = flash_list_size / ROMFS_FLASH_SECTOR;
    uint32_t map_start = (flash_start + flash_list_size) / ROMFS_FLASH_SECTOR;
    uint32_t map_count = flash_map_size / ROMFS_FLASH_SECTOR;

    romfs_link_sector_range(0, firmware_count);
    romfs_link_sector_range(list_start, list_count);
    romfs_link_sector_range(map_start, map_count);
}

static int romfs_dir_parent_id(uint8_t id)
{
    if (id == ROMFS_ROOT_DIR_ID || id >= ROMFS_MAX_DIRS) {
        return -1;
    }
    uint16_t entry_index = romfs_dir_entry_index[id];
    if (entry_index == ROMFS_INVALID_ENTRY_ID) {
        return -1;
    }
    romfs_entry *entries = (romfs_entry *) flash_list_int;
    romfs_entry entry = entries[entry_index];
    entry.attr.raw = from_lsb16(entry.attr.raw);
    return entry.attr.names.parent;
}

static void romfs_operation_enter(void)
{
    romfs_flush_depth++;
}

static uint32_t romfs_operation_leave(uint32_t status)
{
    if (romfs_flush_depth == 0) {
        return status;
    }
    romfs_flush_depth--;
    if (romfs_flush_depth == 0 && status == ROMFS_NOERR) {
        return romfs_sync();
    }
    return status;
}

static bool romfs_dir_is_empty_internal(uint8_t dir_id)
{
    for (romfs_file *file = romfs_open_files; file; file = file->next_open) {
        if (file->entry_pending && file->entry.attr.names.parent == dir_id) {
            return false;
        }
    }
    romfs_entry *entries = (romfs_entry *) flash_list_int;
    uint32_t total = flash_list_size / sizeof(romfs_entry);
    for (uint32_t i = 0; i < total; i++) {
        if (entries[i].name[0] == ROMFS_EMPTY_ENTRY ||
                entries[i].name[0] == ROMFS_DELETED_ENTRY) {
            continue;
        }
        romfs_entry copy = entries[i];
        copy.attr.raw = from_lsb16(copy.attr.raw);
        if (copy.attr.names.parent == dir_id) {
            return false;
        }
    }
    return true;
}

void romfs_get_buffers_sizes(uint32_t rom_size, uint32_t *map_size, uint32_t *list_size)
{
    uint32_t map_bytes = 0;
    uint32_t list_bytes = 0;
    if (rom_size != 0 && rom_size <= 256u * ROMFS_MB &&
            (rom_size & (ROMFS_FLASH_SECTOR - 1)) == 0) {
        map_bytes = ((rom_size / ROMFS_FLASH_SECTOR) * sizeof(uint16_t) + ROMFS_FLASH_SECTOR - 1) &
                    ~(ROMFS_FLASH_SECTOR - 1);
        list_bytes = ((rom_size / ROMFS_MB) * sizeof(romfs_entry) + ROMFS_FLASH_SECTOR - 1) &
                     ~(ROMFS_FLASH_SECTOR - 1);
        if (list_bytes < ROMFS_FLASH_SECTOR) {
            list_bytes = ROMFS_FLASH_SECTOR;
        }
    }

    if (map_size) {
        *map_size = map_bytes;
    }

    if (list_size) {
        *list_size = list_bytes;
    }
}

bool romfs_start(uint32_t start, uint32_t rom_size, uint16_t *flash_map, uint8_t *flash_list)
{
    uint32_t map_bytes, list_bytes;
    romfs_get_buffers_sizes(rom_size, &map_bytes, &list_bytes);
    uint32_t aligned_start = romfs_align_flash_start(start);
    uint32_t sector_limit = rom_size / ROMFS_FLASH_SECTOR;
    if (sector_limit > UINT16_MAX) {
        sector_limit = UINT16_MAX;
    }
    if (!flash_map || !flash_list || (uintptr_t) flash_map % sizeof(uint16_t) != 0 ||
            map_bytes == 0 || list_bytes == 0 || aligned_start > rom_size ||
            list_bytes > rom_size - aligned_start ||
            map_bytes > rom_size - aligned_start - list_bytes) {
        return false;
    }
    uint32_t first_data = (aligned_start + list_bytes + map_bytes) / ROMFS_FLASH_SECTOR;
    if (first_data >= sector_limit) {
        return false;
    }

    /* Publish only after validation, so rejected geometry leaves the mount intact. */
    romfs_ready = false;
    romfs_open_files = NULL;
    romfs_flush_depth = 0;
    romfs_map_dirty = 0;
    romfs_list_dirty = 0;
    flash_start = aligned_start;
    flash_map_size = map_bytes;
    flash_list_size = list_bytes;
    flash_sector_limit = sector_limit;
    flash_data_start = first_data;
    romfs_alloc_hint = first_data;
    flash_map_int = flash_map;
    flash_list_int = flash_list;

    romfs_dir_index_reset();

    for (uint32_t i = 0; i < flash_list_size; i += ROMFS_FLASH_SECTOR) {
        if (!romfs_flash_sector_read(flash_start + i, &flash_list_int[i], ROMFS_FLASH_SECTOR)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < flash_map_size; i += ROMFS_FLASH_SECTOR) {
        if (!romfs_flash_sector_read(flash_start + flash_list_size + i, &((uint8_t *) flash_map_int)[i], ROMFS_FLASH_SECTOR)) {
            return false;
        }
    }
    romfs_dir_index_rebuild();
    romfs_reserve_service_sectors();
    romfs_ready = true;
    return true;
}

static uint32_t romfs_flush(void)
{
    romfs_reserve_service_sectors();

    for (uint32_t i = 0; i < flash_list_size / ROMFS_FLASH_SECTOR; i++) {
        uint32_t bit = UINT32_C(1) << i;
        if (!(romfs_list_dirty & bit)) {
            continue;
        }
        uint32_t offset = i * ROMFS_FLASH_SECTOR;
        if (!romfs_flash_sector_erase(flash_start + offset) ||
                !romfs_flash_sector_write(flash_start + offset, &flash_list_int[offset])) {
            return ROMFS_ERR_IO;
        }
        romfs_list_dirty &= ~bit;
    }

    for (uint32_t i = 0; i < flash_map_size / ROMFS_FLASH_SECTOR; i++) {
        uint32_t bit = UINT32_C(1) << i;
        if (!(romfs_map_dirty & bit)) {
            continue;
        }
        uint32_t offset = i * ROMFS_FLASH_SECTOR;
        if (!romfs_flash_sector_erase(flash_start + flash_list_size + offset) ||
                !romfs_flash_sector_write(flash_start + flash_list_size + offset,
                                           &((uint8_t *) flash_map_int)[offset])) {
            return ROMFS_ERR_IO;
        }
        romfs_map_dirty &= ~bit;
    }
    return ROMFS_NOERR;
}

uint32_t romfs_sync(void)
{
    if (!romfs_ready) {
        return ROMFS_ERR_OPERATION;
    }
    /* The map is shared: publishing it with another writer's old catalog size
     * would persist an invalid chain. First bring every changed writer in sync. */
    for (romfs_file *file = romfs_open_files; file; file = file->next_open) {
        if (file->op != ROMFS_OP_WRITE) {
            continue;
        }
        if (file->nentry >= flash_list_size / sizeof(romfs_entry)) {
            return ROMFS_ERR_OPERATION;
        }
        romfs_entry stored = ((romfs_entry *) flash_list_int)[file->nentry];
        stored.attr.raw = from_lsb16(stored.attr.raw);
        stored.start = from_lsb32(stored.start);
        stored.size = from_lsb32(stored.size);
        if (file->entry_pending || file->buffer_dirty || memcmp(&stored, &file->entry, sizeof(stored)) != 0) {
            uint32_t status = romfs_sync_write_file(file);
            if (status != ROMFS_NOERR) {
                return status;
            }
        }
    }
    return romfs_flush();
}

uint32_t romfs_sync_full(void)
{
    if (!romfs_ready) {
        return ROMFS_ERR_OPERATION;
    }
    romfs_mark_all_metadata();
    return romfs_sync();
}

bool romfs_format(void)
{
    if (!romfs_ready) {
        return false;
    }
    romfs_open_files = NULL;
    romfs_alloc_hint = flash_data_start;
    romfs_operation_enter();
    memset(flash_list_int, 0xff, flash_list_size);
    romfs_dir_index_reset();

    romfs_entry *entry = (romfs_entry *) flash_list_int;

    romfs_entry tmp;

    strncpy(entry[0].name, "firmware", ROMFS_MAX_NAME_LEN - 1);
    entry[0].name[ROMFS_MAX_NAME_LEN - 1] = '\0';
    tmp.attr.names.mode = ROMFS_MODE_READONLY | ROMFS_MODE_SYSTEM;
    tmp.attr.names.type = ROMFS_TYPE_FIRMWARE;
    uint16_t raw = (tmp.attr.names.mode & ROMFS_MODE_MASK) | (tmp.attr.names.type << ROMFS_TYPE_SHIFT);
    entry[0].attr.raw = to_lsb16(raw);
    entry[0].start = to_lsb32(0);
    entry[0].size = to_lsb32(flash_start);

    strncpy(entry[1].name, "flashlist", ROMFS_MAX_NAME_LEN - 1);
    entry[1].name[ROMFS_MAX_NAME_LEN - 1] = '\0';
    tmp.attr.names.mode = ROMFS_MODE_READONLY | ROMFS_MODE_SYSTEM;
    tmp.attr.names.type = ROMFS_TYPE_FLASHLIST;
    raw = (tmp.attr.names.mode & ROMFS_MODE_MASK) | (tmp.attr.names.type << ROMFS_TYPE_SHIFT);
    entry[1].attr.raw = to_lsb16(raw);
    entry[1].start = to_lsb32(flash_start / ROMFS_FLASH_SECTOR);
    entry[1].size = to_lsb32(flash_list_size);

    strncpy(entry[2].name, "flashmap", ROMFS_MAX_NAME_LEN - 1);
    entry[2].name[ROMFS_MAX_NAME_LEN - 1] = '\0';
    tmp.attr.names.mode = ROMFS_MODE_READONLY | ROMFS_MODE_SYSTEM;
    tmp.attr.names.type = ROMFS_TYPE_FLASHMAP;
    raw = (tmp.attr.names.mode & ROMFS_MODE_MASK) | (tmp.attr.names.type << ROMFS_TYPE_SHIFT);
    entry[2].attr.raw = to_lsb16(raw);
    entry[2].start = to_lsb32((flash_start + flash_list_size) / ROMFS_FLASH_SECTOR);
    entry[2].size = to_lsb32(flash_map_size);

    memset((uint8_t *) flash_map_int, 0xff, flash_map_size);

    romfs_reserve_service_sectors();

    romfs_mark_all_metadata();
    uint32_t status = romfs_operation_leave(ROMFS_NOERR);
    romfs_dir_index_rebuild();

    return status == ROMFS_NOERR;
}

uint32_t romfs_free(void)
{
    if (!romfs_ready) {
        return 0;
    }
    uint32_t free_sectors = 0;
    uint32_t reclaimable = 0;
    uint32_t first_data_sector = romfs_first_data_sector();
    uint32_t total_sectors = flash_sector_limit;

    for (uint32_t i = first_data_sector; i < total_sectors; i++) {
        if (flash_map_int[i] == 0xffff) {
            free_sectors++;
        }
    }

    romfs_entry *entries = (romfs_entry *) flash_list_int;
    for (uint32_t i = 0; i < flash_list_size / sizeof(romfs_entry); i++) {
        if (entries[i].name[0] == ROMFS_DELETED_ENTRY) {
            romfs_entry entry_copy = entries[i];
            entry_copy.attr.raw = from_lsb16(entry_copy.attr.raw);
            if (romfs_entry_is_protected(&entry_copy) || entry_copy.attr.names.type == ROMFS_TYPE_DIR) {
                continue;
            }
            entry_copy.start = from_lsb32(entry_copy.start);
            entry_copy.size = from_lsb32(entry_copy.size);
            if (!romfs_validate_chain(&entry_copy, 0, NULL)) {
                /* GC preflight would reject the batch, including valid tombstones. */
                return free_sectors * ROMFS_FLASH_SECTOR;
            }
            uint32_t count = romfs_sector_count(entry_copy.size);
            uint32_t available = total_sectors - first_data_sector - free_sectors - reclaimable;
            reclaimable += count < available ? count : available;
        }
    }

    return (free_sectors + reclaimable) * ROMFS_FLASH_SECTOR;
}

static uint32_t romfs_list_internal(romfs_file *file, bool first, bool with_deleted, uint8_t parent_filter,
                                    uint8_t include_mask)
{
    if (!romfs_ready || !file) {
        return ROMFS_ERR_OPERATION;
    }
    if (romfs_file_is_open(file)) {
        return ROMFS_ERR_BUSY;
    }
    if (first) {
        file->nentry = 0;
    }

    if (include_mask == 0) {
        include_mask = ROMFS_LIST_INCLUDE_FILES | ROMFS_LIST_INCLUDE_DIRS;
    }

    romfs_entry *entries = (romfs_entry *) flash_list_int;
    uint32_t total_entries = flash_list_size / sizeof(romfs_entry);

    while (file->nentry < total_entries) {
        romfs_entry *raw_entry = &entries[file->nentry];
        if ((!with_deleted && raw_entry->name[0] == ROMFS_DELETED_ENTRY) ||
                raw_entry->name[0] == ROMFS_EMPTY_ENTRY) {
            file->nentry++;
            continue;
        }

        romfs_entry entry_copy = *raw_entry;
        entry_copy.attr.raw = from_lsb16(entry_copy.attr.raw);
        bool is_dir = (entry_copy.attr.names.type == ROMFS_TYPE_DIR);

        if ((parent_filter != ROMFS_DIR_FILTER_ANY) &&
                (entry_copy.attr.names.parent != parent_filter)) {
            file->nentry++;
            continue;
        }

        uint8_t mask = is_dir ? ROMFS_LIST_INCLUDE_DIRS : ROMFS_LIST_INCLUDE_FILES;
        if ((include_mask & mask) == 0) {
            file->nentry++;
            continue;
        }

        file->entry.attr.raw = entry_copy.attr.raw;
        memmove(file->entry.name, raw_entry->name, ROMFS_MAX_NAME_LEN);
        file->entry.name[ROMFS_MAX_NAME_LEN - 1] = '\0';
        file->entry.start = from_lsb32(raw_entry->start);
        file->entry.size = from_lsb32(raw_entry->size);

        file->nentry++;
        file->pos = 0;
        file->offset = 0;
        file->parent_dir_id = entry_copy.attr.names.parent;
        file->dir_id = entry_copy.attr.names.current;
        file->buffer_base = ROMFS_BUFFER_NONE;
        file->write_offset = 0;
        file->buffer_dirty = false;

        return (file->err = ROMFS_NOERR);
    }

    return (file->err = ROMFS_ERR_NO_FREE_ENTRIES);
}

static uint32_t romfs_find_file_internal(romfs_file *file, const char *name, uint8_t parent_dir_id, bool include_dirs)
{
    if (!romfs_valid_entry_name(name, name ? strlen(name) : 0)) {
        return (file->err = ROMFS_ERR_FILE_DATA_TOO_BIG);
    }
    if (!romfs_ready || romfs_file_is_open(file)) {
        return (file->err = ROMFS_ERR_NO_ENTRY);
    }
    uint8_t mask = ROMFS_LIST_INCLUDE_FILES;
    if (include_dirs) {
        mask |= ROMFS_LIST_INCLUDE_DIRS;
    }

    romfs_entry *entries = (romfs_entry *) flash_list_int;
    uint32_t total = flash_list_size / sizeof(romfs_entry);
    for (file->nentry = 0; file->nentry < total; file->nentry++) {
        const romfs_entry *entry = &entries[file->nentry];
        if (entry->name[0] == ROMFS_EMPTY_ENTRY || entry->name[0] == ROMFS_DELETED_ENTRY) {
            continue;
        }
        union {
            attr_by_names names;
            uint16_t raw;
        } attr = {.raw = from_lsb16(entry->attr.raw)};
        if (attr.names.parent != parent_dir_id || (!include_dirs && attr.names.type == ROMFS_TYPE_DIR)) {
            continue;
        }
        /* Listing terminates byte 53 even in legacy entries with no NUL.
         * Preserve that bounded name interpretation before decoding a match. */
        if (strncmp(name, entry->name, ROMFS_MAX_NAME_LEN - 1) == 0 &&
                romfs_list_internal(file, false, false, parent_dir_id, mask) == ROMFS_NOERR) {
            file->nentry--;
            return (file->err = ROMFS_NOERR);
        }
    }

    return (file->err = ROMFS_ERR_NO_ENTRY);
}

static uint32_t romfs_find_entry_internal(uint32_t *entry_index, bool reclaim)
{
    romfs_entry *entries = (romfs_entry *) flash_list_int;

    for (uint32_t i = 0; i < flash_list_size / sizeof(romfs_entry); i++) {
        if (entries[i].name[0] == ROMFS_EMPTY_ENTRY && !romfs_slot_busy(i, NULL, false)) {
            if (entry_index) {
                *entry_index = i;
            }
            return ROMFS_ERR_NO_ENTRY;
        }
    }

    if (reclaim) {
        bool freed;
        uint32_t err = romfs_garbage_collect(&freed);
        if (err != ROMFS_NOERR) {
            return err;
        }
        if (freed) {
            return romfs_find_entry_internal(entry_index, false);
        }
    }

    return ROMFS_ERR_NO_FREE_ENTRIES;
}

static void romfs_store_file_entry(romfs_file *file)
{
    romfs_entry *stored = &((romfs_entry *) flash_list_int)[file->nentry];
    uint16_t attr = to_lsb16(file->entry.attr.raw);
    uint32_t start = to_lsb32(file->entry.start);
    uint32_t size = to_lsb32(file->entry.size);
    if (stored->attr.raw != attr || stored->start != start || stored->size != size ||
            memcmp(stored->name, file->entry.name, ROMFS_MAX_NAME_LEN) != 0) {
        memmove(stored->name, file->entry.name, ROMFS_MAX_NAME_LEN);
        stored->attr.raw = attr;
        stored->start = start;
        stored->size = size;
        romfs_mark_list_entry(file->nentry);
    }
    file->entry_pending = false;
}

static uint32_t romfs_prepare_write_state(romfs_file *file, uint32_t offset)
{
    if (!file || !file->io_buffer) {
        return file ? (file->err = ROMFS_ERR_NO_IO_BUFFER) : ROMFS_ERR_OPERATION;
    }

    file->op = ROMFS_OP_WRITE;
    file->write_offset = offset;
    file->offset = offset % ROMFS_FLASH_SECTOR;
    file->pos = 0xffff;
    file->buffer_base = ROMFS_BUFFER_NONE;
    file->buffer_dirty = false;

    return (file->err = ROMFS_NOERR);
}

uint32_t romfs_list(romfs_file *file, bool first)
{
    return romfs_list_internal(file, first, false, ROMFS_DIR_FILTER_ANY,
                               ROMFS_LIST_INCLUDE_FILES | ROMFS_LIST_INCLUDE_DIRS);
}

/* Call only after validating the complete chain, before changing its links. */
static void romfs_unallocate_sectors_from(uint32_t sector, uint32_t count)
{
    for (uint32_t i = 0; i < count && sector != 0xffff; i++) {
        uint32_t next = from_lsb16(flash_map_int[sector]);
        romfs_store_map_link(sector, 0xffff);
        if (sector < romfs_alloc_hint) {
            romfs_alloc_hint = sector;
        }
        if (next == sector) {
            break;
        }
        sector = next;
    }
}

uint32_t romfs_delete(const char *name)
{
    romfs_dir root;
    uint32_t err = romfs_dir_root(&root);
    if (err != ROMFS_NOERR) {
        return err;
    }
    return romfs_delete_in_dir(&root, name);
}

static uint32_t romfs_garbage_collect(bool *freed)
{
    *freed = false;
    romfs_entry *entries = (romfs_entry *) flash_list_int;

    /* Preflight all candidates so a bad tombstone cannot cause partial GC. */
    for (uint32_t i = 0; i < flash_list_size / sizeof(romfs_entry); i++) {
        if (entries[i].name[0] != ROMFS_DELETED_ENTRY) {
            continue;
        }
        romfs_entry entry = entries[i];
        entry.attr.raw = from_lsb16(entry.attr.raw);
        entry.start = from_lsb32(entry.start);
        entry.size = from_lsb32(entry.size);
        if (romfs_slot_busy(i, NULL, false)) {
            return ROMFS_ERR_BUSY;
        }
        if (!romfs_entry_is_protected(&entry) && entry.attr.names.type != ROMFS_TYPE_DIR &&
                !romfs_validate_chain(&entry, 0, NULL)) {
            return ROMFS_ERR_OPERATION;
        }
    }

    for (uint32_t i = 0; i < flash_list_size / sizeof(romfs_entry); i++) {
        if (entries[i].name[0] == ROMFS_DELETED_ENTRY) {
            romfs_entry entry_copy = entries[i];
            entry_copy.attr.raw = from_lsb16(entry_copy.attr.raw);
            if (romfs_entry_is_protected(&entry_copy)) {
                continue;
            }

            /* Directory IDs were released at deletion, not at GC. */
            if (entry_copy.attr.names.type != ROMFS_TYPE_DIR) {
                romfs_unallocate_sectors_from(from_lsb32(entries[i].start),
                                               romfs_sector_count(from_lsb32(entries[i].size)));
            }

            entries[i].name[0] = ROMFS_EMPTY_ENTRY;
            romfs_mark_list_entry(i);
            *freed = true;
        }
    }

    return ROMFS_NOERR;
}

static uint32_t romfs_find_free_sector(uint32_t start, bool reclaim, uint32_t *sector_out)
{
    uint32_t first_data_sector = romfs_first_data_sector();
    uint32_t total_sectors = flash_sector_limit;

    if (first_data_sector >= total_sectors) {
        return ROMFS_ERR_NO_SPACE;
    }

    if (start < first_data_sector || start >= total_sectors) {
        start = first_data_sector;
    }

    for (uint32_t i = start; i < total_sectors; i++) {
        if (flash_map_int[i] == 0xffff) {
            *sector_out = i;
            return ROMFS_NOERR;
        }
    }

    for (uint32_t i = first_data_sector; i < start; i++) {
        if (flash_map_int[i] == 0xffff) {
            *sector_out = i;
            return ROMFS_NOERR;
        }
    }

    if (reclaim) {
        bool freed;
        uint32_t err = romfs_garbage_collect(&freed);
        if (err != ROMFS_NOERR) {
            return err;
        }
        if (freed) {
            return romfs_find_free_sector(start, false, sector_out);
        }
    }

    return ROMFS_ERR_NO_SPACE;
}

static uint32_t romfs_allocate_sector_after(romfs_file *file, uint32_t prev_sector, uint32_t *sector_out)
{
    if (prev_sector != 0xffff &&
            (prev_sector < romfs_first_data_sector() || prev_sector >= flash_sector_limit)) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    uint32_t hint = (prev_sector == 0xffff) ? romfs_alloc_hint : prev_sector;
    uint32_t sector;
    uint32_t err = romfs_find_free_sector(hint, true, &sector);
    if (err != ROMFS_NOERR) {
        return (file->err = err);
    }

    /* Reserve in RAM so other writers cannot claim this sector. The caller
     * initializes its buffer and accepts data before returning; shared sync
     * flushes every changed writer before publishing the map and catalog. */
    if (prev_sector == 0xffff) {
        file->entry.start = sector;
    } else {
        romfs_store_map_link(prev_sector, sector);
    }
    romfs_store_map_link(sector, sector);
    romfs_alloc_hint = sector + 1 < flash_sector_limit ? sector + 1 : flash_data_start;

    if (sector_out) {
        *sector_out = sector;
    }
    return (file->err = ROMFS_NOERR);
}

static uint32_t romfs_sector_at_index(romfs_file *file, uint32_t sector_index, romfs_write_cursor *cursor,
                                      uint32_t *sector_out)
{
    if (!file || !cursor || !sector_out || sector_index < cursor->index) {
        return file ? (file->err = ROMFS_ERR_OPERATION) : ROMFS_ERR_OPERATION;
    }

    uint32_t sector = cursor->sector;
    uint32_t first_data_sector = romfs_first_data_sector();
    if (sector == 0xffff) {
        if (sector_index != 0) {
            return (file->err = ROMFS_ERR_OPERATION);
        }
        uint32_t err = romfs_allocate_sector_after(file, 0xffff, &sector);
        if (err != ROMFS_NOERR) {
            return err;
        }
    }

    if (sector >= flash_sector_limit || sector < first_data_sector) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    for (uint32_t i = cursor->index; i < sector_index; i++) {
        uint32_t next = from_lsb16(flash_map_int[sector]);
        if (next >= flash_sector_limit || next < first_data_sector) {
            return (file->err = ROMFS_ERR_OPERATION);
        }
        if (next == sector) {
            uint32_t err = romfs_allocate_sector_after(file, sector, &next);
            if (err != ROMFS_NOERR) {
                return err;
            }
        }
        sector = next;
    }

    *cursor = (romfs_write_cursor) {sector_index, sector};
    *sector_out = sector;
    return (file->err = ROMFS_NOERR);
}

uint32_t romfs_create_file(const char *name, romfs_file *file, uint16_t mode, uint16_t type, uint8_t *io_buffer)
{
    romfs_dir root;
    uint32_t err = romfs_dir_root(&root);
    if (err != ROMFS_NOERR) {
        return file ? (file->err = err) : err;
    }
    return romfs_create_file_in_dir(&root, name, file, mode, type, io_buffer);
}

static uint32_t romfs_flush_write_buffer(romfs_file *file)
{
    if (!file->buffer_dirty) {
        return (file->err = ROMFS_NOERR);
    }

    if (file->buffer_base == ROMFS_BUFFER_NONE || file->pos < flash_data_start ||
            file->pos >= flash_sector_limit) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    if (!romfs_flash_sector_erase(file->pos * ROMFS_FLASH_SECTOR) ||
            !romfs_flash_sector_write(file->pos * ROMFS_FLASH_SECTOR, file->io_buffer)) {
        return (file->err = ROMFS_ERR_IO);
    }
    file->buffer_dirty = false;
    return (file->err = ROMFS_NOERR);
}

static uint32_t romfs_load_write_buffer(romfs_file *file, uint32_t logical_offset, romfs_write_cursor *cursor)
{
    uint32_t sector_base = logical_offset & ~(ROMFS_FLASH_SECTOR - 1);
    if (file->buffer_base == sector_base) {
        return (file->err = ROMFS_NOERR);
    }

    uint32_t err = romfs_flush_write_buffer(file);
    if (err != ROMFS_NOERR) {
        return err;
    }

    /* Loading another sector may overwrite the buffer even on a failed read. */
    file->buffer_base = ROMFS_BUFFER_NONE;
    file->pos = 0xffff;
    uint32_t sector = 0xffff;
    uint32_t sector_index = sector_base / ROMFS_FLASH_SECTOR;
    bool existing_sector = sector_base < file->entry.size;
    err = romfs_sector_at_index(file, sector_index, cursor, &sector);
    if (err != ROMFS_NOERR) {
        return err;
    }

    if (existing_sector) {
        if (!romfs_flash_sector_read(sector * ROMFS_FLASH_SECTOR, file->io_buffer, ROMFS_FLASH_SECTOR)) {
            return (file->err = ROMFS_ERR_IO);
        }
    } else {
        /* New/reclaimed sectors may still contain an old file on flash. */
        memset(file->io_buffer, 0, ROMFS_FLASH_SECTOR);
    }

    file->pos = sector;
    file->buffer_base = sector_base;
    file->buffer_dirty = false;
    return (file->err = ROMFS_NOERR);
}

static uint32_t romfs_write_file_no_gap(const void *buffer, uint32_t size, romfs_file *file, romfs_write_cursor *cursor)
{
    const uint8_t *src = (const uint8_t *) buffer;
    uint32_t remaining = size;
    uint32_t total = 0;

    while (remaining > 0) {
        uint32_t err = romfs_load_write_buffer(file, file->write_offset, cursor);
        if (err != ROMFS_NOERR) {
            return total;
        }

        uint32_t within = file->write_offset % ROMFS_FLASH_SECTOR;
        uint32_t space = ROMFS_FLASH_SECTOR - within;
        uint32_t chunk = remaining < space ? remaining : space;

        memmove(&file->io_buffer[within], src, chunk);
        file->buffer_dirty = true;
        file->write_offset += chunk;
        file->offset = file->write_offset % ROMFS_FLASH_SECTOR;
        if (file->write_offset > file->entry.size) {
            file->entry.size = file->write_offset;
        }

        src += chunk;
        remaining -= chunk;
        total += chunk;

        if (within + chunk == ROMFS_FLASH_SECTOR) {
            err = romfs_flush_write_buffer(file);
            if (err != ROMFS_NOERR) {
                return total;
            }
            file->buffer_base = ROMFS_BUFFER_NONE;
            file->pos = 0xffff;
        }
    }

    return total;
}

static uint32_t romfs_zero_extend_to(romfs_file *file, uint32_t size, romfs_write_cursor *cursor)
{
    uint8_t zeros[256] = {0};

    while (file->entry.size < size) {
        file->write_offset = file->entry.size;
        uint32_t remaining = size - file->entry.size;
        uint32_t chunk = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
        if (romfs_write_file_no_gap(zeros, chunk, file, cursor) != chunk || file->err != ROMFS_NOERR) {
            return file->err;
        }
    }

    return (file->err = ROMFS_NOERR);
}

uint32_t romfs_write_file(const void *buffer, uint32_t size, romfs_file *file)
{
    if (!file) {
        return 0;
    }
    if (!romfs_file_is_open(file) || file->op != ROMFS_OP_WRITE || !file->io_buffer || (size && !buffer)) {
        file->err = ROMFS_ERR_OPERATION;
        return 0;
    }
    romfs_write_cursor cursor = {.index = file->write_offset / ROMFS_FLASH_SECTOR};
    if (!romfs_validate_write_file(file, &cursor)) {
        file->err = ROMFS_ERR_OPERATION;
        return 0;
    }
    if (romfs_slot_busy(file->nentry, file, false)) {
        file->err = ROMFS_ERR_BUSY;
        return 0;
    }
    if (size > UINT32_MAX - file->write_offset) {
        file->err = ROMFS_ERR_FILE_DATA_TOO_BIG;
        return 0;
    }

    file->err = ROMFS_NOERR;

    if (size == 0) {
        return 0;
    }

    uint32_t target = file->write_offset;
    if (target > file->entry.size) {
        uint32_t err = romfs_zero_extend_to(file, target, &cursor);
        file->write_offset = target;
        file->offset = target % ROMFS_FLASH_SECTOR;
        if (err != ROMFS_NOERR) {
            return 0;
        }
    }

    return romfs_write_file_no_gap(buffer, size, file, &cursor);
}

static uint32_t romfs_sync_write_file(romfs_file *file)
{
    if (!romfs_file_is_open(file) || file->nentry >= flash_list_size / sizeof(romfs_entry)) {
        return (file->err = ROMFS_ERR_OPERATION);
    }
    if (romfs_slot_busy(file->nentry, file, false)) {
        return (file->err = ROMFS_ERR_BUSY);
    }
    if (file->err == ROMFS_ERR_BUSY || file->err == ROMFS_ERR_NO_SPACE || file->err == ROMFS_ERR_IO) {
        file->err = ROMFS_NOERR;
    }
    if (file->err != ROMFS_NOERR) {
        return file->err;
    }
    if (!romfs_validate_write_file(file, NULL)) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    uint32_t err = romfs_flush_write_buffer(file);
    if (err != ROMFS_NOERR) {
        return err;
    }

    romfs_store_file_entry(file);
    return ROMFS_NOERR;
}

uint32_t romfs_flush_file(romfs_file *file)
{
    if (!file || !romfs_file_is_open(file)) {
        return ROMFS_ERR_OPERATION;
    }

    if (file->op == ROMFS_OP_WRITE) {
        uint32_t status = ROMFS_NOERR;
        romfs_operation_enter();

        status = romfs_sync_write_file(file);
        return (file->err = romfs_operation_leave(status));
    }

    return ROMFS_NOERR;
}

/* Close consumes the descriptor even on failure. Reclaim unpublished tail
 * allocations before its buffer disappears; published metadata stays retryable
 * through romfs_sync(). This cannot undo an in-place physical write failure. */
static void romfs_discard_unpublished_tail(romfs_file *file)
{
    if (!romfs_validate_write_file(file, NULL) || file->nentry >= flash_list_size / sizeof(romfs_entry)) {
        return;
    }
    uint32_t count = romfs_sector_count(file->entry.size);
    uint32_t keep = 0;
    if (!file->entry_pending) {
        romfs_entry published = ((romfs_entry *) flash_list_int)[file->nentry];
        published.start = from_lsb32(published.start);
        published.size = from_lsb32(published.size);
        keep = romfs_sector_count(published.size);
        if (published.name[0] == ROMFS_EMPTY_ENTRY || published.name[0] == ROMFS_DELETED_ENTRY ||
                keep > count || (keep && published.start != file->entry.start)) {
            return;
        }
    }
    if (keep == count) {
        return;
    }
    uint32_t first_free = file->entry.start;
    if (keep) {
        uint32_t last_kept;
        if (!romfs_validate_chain(&file->entry, keep - 1, &last_kept)) {
            return;
        }
        first_free = from_lsb16(flash_map_int[last_kept]);
        romfs_store_map_link(last_kept, last_kept);
    }
    romfs_unallocate_sectors_from(first_free, count - keep);
}

uint32_t romfs_close_file(romfs_file *file)
{
    if (!file) {
        return ROMFS_ERR_OPERATION;
    }
    if (!romfs_file_is_open(file)) {
        return ROMFS_NOERR;
    }
    uint32_t err = romfs_flush_file(file);
    if (err != ROMFS_NOERR && file->op == ROMFS_OP_WRITE) {
        romfs_discard_unpublished_tail(file);
    }
    romfs_unregister_file(file);
    return err;
}

uint32_t romfs_truncate_file(romfs_file *file, uint32_t size)
{
    if (!file || !romfs_file_is_open(file) || file->op != ROMFS_OP_WRITE) {
        return file ? (file->err = ROMFS_ERR_OPERATION) : ROMFS_ERR_OPERATION;
    }

    if (!file->io_buffer) {
        return (file->err = ROMFS_ERR_NO_IO_BUFFER);
    }
    romfs_write_cursor cursor = {.index = file->entry.size / ROMFS_FLASH_SECTOR};
    if (!romfs_validate_write_file(file, &cursor)) {
        return (file->err = ROMFS_ERR_OPERATION);
    }
    if (romfs_slot_busy(file->nentry, file, false)) {
        return (file->err = ROMFS_ERR_BUSY);
    }

    uint32_t status = ROMFS_NOERR;
    romfs_operation_enter();

    uint32_t saved_offset = file->write_offset;
    status = romfs_sync_write_file(file);
    if (status != ROMFS_NOERR) {
        goto out;
    }

    uint32_t old_size = file->entry.size;
    if (size == old_size) {
        goto out_store;
    }

    if (size > old_size) {
        file->write_offset = old_size;
        status = romfs_zero_extend_to(file, size, &cursor);
        if (status != ROMFS_NOERR) {
            goto out;
        }
        status = romfs_sync_write_file(file);
        goto out_store;
    }

    uint32_t old_sectors = romfs_sector_count(old_size);
    uint32_t keep_sectors = romfs_sector_count(size);

    if (keep_sectors == 0) {
        romfs_unallocate_sectors_from(file->entry.start, old_sectors);
        file->entry.start = 0xffff;
    } else {
        uint32_t sector = file->entry.start;
        uint32_t last_kept = sector;
        for (uint32_t i = 1; i < keep_sectors; i++) {
            uint32_t next = from_lsb16(flash_map_int[sector]);
            if (next == sector) {
                status = ROMFS_ERR_OPERATION;
                goto out;
            }
            sector = next;
            last_kept = sector;
        }

        uint32_t tail = size % ROMFS_FLASH_SECTOR;
        if (tail != 0) {
            file->buffer_base = ROMFS_BUFFER_NONE;
            file->pos = 0xffff;
            if (!romfs_flash_sector_read(last_kept * ROMFS_FLASH_SECTOR, file->io_buffer, ROMFS_FLASH_SECTOR)) {
                status = ROMFS_ERR_IO;
                goto out;
            }
            memset(&file->io_buffer[tail], 0, ROMFS_FLASH_SECTOR - tail);
            file->buffer_base = size - tail;
            file->pos = last_kept;
            file->buffer_dirty = true;
            status = romfs_flush_write_buffer(file);
            if (status != ROMFS_NOERR) {
                goto out;
            }
        }
        if (keep_sectors < old_sectors) {
            uint32_t first_free = from_lsb16(flash_map_int[last_kept]);
            romfs_store_map_link(last_kept, last_kept);
            romfs_unallocate_sectors_from(first_free, old_sectors - keep_sectors);
        }
    }

    file->entry.size = size;
    file->buffer_base = ROMFS_BUFFER_NONE;
    file->pos = 0xffff;
    file->buffer_dirty = false;

out_store:
    if (status == ROMFS_NOERR) {
        file->write_offset = saved_offset;
        file->offset = file->write_offset % ROMFS_FLASH_SECTOR;
        romfs_store_file_entry(file);
    }

out:
    file->write_offset = saved_offset;
    file->offset = saved_offset % ROMFS_FLASH_SECTOR;
    return (file->err = romfs_operation_leave(status));
}

uint32_t romfs_open_file(const char *name, romfs_file *file, uint8_t *io_buffer)
{
    romfs_dir root;
    uint32_t err = romfs_dir_root(&root);
    if (err != ROMFS_NOERR) {
        return file ? (file->err = err) : err;
    }
    return romfs_open_file_in_dir(&root, name, file, io_buffer);
}

uint32_t romfs_read_map_table(uint16_t *map_buffer, uint32_t map_size, romfs_file *file)
{
    if (!file) {
        return 0;
    }
    if (!romfs_file_is_open(file) || file->op != ROMFS_OP_READ || (map_size && !map_buffer) ||
            map_size > UINT32_MAX / sizeof(uint16_t) ||
            !romfs_validate_chain(&file->entry, 0, NULL)) {
        file->err = ROMFS_ERR_OPERATION;
        return 0;
    }

    uint32_t num_sectors = romfs_sector_count(file->entry.size);
    if (num_sectors > map_size) {
        file->err = ROMFS_ERR_BUFFER_TOO_SMALL;
        return 0;
    }
    file->err = ROMFS_NOERR;
    if (map_size) {
        memset(map_buffer, 0, map_size * sizeof(uint16_t));
    }
    uint32_t sector = file->entry.start;
    for (uint32_t i = 0; i < num_sectors; i++) {
        uint32_t next = from_lsb16(flash_map_int[sector]);
        map_buffer[i] = sector;
        sector = next;
    }

    return num_sectors;
}

uint32_t romfs_read_file(void *buffer, uint32_t size, romfs_file *file)
{
    if (!file) {
        return 0;
    }
    if (!romfs_file_is_open(file) || file->op != ROMFS_OP_READ || (size && !buffer)) {
        file->err = ROMFS_ERR_OPERATION;
        return 0;
    }

    file->err = ROMFS_NOERR;

    if (size == 0) {
        file->err = ROMFS_NOERR;

        return 0;
    }

    uint32_t sector;
    bool at_end = file->read_offset >= file->entry.size;
    if (!romfs_validate_chain(&file->entry, file->read_offset / ROMFS_FLASH_SECTOR,
                               at_end ? NULL : &sector) || file->read_offset > file->entry.size) {
        file->err = ROMFS_ERR_OPERATION;
        return 0;
    }
    if (at_end) {
        file->err = ROMFS_ERR_EOF;
        return 0;
    }

    uint32_t available = file->entry.size - file->read_offset;
    uint32_t readable = size < available ? size : available;
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t total_read = 0;
    file->pos = sector;
    file->offset = file->read_offset % ROMFS_FLASH_SECTOR;

    while (readable > 0) {
        uint32_t space = ROMFS_FLASH_SECTOR - file->offset;
        uint32_t chunk = readable < space ? readable : space;

        if (!romfs_flash_sector_read(file->pos * ROMFS_FLASH_SECTOR + file->offset, &dst[total_read], chunk)) {
            file->err = ROMFS_ERR_IO;
            return total_read;
        }

        file->offset += chunk;
        file->read_offset += chunk;
        total_read += chunk;
        readable -= chunk;

        if (file->offset == ROMFS_FLASH_SECTOR) {
            uint32_t current = file->pos;
            file->offset = 0;
            if (file->read_offset < file->entry.size) {
                file->pos = from_lsb16(flash_map_int[current]);
            }
        }
    }

    if (file->read_offset >= file->entry.size) {
        file->err = ROMFS_ERR_EOF;
    }

    return total_read;
}

uint32_t romfs_tell_file(romfs_file *file, uint32_t *position)
{
    if (!file || !position || !romfs_file_is_open(file)) {
        if (file) {
            file->err = ROMFS_ERR_OPERATION;
        }
        return ROMFS_ERR_OPERATION;
    }

    uint32_t pos = 0;
    if (file->op == ROMFS_OP_WRITE) {
        pos = file->write_offset;
    } else {
        pos = file->read_offset;
    }

    *position = pos;
    file->err = ROMFS_NOERR;
    return ROMFS_NOERR;
}

uint32_t romfs_seek_file(romfs_file *file, int32_t offset, int whence)
{
    if (!file) {
        return ROMFS_ERR_OPERATION;
    }
    if (!romfs_file_is_open(file)) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    int64_t target64 = 0;
    int64_t current = (file->op == ROMFS_OP_WRITE) ? (int64_t) file->write_offset : (int64_t) file->read_offset;

    switch (whence) {
    case SEEK_SET:
        target64 = offset;
        break;
    case SEEK_CUR:
        target64 = current + offset;
        break;
    case SEEK_END:
        target64 = (int64_t) file->entry.size + offset;
        break;
    default:
        return (file->err = ROMFS_ERR_OPERATION);
    }

    if (target64 < 0) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    if ((uint64_t) target64 > UINT32_MAX) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    uint32_t target = (uint32_t) target64;

    if (file->op == ROMFS_OP_WRITE) {
        if (!romfs_validate_write_file(file, NULL)) {
            return (file->err = ROMFS_ERR_OPERATION);
        }
        file->write_offset = target;
        file->offset = target % ROMFS_FLASH_SECTOR;
        return (file->err = ROMFS_NOERR);
    }

    if (file->op != ROMFS_OP_READ || (uint64_t) target > file->entry.size) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    if (file->entry.size == 0) {
        if (!romfs_validate_chain(&file->entry, 0, NULL)) {
            return (file->err = ROMFS_ERR_OPERATION);
        }
        file->read_offset = 0;
        file->offset = 0;
        file->pos = file->entry.start;
        return (file->err = ROMFS_NOERR);
    }

    uint32_t total_sectors = romfs_sector_count(file->entry.size);
    uint32_t sector_index = 0;
    uint32_t within = 0;
    uint32_t remainder = file->entry.size % ROMFS_FLASH_SECTOR;

    if (target == file->entry.size && remainder == 0) {
        sector_index = (total_sectors > 0) ? (total_sectors - 1) : 0;
        within = 0;
    } else {
        sector_index = target / ROMFS_FLASH_SECTOR;
        within = target % ROMFS_FLASH_SECTOR;
    }

    uint32_t sector;
    if (!romfs_validate_chain(&file->entry, sector_index, &sector)) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    file->pos = sector;
    file->offset = within;
    file->read_offset = target;
    file->err = ROMFS_NOERR;
    return ROMFS_NOERR;
}

static uint32_t romfs_resolve_parent(const char *path, bool create_dirs, romfs_dir *parent_dir, char *leaf,
                                     size_t leaf_len)
{
    if (!path || !parent_dir || !leaf || leaf_len == 0) {
        return ROMFS_ERR_DIR_INVALID;
    }

    /* Validate every component before create_dirs can change the filesystem. */
    const char *part = path;
    while (*part == '/') {
        part++;
    }
    for (const char *end = part; *part; end++) {
        if (*end == '/' || *end == '\0') {
            if (!romfs_valid_entry_name(part, (size_t) (end - part))) {
                return ROMFS_ERR_FILE_DATA_TOO_BIG;
            }
            if (*end == '\0') {
                break;
            }
            part = end + 1;
            if (*part == '\0') {
                return ROMFS_ERR_FILE_DATA_TOO_BIG;
            }
        }
    }

    romfs_dir current;
    uint32_t err = romfs_dir_root(&current);
    if (err != ROMFS_NOERR) {
        return err;
    }

    const char *p = path;
    while (*p == '/') {
        p++;
    }

    if (*p == '\0') {
        return ROMFS_ERR_NO_ENTRY;
    }

    const char *segment_start = p;
    char segment[ROMFS_MAX_NAME_LEN];

    while (*p != '\0') {
        if (*p == '/') {
            size_t len = (size_t)(p - segment_start);
            if (len == 0) {
                return ROMFS_ERR_OPERATION;
            }
            if (len >= sizeof(segment)) {
                return ROMFS_ERR_FILE_DATA_TOO_BIG;
            }
            memcpy(segment, segment_start, len);
            segment[len] = '\0';

            if ((len == 1 && segment[0] == '.') ||
                    (len == 2 && segment[0] == '.' && segment[1] == '.')) {
                return ROMFS_ERR_DIR_INVALID;
            }

            romfs_dir next;
            uint32_t open_err = romfs_dir_open(&current, segment, &next);
            if (open_err != ROMFS_NOERR) {
                if (create_dirs) {
                    open_err = romfs_dir_create(&current, segment, &next);
                }
            }
            if (open_err != ROMFS_NOERR) {
                return open_err;
            }

            current = next;
            p++;
            segment_start = p;
            continue;
        }
        p++;
    }

    size_t len = (size_t)(p - segment_start);
    if (len == 0 || len >= leaf_len || len >= ROMFS_MAX_NAME_LEN) {
        return ROMFS_ERR_FILE_DATA_TOO_BIG;
    }

    if ((len == 1 && segment_start[0] == '.') ||
            (len == 2 && segment_start[0] == '.' && segment_start[1] == '.')) {
        return ROMFS_ERR_DIR_INVALID;
    }

    memcpy(leaf, segment_start, len);
    leaf[len] = '\0';
    *parent_dir = current;
    return ROMFS_NOERR;
}

uint32_t romfs_dir_root(romfs_dir *dir)
{
    if (!romfs_ready || !dir) {
        return ROMFS_ERR_DIR_INVALID;
    }

    dir->id = ROMFS_ROOT_DIR_ID;
    dir->entry_index = ROMFS_INVALID_ENTRY_ID;
    dir->generation = romfs_dir_generation[ROMFS_ROOT_DIR_ID];
    return ROMFS_NOERR;
}

uint32_t romfs_dir_open(const romfs_dir *parent, const char *name, romfs_dir *out)
{
    if (!parent || !out || !name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_valid(parent)) {
        return ROMFS_ERR_DIR_INVALID;
    }

    size_t name_len = strlen(name);
    if (!romfs_valid_entry_name(name, name_len)) {
        return ROMFS_ERR_FILE_DATA_TOO_BIG;
    }

    romfs_file file = {0};
    uint32_t res = romfs_find_file_internal(&file, name, parent->id, true);
    if (res != ROMFS_NOERR) {
        return res;
    }

    if (file.entry.attr.names.type != ROMFS_TYPE_DIR) {
        return ROMFS_ERR_DIR_INVALID;
    }

    uint8_t id = file.entry.attr.names.current;
    if (id == ROMFS_ROOT_DIR_ID || !romfs_dir_id_valid(id) || romfs_dir_entry_index[id] != file.nentry) {
        return ROMFS_ERR_DIR_INVALID;
    }
    out->id = id;
    out->entry_index = file.nentry;
    out->generation = romfs_dir_generation[id];

    return ROMFS_NOERR;
}

uint32_t romfs_dir_create(const romfs_dir *parent, const char *name, romfs_dir *out)
{
    if (!parent || !name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_valid(parent)) {
        return ROMFS_ERR_DIR_INVALID;
    }

    size_t name_len = strlen(name);
    if (!romfs_valid_entry_name(name, name_len)) {
        return ROMFS_ERR_FILE_DATA_TOO_BIG;
    }
    if (romfs_name_pending(parent->id, name)) {
        return ROMFS_ERR_FILE_EXISTS;
    }

    romfs_file file = {0};
    uint32_t res = romfs_find_file_internal(&file, name, parent->id, true);
    if (res == ROMFS_NOERR) {
        if (file.entry.attr.names.type != ROMFS_TYPE_DIR) {
            return ROMFS_ERR_FILE_EXISTS;
        }
        romfs_dir found;
        res = romfs_dir_open(parent, name, out ? out : &found);
        return res == ROMFS_NOERR ? romfs_sync() : res;
    } else if (res != ROMFS_ERR_NO_ENTRY) {
        return res;
    }

    int new_id = romfs_dir_alloc_id();
    if (new_id < 0) {
        return ROMFS_ERR_DIR_LIMIT;
    }

    uint32_t entry_index = 0;
    uint32_t entry_res = romfs_find_entry_internal(&entry_index, true);
    if (entry_res != ROMFS_ERR_NO_ENTRY) {
        romfs_dir_release_id((uint8_t) new_id);
        return entry_res;
    }

    romfs_operation_enter();
    romfs_entry *entries = (romfs_entry *) flash_list_int;
    romfs_entry *slot = &entries[entry_index];
    memset(slot, 0xff, sizeof(*slot));
    memset(slot->name, 0, ROMFS_MAX_NAME_LEN);
    memcpy(slot->name, name, name_len);
    slot->name[name_len] = '\0';

    union {
        attr_by_names names;
        uint16_t raw;
    } attr_union = {0};
    attr_union.names.mode = ROMFS_MODE_READWRITE;
    attr_union.names.type = ROMFS_TYPE_DIR;
    attr_union.names.parent = parent->id;
    attr_union.names.current = (uint16_t) new_id;
    slot->attr.raw = to_lsb16(attr_union.raw);
    slot->start = to_lsb32(0);
    slot->size = to_lsb32(0);

    romfs_dir_entry_index[new_id] = entry_index;
    romfs_mark_list_entry(entry_index);

    if (out) {
        out->id = (uint8_t) new_id;
        out->entry_index = entry_index;
        out->generation = romfs_dir_generation[new_id];
    }

    return romfs_operation_leave(ROMFS_NOERR);
}

uint32_t romfs_dir_remove(const romfs_dir *dir)
{
    if (!dir || dir->id == ROMFS_ROOT_DIR_ID) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_valid(dir)) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_is_empty_internal(dir->id)) {
        return ROMFS_ERR_DIR_NOT_EMPTY;
    }

    uint32_t entry_index = (dir->entry_index != ROMFS_INVALID_ENTRY_ID) ? dir->entry_index : romfs_dir_entry_index[dir->id];
    if (entry_index == ROMFS_INVALID_ENTRY_ID) {
        return ROMFS_ERR_NO_ENTRY;
    }

    romfs_operation_enter();
    romfs_entry *entries = (romfs_entry *) flash_list_int;
    entries[entry_index].name[0] = ROMFS_DELETED_ENTRY;

    romfs_dir_release_id(dir->id);

    romfs_mark_list_entry(entry_index);
    return romfs_operation_leave(ROMFS_NOERR);
}

uint32_t romfs_list_dir(romfs_file *entry, bool first, const romfs_dir *dir, bool include_dirs)
{
    if (!dir || !romfs_dir_valid(dir)) {
        return ROMFS_ERR_DIR_INVALID;
    }

    uint8_t mask = ROMFS_LIST_INCLUDE_FILES;
    if (include_dirs) {
        mask |= ROMFS_LIST_INCLUDE_DIRS;
    }

    return romfs_list_internal(entry, first, false, dir->id, mask);
}

uint32_t romfs_create_file_in_dir(const romfs_dir *dir, const char *name, romfs_file *file, uint16_t mode,
                                  uint16_t type, uint8_t *io_buffer)
{
    if (romfs_file_is_open(file)) {
        return ROMFS_ERR_BUSY;
    }
    if (!file || !dir || !name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!io_buffer) {
        return (file->err = ROMFS_ERR_NO_IO_BUFFER);
    }

    if (!romfs_dir_valid(dir)) {
        return (file->err = ROMFS_ERR_DIR_INVALID);
    }

    size_t name_len = strlen(name);
    if (!romfs_valid_entry_name(name, name_len)) {
        return (file->err = ROMFS_ERR_FILE_DATA_TOO_BIG);
    }

    if (romfs_type_is_service(type) || (type & 0x1f) == ROMFS_TYPE_DIR ||
            (mode & (ROMFS_MODE_SYSTEM | ROMFS_MODE_RESERVED)) != 0) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    file->op = ROMFS_OP_WRITE;

    if (romfs_name_pending(dir->id, name)) {
        return (file->err = ROMFS_ERR_FILE_EXISTS);
    }
    uint32_t res = romfs_find_file_internal(file, name, dir->id, true);
    if (res == ROMFS_NOERR) {
        return (file->err = ROMFS_ERR_FILE_EXISTS);
    } else if (res != ROMFS_ERR_NO_ENTRY) {
        return (file->err = res);
    }

    uint32_t entry_index = 0;
    uint32_t entry_res = romfs_find_entry_internal(&entry_index, true);
    if (entry_res != ROMFS_ERR_NO_ENTRY) {
        return (file->err = entry_res);
    }

    memset(file->entry.name, 0, ROMFS_MAX_NAME_LEN);
    memcpy(file->entry.name, name, name_len);
    file->entry.name[name_len] = '\0';
    file->entry.attr.names.mode = mode & ROMFS_MODE_MASK;
    file->entry.attr.names.type = type & 0x1f;
    file->entry.attr.names.parent = dir->id;
    file->entry.attr.names.current = 0;
    file->entry.size = 0;
    file->entry.start = 0xffff;
    file->offset = 0;
    file->pos = 0xffff;
    file->read_offset = 0;
    file->io_buffer = io_buffer;
    file->nentry = entry_index;
    file->parent_dir_id = dir->id;
    file->dir_id = 0;
    file->buffer_base = ROMFS_BUFFER_NONE;
    file->write_offset = 0;
    file->buffer_dirty = false;
    file->entry_pending = true;
    romfs_register_file(file);

    return (file->err = ROMFS_NOERR);
}

uint32_t romfs_open_file_in_dir(const romfs_dir *dir, const char *name, romfs_file *file, uint8_t *io_buffer)
{
    if (romfs_file_is_open(file)) {
        return ROMFS_ERR_BUSY;
    }
    if (!file || !dir || !name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_valid(dir)) {
        return (file->err = ROMFS_ERR_DIR_INVALID);
    }

    file->op = ROMFS_OP_READ;
    if (romfs_name_pending(dir->id, name)) {
        return (file->err = ROMFS_ERR_BUSY);
    }
    uint32_t res = romfs_find_file_internal(file, name, dir->id, false);
    if (res == ROMFS_NOERR) {
        if (!romfs_validate_chain(&file->entry, 0, NULL)) {
            return (file->err = ROMFS_ERR_OPERATION);
        }
        if (romfs_slot_busy(file->nentry, NULL, true)) {
            return (file->err = ROMFS_ERR_BUSY);
        }
        file->pos = file->entry.start;
        file->offset = 0;
        file->read_offset = 0;
        file->io_buffer = io_buffer;
        file->buffer_base = ROMFS_BUFFER_NONE;
        file->write_offset = 0;
        file->buffer_dirty = false;
        file->entry_pending = false;
        romfs_register_file(file);
        return file->err;
    }

    return (file->err = res);
}

uint32_t romfs_open_read_view(romfs_file *writer, romfs_file *reader, uint8_t *io_buffer)
{
    if (romfs_file_is_open(reader)) {
        return ROMFS_ERR_BUSY;
    }
    if (!reader || !romfs_file_is_open(writer) || writer->op != ROMFS_OP_WRITE) {
        return ROMFS_ERR_OPERATION;
    }
    uint32_t err = romfs_flush_file(writer);
    if (err != ROMFS_NOERR) {
        return (reader->err = err);
    }
    *reader = (romfs_file) {
        .op = ROMFS_OP_READ,
        .entry = writer->entry,
        .nentry = writer->nentry,
        .pos = writer->entry.start,
        .io_buffer = io_buffer,
        .parent_dir_id = writer->parent_dir_id,
        .buffer_base = ROMFS_BUFFER_NONE,
    };
    romfs_register_file(reader);
    return ROMFS_NOERR;
}

static uint32_t romfs_open_write_in_dir(const romfs_dir *dir, const char *name, romfs_file *file,
                                       uint8_t *io_buffer, bool append)
{
    if (romfs_file_is_open(file)) {
        return ROMFS_ERR_BUSY;
    }
    if (!file || !dir || !name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!io_buffer) {
        return (file->err = ROMFS_ERR_NO_IO_BUFFER);
    }

    if (!romfs_dir_valid(dir)) {
        return (file->err = ROMFS_ERR_DIR_INVALID);
    }

    file->op = ROMFS_OP_WRITE;
    file->io_buffer = io_buffer;
    file->read_offset = 0;
    file->buffer_base = ROMFS_BUFFER_NONE;
    file->buffer_dirty = false;

    if (romfs_name_pending(dir->id, name)) {
        return (file->err = ROMFS_ERR_BUSY);
    }
    uint32_t res = romfs_find_file_internal(file, name, dir->id, true);
    if (res == ROMFS_NOERR) {
        if (file->entry.attr.names.type == ROMFS_TYPE_DIR) {
            return (file->err = ROMFS_ERR_OPERATION);
        }
        if (romfs_entry_is_protected(&file->entry) ||
                (file->entry.attr.names.mode & ROMFS_MODE_READONLY) != 0) {
            return (file->err = ROMFS_ERR_OPERATION);
        }
        if (!romfs_validate_chain(&file->entry, 0, NULL)) {
            return (file->err = ROMFS_ERR_OPERATION);
        }

        if (romfs_slot_busy(file->nentry, NULL, false)) {
            return (file->err = ROMFS_ERR_BUSY);
        }
        res = romfs_prepare_write_state(file, append ? file->entry.size : 0);
        if (res == ROMFS_NOERR) {
            file->entry_pending = false;
            romfs_register_file(file);
        }
        return res;
    }

    return (file->err = res);
}

uint32_t romfs_open_append_in_dir(const romfs_dir *dir, const char *name, romfs_file *file, uint16_t type,
                                  uint8_t *io_buffer)
{
    uint32_t err = romfs_open_write_in_dir(dir, name, file, io_buffer, true);
    if (err == ROMFS_ERR_NO_ENTRY) {
        return romfs_create_file_in_dir(dir, name, file, ROMFS_MODE_READWRITE, type, io_buffer);
    }
    return err;
}

uint32_t romfs_open_append(const char *name, romfs_file *file, uint16_t type, uint8_t *io_buffer)
{
    romfs_dir root;
    uint32_t err = romfs_dir_root(&root);
    if (err != ROMFS_NOERR) {
        if (file) {
            file->err = err;
        }
        return err;
    }
    return romfs_open_append_in_dir(&root, name, file, type, io_buffer);
}

uint32_t romfs_delete_in_dir(const romfs_dir *dir, const char *name)
{
    if (!dir || !name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_valid(dir)) {
        return ROMFS_ERR_DIR_INVALID;
    }
    if (romfs_name_pending(dir->id, name)) {
        return ROMFS_ERR_BUSY;
    }

    romfs_file file = {0};
    uint32_t res = romfs_find_file_internal(&file, name, dir->id, true);
    if (res != ROMFS_NOERR) {
        return res;
    }

    if (romfs_entry_is_protected(&file.entry)) {
        return ROMFS_ERR_OPERATION;
    }

    if (file.entry.attr.names.type == ROMFS_TYPE_DIR) {
        if (!romfs_dir_is_empty_internal(file.entry.attr.names.current)) {
            return ROMFS_ERR_DIR_NOT_EMPTY;
        }
        romfs_dir_release_id(file.entry.attr.names.current);
    } else if (!romfs_validate_chain(&file.entry, 0, NULL)) {
        return ROMFS_ERR_OPERATION;
    }
    if (romfs_slot_busy(file.nentry, NULL, false)) {
        return ROMFS_ERR_BUSY;
    }

    romfs_operation_enter();
    ((romfs_entry *) flash_list_int)[file.nentry].name[0] = ROMFS_DELETED_ENTRY;
    romfs_mark_list_entry(file.nentry);
    return romfs_operation_leave(ROMFS_NOERR);
}

uint32_t romfs_rename_in_dir(const romfs_dir *src_dir, const char *src_name,
                             const romfs_dir *dst_dir, const char *dst_name)
{
    if (!src_dir || !dst_dir || !src_name || !dst_name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_valid(src_dir) || !romfs_dir_valid(dst_dir)) {
        return ROMFS_ERR_DIR_INVALID;
    }

    size_t src_len = strlen(src_name);
    size_t dst_len = strlen(dst_name);
    if (!romfs_valid_entry_name(src_name, src_len) || !romfs_valid_entry_name(dst_name, dst_len)) {
        return ROMFS_ERR_FILE_DATA_TOO_BIG;
    }

    if (romfs_name_pending(src_dir->id, src_name)) {
        return ROMFS_ERR_BUSY;
    }
    if (romfs_name_pending(dst_dir->id, dst_name)) {
        return ROMFS_ERR_FILE_EXISTS;
    }

    romfs_file src = {0};
    uint32_t res = romfs_find_file_internal(&src, src_name, src_dir->id, true);
    if (res != ROMFS_NOERR) {
        return res;
    }
    if (src_dir->id == dst_dir->id && strcmp(src_name, dst_name) == 0) {
        return ROMFS_NOERR;
    }
    if (romfs_slot_busy(src.nentry, NULL, false)) {
        return ROMFS_ERR_BUSY;
    }

    if (romfs_entry_is_protected(&src.entry)) {
        return ROMFS_ERR_OPERATION;
    }

    romfs_file dst_check = {0};
    res = romfs_find_file_internal(&dst_check, dst_name, dst_dir->id, true);
    if (res == ROMFS_NOERR) {
        return ROMFS_ERR_FILE_EXISTS;
    }
    if (res != ROMFS_ERR_NO_ENTRY) {
        return res;
    }

    bool is_dir = (src.entry.attr.names.type == ROMFS_TYPE_DIR);
    uint8_t moving_dir_id = 0;

    if (is_dir) {
        moving_dir_id = src.entry.attr.names.current;
        for (romfs_file *file = romfs_open_files; file; file = file->next_open) {
            int parent = file->entry.attr.names.parent;
            for (uint32_t depth = 0; parent > ROMFS_ROOT_DIR_ID && depth < ROMFS_MAX_DIRS; depth++) {
                if (parent == moving_dir_id) {
                    return ROMFS_ERR_BUSY;
                }
                parent = romfs_dir_parent_id((uint8_t) parent);
            }
        }
        if (dst_dir->id == moving_dir_id) {
            return ROMFS_ERR_DIR_INVALID;
        }
        uint8_t check = dst_dir->id;
        while (true) {
            if (check == moving_dir_id) {
                return ROMFS_ERR_DIR_INVALID;
            }
            if (check == ROMFS_ROOT_DIR_ID) {
                break;
            }
            int parent = romfs_dir_parent_id(check);
            if (parent < 0) {
                break;
            }
            check = (uint8_t) parent;
        }
    }

    romfs_entry *entries = (romfs_entry *) flash_list_int;
    romfs_entry *entry = &entries[src.nentry];

    romfs_operation_enter();
    memset(entry->name, 0, ROMFS_MAX_NAME_LEN);
    memcpy(entry->name, dst_name, dst_len);

    union {
        attr_by_names names;
        uint16_t raw;
    } attr_union = {0};
    attr_union.raw = from_lsb16(entry->attr.raw);
    attr_union.names.parent = dst_dir->id;
    entry->attr.raw = to_lsb16(attr_union.raw);

    romfs_mark_list_entry(src.nentry);
    return romfs_operation_leave(ROMFS_NOERR);
}

uint32_t romfs_rename(const char *src_name, const char *dst_name)
{
    romfs_dir root;
    uint32_t err = romfs_dir_root(&root);
    if (err != ROMFS_NOERR) {
        return err;
    }
    return romfs_rename_in_dir(&root, src_name, &root, dst_name);
}

uint32_t romfs_open_path(const char *path, romfs_file *file, uint8_t *io_buffer)
{
    if (romfs_file_is_open(file)) {
        return ROMFS_ERR_BUSY;
    }
    char leaf[ROMFS_MAX_NAME_LEN];
    romfs_dir parent;
    uint32_t err = romfs_resolve_parent(path, false, &parent, leaf, sizeof(leaf));
    if (err != ROMFS_NOERR) {
        if (file) {
            file->err = err;
        }
        return err;
    }

    return romfs_open_file_in_dir(&parent, leaf, file, io_buffer);
}

uint32_t romfs_rename_path(const char *src_path, const char *dst_path, bool create_dirs)
{
    if (!src_path || !dst_path) {
        return ROMFS_ERR_DIR_INVALID;
    }

    char src_leaf[ROMFS_MAX_NAME_LEN];
    romfs_dir src_parent;
    uint32_t err = romfs_resolve_parent(src_path, false, &src_parent, src_leaf, sizeof(src_leaf));
    if (err != ROMFS_NOERR) {
        return err;
    }

    char dst_leaf[ROMFS_MAX_NAME_LEN];
    romfs_dir dst_parent;
    err = romfs_resolve_parent(dst_path, create_dirs, &dst_parent, dst_leaf, sizeof(dst_leaf));
    if (err != ROMFS_NOERR) {
        return err;
    }

    return romfs_rename_in_dir(&src_parent, src_leaf, &dst_parent, dst_leaf);
}

uint32_t romfs_open_write_path(const char *path, romfs_file *file, uint8_t *io_buffer)
{
    if (romfs_file_is_open(file)) {
        return ROMFS_ERR_BUSY;
    }
    romfs_dir parent;
    char leaf[ROMFS_MAX_NAME_LEN];
    uint32_t err = romfs_resolve_parent(path, false, &parent, leaf, sizeof(leaf));
    if (err != ROMFS_NOERR) {
        if (file) {
            file->err = err;
        }
        return err;
    }
    return romfs_open_write_in_dir(&parent, leaf, file, io_buffer, false);
}

uint32_t romfs_open_append_path(const char *path, romfs_file *file, uint16_t type, uint8_t *io_buffer, bool create_dirs)
{
    if (romfs_file_is_open(file)) {
        return ROMFS_ERR_BUSY;
    }
    char leaf[ROMFS_MAX_NAME_LEN];
    romfs_dir parent;
    uint32_t err = romfs_resolve_parent(path, create_dirs, &parent, leaf, sizeof(leaf));
    if (err != ROMFS_NOERR) {
        if (file) {
            file->err = err;
        }
        return err;
    }

    return romfs_open_append_in_dir(&parent, leaf, file, type, io_buffer);
}

uint32_t romfs_create_path(const char *path, romfs_file *file, uint16_t mode, uint16_t type, uint8_t *io_buffer,
                           bool create_dirs)
{
    if (romfs_file_is_open(file)) {
        return ROMFS_ERR_BUSY;
    }
    char leaf[ROMFS_MAX_NAME_LEN];
    romfs_dir parent;
    uint32_t err = romfs_resolve_parent(path, create_dirs, &parent, leaf, sizeof(leaf));
    if (err != ROMFS_NOERR) {
        if (file) {
            file->err = err;
        }
        return err;
    }

    return romfs_create_file_in_dir(&parent, leaf, file, mode, type, io_buffer);
}

uint32_t romfs_get_entry_in_dir(const romfs_dir *dir, const char *name, romfs_entry *out_entry)
{
    if (!dir || !name || !out_entry) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_valid(dir)) {
        return ROMFS_ERR_DIR_INVALID;
    }

    size_t name_len = strlen(name);
    if (!romfs_valid_entry_name(name, name_len)) {
        return ROMFS_ERR_FILE_DATA_TOO_BIG;
    }

    romfs_file file = {0};
    uint32_t res = romfs_find_file_internal(&file, name, dir->id, true);
    if (res != ROMFS_NOERR) {
        return res;
    }

    *out_entry = file.entry;
    return ROMFS_NOERR;
}

uint32_t romfs_get_entry(const char *name, romfs_entry *out_entry)
{
    if (!name || !out_entry) {
        return ROMFS_ERR_DIR_INVALID;
    }

    romfs_dir root;
    uint32_t err = romfs_dir_root(&root);
    if (err != ROMFS_NOERR) {
        return err;
    }

    return romfs_get_entry_in_dir(&root, name, out_entry);
}

uint32_t romfs_get_entry_path(const char *path, romfs_entry *out_entry)
{
    if (!romfs_ready || !path || !out_entry) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (path[0] == '/' && path[1] == '\0') {
        memset(out_entry, 0, sizeof(*out_entry));
        out_entry->name[0] = '/';
        out_entry->name[1] = '\0';
        out_entry->attr.names.mode = ROMFS_MODE_READWRITE;
        out_entry->attr.names.type = ROMFS_TYPE_DIR;
        out_entry->attr.names.parent = ROMFS_ROOT_DIR_ID;
        out_entry->attr.names.current = ROMFS_ROOT_DIR_ID;
        out_entry->start = 0;
        out_entry->size = 0;
        return ROMFS_NOERR;
    }

    char leaf[ROMFS_MAX_NAME_LEN];
    romfs_dir parent;
    uint32_t err = romfs_resolve_parent(path, false, &parent, leaf, sizeof(leaf));
    if (err != ROMFS_NOERR) {
        return err;
    }

    return romfs_get_entry_in_dir(&parent, leaf, out_entry);
}

uint32_t romfs_mkdir_path(const char *path, bool create_parents, romfs_dir *out_dir)
{
    char leaf[ROMFS_MAX_NAME_LEN];
    romfs_dir parent;
    uint32_t err = romfs_resolve_parent(path, create_parents, &parent, leaf, sizeof(leaf));
    if (err != ROMFS_NOERR) {
        return err;
    }

    return romfs_dir_create(&parent, leaf, out_dir);
}

uint32_t romfs_rmdir_path(const char *path)
{
    char leaf[ROMFS_MAX_NAME_LEN];
    romfs_dir parent;
    uint32_t err = romfs_resolve_parent(path, false, &parent, leaf, sizeof(leaf));
    if (err != ROMFS_NOERR) {
        return err;
    }

    romfs_dir target;
    err = romfs_dir_open(&parent, leaf, &target);
    if (err != ROMFS_NOERR) {
        return err;
    }

    return romfs_dir_remove(&target);
}

uint32_t romfs_dir_open_path(const char *path, romfs_dir *out_dir)
{
    char leaf[ROMFS_MAX_NAME_LEN];
    romfs_dir parent;
    uint32_t err = romfs_resolve_parent(path, false, &parent, leaf, sizeof(leaf));
    if (err != ROMFS_NOERR) {
        return err;
    }

    return romfs_dir_open(&parent, leaf, out_dir);
}

uint32_t romfs_delete_path(const char *path)
{
    char leaf[ROMFS_MAX_NAME_LEN];
    romfs_dir parent;
    uint32_t err = romfs_resolve_parent(path, false, &parent, leaf, sizeof(leaf));
    if (err != ROMFS_NOERR) {
        return err;
    }

    return romfs_delete_in_dir(&parent, leaf);
}

const char *romfs_strerror(uint32_t err)
{
    if (err < sizeof(romfs_errlist) / sizeof(const char *)) {
        return romfs_errlist[err];
    }

    return "Unknown";
}
