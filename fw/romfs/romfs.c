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
};

static uint32_t flash_start = 0;
static uint32_t mem_size = 0;
static uint32_t flash_map_size = 0;
static uint32_t flash_list_size = 0;

static uint16_t *flash_map_int;
static uint8_t *flash_list_int;

static bool romfs_garbage_collect(void);
#define ROMFS_DIR_FILTER_ANY 0xff
#define ROMFS_LIST_INCLUDE_FILES 0x01
#define ROMFS_LIST_INCLUDE_DIRS 0x02

static uint16_t romfs_dir_entry_index[ROMFS_MAX_DIRS];
static uint16_t romfs_dir_used_mask = (1u << ROMFS_ROOT_DIR_ID);

static void romfs_dir_index_reset(void);
static void romfs_dir_index_rebuild(void);
static int romfs_dir_alloc_id(void);
static void romfs_dir_release_id(uint8_t id);
static bool romfs_dir_id_valid(uint8_t id);
static bool romfs_dir_is_empty_internal(uint8_t dir_id);
static uint32_t romfs_resolve_parent(const char *path, bool create_dirs, romfs_dir *parent_dir, char *leaf, size_t leaf_len);
static bool romfs_valid_entry_name(const char *name, size_t len);
static int romfs_dir_parent_id(uint8_t id);
static void romfs_flush(void);
static void romfs_operation_enter(void);
static void romfs_operation_leave(void);
static void romfs_request_flush(void);

static uint32_t romfs_flush_depth;
static bool romfs_flush_pending;

static void romfs_dir_index_reset(void)
{
    for (uint32_t i = 0; i < ROMFS_MAX_DIRS; i++) {
        romfs_dir_entry_index[i] = ROMFS_INVALID_ENTRY_ID;
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

static bool romfs_valid_entry_name(const char *name, size_t len)
{
    if (!name || len == 0 || len >= ROMFS_MAX_NAME_LEN) {
        return false;
    }
    if ((len == 1 && name[0] == '.') ||
            (len == 2 && name[0] == '.' && name[1] == '.')) {
        return false;
    }
    return true;
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

static void romfs_operation_leave(void)
{
    if (romfs_flush_depth == 0) {
        return;
    }
    romfs_flush_depth--;
    if (romfs_flush_depth == 0 && romfs_flush_pending) {
        romfs_flush();
        romfs_flush_pending = false;
    }
}

static void romfs_request_flush(void)
{
    if (romfs_flush_depth == 0) {
        romfs_flush();
    } else {
        romfs_flush_pending = true;
    }
}

static bool romfs_dir_is_empty_internal(uint8_t dir_id)
{
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
    flash_map_size = ((rom_size / ROMFS_FLASH_SECTOR) * sizeof(uint16_t) + (ROMFS_FLASH_SECTOR - 1)) & ~(ROMFS_FLASH_SECTOR - 1);
    if (flash_map_size <  ROMFS_FLASH_SECTOR) {
        flash_map_size = ROMFS_FLASH_SECTOR;
    }

    flash_list_size = ((rom_size / ROMFS_MB) * sizeof(romfs_entry) + (ROMFS_FLASH_SECTOR - 1)) & ~(ROMFS_FLASH_SECTOR - 1);
    if (flash_list_size < ROMFS_FLASH_SECTOR) {
        flash_list_size = ROMFS_FLASH_SECTOR;
    }

    if (map_size) {
        *map_size = flash_map_size;
    }

    if (list_size) {
        *list_size = flash_list_size;
    }
}

bool romfs_start(uint32_t start, uint32_t rom_size, uint16_t *flash_map, uint8_t *flash_list)
{
    flash_start = (start + 0x7fff) & ~0x7fff;
    mem_size = rom_size;

    flash_map_int = flash_map;
    flash_list_int = flash_list;

    //    printf("romfs memory size %d\n", mem_size);
    //    printf("romfs map size %d\n", flash_map_size);
    //    printf("romfs list size %d\n", flash_list_size);

    romfs_dir_index_reset();

    if (flash_map_size && flash_list_size) {
        for (uint32_t i = 0; i < flash_list_size; i += ROMFS_FLASH_SECTOR) {
            romfs_flash_sector_read(flash_start + i, &flash_list_int[i], ROMFS_FLASH_SECTOR);
        }
        for (uint32_t i = 0; i < flash_map_size; i += ROMFS_FLASH_SECTOR) {
            romfs_flash_sector_read(flash_start + flash_list_size + i, &((uint8_t *) flash_map_int)[i], ROMFS_FLASH_SECTOR);
        }
        romfs_dir_index_rebuild();
        return true;
    }

    return false;
}

static void romfs_flush(void)
{
    for (uint32_t i = 0; i < flash_list_size; i += ROMFS_FLASH_SECTOR) {
        romfs_flash_sector_erase(flash_start + i);
        romfs_flash_sector_write(flash_start + i, &flash_list_int[i]);
    }

    for (uint32_t i = 0; i < flash_map_size; i += ROMFS_FLASH_SECTOR) {
        romfs_flash_sector_erase(flash_start + flash_list_size + i);
        romfs_flash_sector_write(flash_start + flash_list_size + i, &((uint8_t *) flash_map_int)[i]);
    }
}

bool romfs_format(void)
{
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

    for (uint32_t i = 0; i < (flash_start + flash_list_size + flash_map_size) / ROMFS_FLASH_SECTOR; i++) {
        flash_map_int[i] = to_lsb16(i + 1);
    }

    romfs_request_flush();
    romfs_operation_leave();
    romfs_dir_index_rebuild();

    return true;
}

uint32_t romfs_free(void)
{
    uint32_t free_sectors = 0;
    for (uint32_t i = 0; i < flash_map_size / sizeof(uint16_t); i++) {
        if (flash_map_int[i] == 0xffff) {
            free_sectors++;
        }
    }

    romfs_entry *entries = (romfs_entry *) flash_list_int;
    for (uint32_t i = 0; i < flash_list_size / sizeof(romfs_entry); i++) {
        if (entries[i].name[0] == ROMFS_DELETED_ENTRY) {
            uint32_t file_size = from_lsb32(entries[i].size);
            free_sectors += (file_size + (ROMFS_FLASH_SECTOR - 1)) / ROMFS_FLASH_SECTOR;
        }
    }

    return free_sectors * ROMFS_FLASH_SECTOR;
}

static uint32_t romfs_list_internal(romfs_file *file, bool first, bool with_deleted, uint8_t parent_filter, uint8_t include_mask)
{
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
        file->buffer_base = 0;
        file->buffer_from_flash = false;

        return (file->err = ROMFS_NOERR);
    }

    return (file->err = ROMFS_ERR_NO_FREE_ENTRIES);
}

static uint32_t romfs_find_file_internal(romfs_file *file, const char *name, uint8_t parent_dir_id, bool include_dirs)
{
    uint8_t mask = ROMFS_LIST_INCLUDE_FILES;
    if (include_dirs) {
        mask |= ROMFS_LIST_INCLUDE_DIRS;
    }

    if (romfs_list_internal(file, true, false, parent_dir_id, mask) == ROMFS_NOERR) {
        do {
            if (!strncmp(name, file->entry.name, ROMFS_MAX_NAME_LEN)) {
                file->nentry--;
                return (file->err = ROMFS_NOERR);
            }
        } while (romfs_list_internal(file, false, false, parent_dir_id, mask) == ROMFS_NOERR);
    }

    file->err = ROMFS_ERR_NO_ENTRY;
    return file->err;
}

static uint32_t romfs_find_entry_internal(uint32_t *entry_index, bool reclaim)
{
    romfs_entry *entries = (romfs_entry *) flash_list_int;

    for (uint32_t i = 0; i < flash_list_size / sizeof(romfs_entry); i++) {
        if (entries[i].name[0] == ROMFS_EMPTY_ENTRY) {
            if (entry_index) {
                *entry_index = i;
            }
            return ROMFS_ERR_NO_ENTRY;
        }
    }

    if (reclaim && romfs_garbage_collect()) {
        return romfs_find_entry_internal(entry_index, false);
    }

    return ROMFS_ERR_NO_FREE_ENTRIES;
}

static uint32_t romfs_last_sector(uint32_t start)
{
    if (start == 0xffff) {
        return 0xffff;
    }

    uint32_t sector = start;
    while (true) {
        uint32_t next = from_lsb16(flash_map_int[sector]);
        if (next == sector) {
            break;
        }
        sector = next;
    }
    return sector;
}

uint32_t romfs_list(romfs_file *file, bool first)
{
    return romfs_list_internal(file, first, false, ROMFS_DIR_FILTER_ANY, ROMFS_LIST_INCLUDE_FILES | ROMFS_LIST_INCLUDE_DIRS);
}

static void romfs_unallocate_sectors_chain(romfs_file *file)
{
    if (file->entry.start == 0xffff) {
        return;
    }

    uint32_t sectors = (file->entry.size + (ROMFS_FLASH_SECTOR - 1)) / ROMFS_FLASH_SECTOR;

    uint32_t sector = file->entry.start;
    for (uint32_t i = 0; i < sectors; i++) {
        uint32_t next = from_lsb16(flash_map_int[sector]);
        flash_map_int[sector] = 0xffff;
        sector = next;
    }
}

uint32_t romfs_delete(const char *name)
{
    romfs_dir root;
    romfs_dir_root(&root);
    return romfs_delete_in_dir(&root, name);
}

static bool romfs_garbage_collect(void)
{
    bool freed = false;
    romfs_entry *entries = (romfs_entry *) flash_list_int;

    for (uint32_t i = 0; i < flash_list_size / sizeof(romfs_entry); i++) {
        if (entries[i].name[0] == ROMFS_DELETED_ENTRY) {
            romfs_entry entry_copy = entries[i];
            entry_copy.attr.raw = from_lsb16(entry_copy.attr.raw);

            if (entry_copy.attr.names.type == ROMFS_TYPE_DIR) {
                romfs_dir_release_id(entry_copy.attr.names.current);
            } else {
                romfs_file file = {
                    .entry = {
                        .attr.raw = entry_copy.attr.raw,
                        .start = from_lsb32(entries[i].start),
                        .size = from_lsb32(entries[i].size),
                    }
                };
                romfs_unallocate_sectors_chain(&file);
            }

            entries[i].name[0] = ROMFS_EMPTY_ENTRY;
            freed = true;
        }
    }

    return freed;
}

static uint32_t romfs_find_free_sector(uint32_t start, bool reclaim)
{
    for (uint32_t i = start; i < flash_map_size / sizeof(uint16_t); i++) {
        if (flash_map_int[i] == 0xffff) {
            return i;
        }
    }

    for (uint32_t i = 0; i < start; i++) {
        if (flash_map_int[i] == 0xffff) {
            return i;
        }
    }

    if (reclaim && romfs_garbage_collect()) {
        return romfs_find_free_sector(start, false);
    }

    return 0xffff;
}

uint32_t romfs_create_file(const char *name, romfs_file *file, uint16_t mode, uint16_t type, uint8_t *io_buffer)
{
    romfs_dir root;
    romfs_dir_root(&root);
    return romfs_create_file_in_dir(&root, name, file, mode, type, io_buffer);
}

static uint32_t romfs_allocate_and_write_sector_internal(const void *buffer, romfs_file *file)
{
    if (file->entry.start == 0xffff) {
        file->entry.start = romfs_find_free_sector(0, true);
        if (file->entry.start == 0xffff) {
            return (file->err = ROMFS_ERR_NO_SPACE);
        }
        file->pos = file->entry.start;
        flash_map_int[file->pos] = to_lsb16(file->pos);
    } else {
        uint32_t pos = romfs_find_free_sector(file->pos, true);
        if (pos == 0xffff) {
            romfs_unallocate_sectors_chain(file);
            file->entry.start = 0xffff;
            file->entry.size = 0;
            file->pos = 0xffff;
            return (file->err = ROMFS_ERR_NO_SPACE);
        }
        flash_map_int[file->pos] = to_lsb16(pos);
        flash_map_int[pos] = to_lsb16(pos);
        file->pos = pos;
    }

    romfs_flash_sector_erase(file->pos * ROMFS_FLASH_SECTOR);
    romfs_flash_sector_write(file->pos * ROMFS_FLASH_SECTOR, (uint8_t *) buffer);

    return (file->err = ROMFS_NOERR);
}

uint32_t romfs_write_file(const void *buffer, uint32_t size, romfs_file *file)
{
    if (file->op == ROMFS_OP_READ) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    file->err = ROMFS_NOERR;

    if (size == 0) {
        return 0;
    }

    const uint8_t *src = (const uint8_t *) buffer;
    uint32_t remaining = size;

    while (remaining > 0) {
        if (file->offset == 0 &&
                file->buffer_base == 0 &&
                !file->buffer_from_flash &&
                remaining >= ROMFS_FLASH_SECTOR) {
            if (romfs_allocate_and_write_sector_internal(src, file) != ROMFS_NOERR) {
                return 0;
            }
            file->entry.size += ROMFS_FLASH_SECTOR;
            src += ROMFS_FLASH_SECTOR;
            remaining -= ROMFS_FLASH_SECTOR;
            continue;
        }

        uint32_t space = ROMFS_FLASH_SECTOR - file->offset;
        uint32_t chunk = remaining < space ? remaining : space;
        uint32_t prev_offset = file->offset;
        bool prev_buffer_from_flash = file->buffer_from_flash;

        memmove(&file->io_buffer[file->offset], src, chunk);
        file->offset += chunk;
        src += chunk;
        remaining -= chunk;

        if (file->offset == ROMFS_FLASH_SECTOR) {
            if (prev_buffer_from_flash) {
                uint32_t new_bytes = ROMFS_FLASH_SECTOR - file->buffer_base;
                uint32_t sector = file->pos;
                romfs_flash_sector_erase(sector * ROMFS_FLASH_SECTOR);
                romfs_flash_sector_write(sector * ROMFS_FLASH_SECTOR, file->io_buffer);
                file->entry.size += new_bytes;
                file->buffer_from_flash = false;
                file->buffer_base = 0;
                file->offset = 0;
            } else {
                if (romfs_allocate_and_write_sector_internal(file->io_buffer, file) != ROMFS_NOERR) {
                    file->offset = prev_offset;
                    return 0;
                }
                file->entry.size += ROMFS_FLASH_SECTOR;
                file->buffer_base = 0;
                file->offset = 0;
            }
        }
    }

    return size;
}

uint32_t romfs_close_file(romfs_file *file)
{
    if (file->op == ROMFS_OP_WRITE) {
        uint32_t status = ROMFS_NOERR;
        romfs_operation_enter();

        if (file->err != ROMFS_NOERR) {
            status = file->err;
            romfs_operation_leave();
            return status;
        }

        if (file->offset > file->buffer_base) {
            uint32_t pending = file->offset - file->buffer_base;
            if (file->buffer_from_flash) {
                uint32_t sector = file->pos;
                romfs_flash_sector_erase(sector * ROMFS_FLASH_SECTOR);
                romfs_flash_sector_write(sector * ROMFS_FLASH_SECTOR, file->io_buffer);
                file->buffer_from_flash = false;
            } else {
                if (romfs_allocate_and_write_sector_internal(file->io_buffer, file) != ROMFS_NOERR) {
                    status = file->err;
                    romfs_operation_leave();
                    return status;
                }
            }
            file->entry.size += pending;
            file->offset = 0;
            file->buffer_base = 0;
        }

        romfs_entry *_entry = &((romfs_entry *) flash_list_int)[file->nentry];
        memmove(_entry->name, file->entry.name, ROMFS_MAX_NAME_LEN);
        _entry->attr.raw = to_lsb16(file->entry.attr.raw);
        _entry->start = to_lsb32(file->entry.start);
        _entry->size = to_lsb32(file->entry.size);

        romfs_request_flush();
        romfs_operation_leave();

        return status;
    }

    return ROMFS_NOERR;
}

uint32_t romfs_open_file(const char *name, romfs_file *file, uint8_t *io_buffer)
{
    romfs_dir root;
    romfs_dir_root(&root);
    return romfs_open_file_in_dir(&root, name, file, io_buffer);
}

uint32_t romfs_read_map_table(uint16_t *map_buffer, uint32_t map_size, romfs_file *file)
{
    if (file->op == ROMFS_OP_WRITE) {
        file->err = ROMFS_ERR_OPERATION;
        return 0;
    }

    file->err = ROMFS_NOERR;

    memset(map_buffer, 0, map_size * sizeof(uint16_t));

    if (file->entry.size == 0) {
        file->err = ROMFS_NOERR;

        return 0;
    }

    uint32_t sector = file->entry.start;
    uint32_t num_sectors = (file->entry.size + (ROMFS_FLASH_SECTOR - 1)) / ROMFS_FLASH_SECTOR;
    if (num_sectors > map_size) {
        file->err = ROMFS_ERR_BUFFER_TOO_SMALL;
        return 0;
    }
    for (uint32_t i = 0; i < num_sectors; i++) {
        uint32_t next = from_lsb16(flash_map_int[sector]);
        map_buffer[i] = sector;
        sector = next;
    }

    return num_sectors;
}

uint32_t romfs_read_file(void *buffer, uint32_t size, romfs_file *file)
{
    if (file->op == ROMFS_OP_WRITE) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    file->err = ROMFS_NOERR;

    if (size == 0) {
        file->err = ROMFS_NOERR;

        return 0;
    }

    if (file->read_offset >= file->entry.size) {
        file->err = ROMFS_ERR_EOF;
        return 0;
    }

    uint32_t readable = (file->read_offset + size > file->entry.size) ?
                (file->entry.size - file->read_offset) : size;
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t total_read = 0;

    while (readable > 0) {
        uint32_t space = ROMFS_FLASH_SECTOR - file->offset;
        uint32_t chunk = readable < space ? readable : space;

        romfs_flash_sector_read(file->pos * ROMFS_FLASH_SECTOR + file->offset, &dst[total_read], chunk);

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
    if (!file || !position) {
        if (file) {
            file->err = ROMFS_ERR_OPERATION;
        }
        return ROMFS_ERR_OPERATION;
    }

    uint32_t pos = 0;
    if (file->op == ROMFS_OP_WRITE) {
        uint32_t buffered = (file->offset > file->buffer_base) ? (file->offset - file->buffer_base) : 0;
        pos = file->entry.size + buffered;
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

    if (file->op != ROMFS_OP_READ) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    int64_t target64 = 0;

    switch (whence) {
    case SEEK_SET:
        target64 = offset;
        break;
    case SEEK_CUR:
        target64 = (int64_t) file->read_offset + offset;
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

    if ((uint64_t) target64 > file->entry.size) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    uint32_t target = (uint32_t) target64;

    if (file->entry.size == 0) {
        file->read_offset = 0;
        file->offset = 0;
        file->pos = file->entry.start;
        return (file->err = ROMFS_NOERR);
    }

    uint32_t total_sectors = (file->entry.size + (ROMFS_FLASH_SECTOR - 1)) / ROMFS_FLASH_SECTOR;
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

    uint32_t sector = file->entry.start;
    if (sector == 0xffff && file->entry.size > 0) {
        return (file->err = ROMFS_ERR_OPERATION);
    }

    for (uint32_t i = 0; i < sector_index; i++) {
        uint32_t next = from_lsb16(flash_map_int[sector]);
        if (next == sector) {
            return (file->err = ROMFS_ERR_OPERATION);
        }
        sector = next;
    }

    file->pos = sector;
    file->offset = within;
    file->read_offset = target;
    file->err = ROMFS_NOERR;
    return ROMFS_NOERR;
}

static uint32_t romfs_resolve_parent(const char *path, bool create_dirs, romfs_dir *parent_dir, char *leaf, size_t leaf_len)
{
    if (!path || !parent_dir || !leaf || leaf_len == 0) {
        return ROMFS_ERR_DIR_INVALID;
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
    if (!dir) {
        return ROMFS_ERR_DIR_INVALID;
    }

    dir->id = ROMFS_ROOT_DIR_ID;
    dir->entry_index = ROMFS_INVALID_ENTRY_ID;
    return ROMFS_NOERR;
}

uint32_t romfs_dir_open(const romfs_dir *parent, const char *name, romfs_dir *out)
{
    if (!parent || !out || !name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_id_valid(parent->id)) {
        return ROMFS_ERR_DIR_INVALID;
    }

    size_t name_len = strlen(name);
    if (name_len == 0 || name_len >= ROMFS_MAX_NAME_LEN) {
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

    out->id = file.entry.attr.names.current;
    out->entry_index = file.nentry;
    if (out->id < ROMFS_MAX_DIRS) {
        romfs_dir_entry_index[out->id] = file.nentry;
        romfs_dir_used_mask |= (1u << out->id);
    }

    return ROMFS_NOERR;
}

uint32_t romfs_dir_create(const romfs_dir *parent, const char *name, romfs_dir *out)
{
    if (!parent || !name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_id_valid(parent->id)) {
        return ROMFS_ERR_DIR_INVALID;
    }

    size_t name_len = strlen(name);
    if (name_len == 0 || name_len >= ROMFS_MAX_NAME_LEN) {
        return ROMFS_ERR_FILE_DATA_TOO_BIG;
    }

    romfs_file file = {0};
    uint32_t res = romfs_find_file_internal(&file, name, parent->id, true);
    if (res == ROMFS_NOERR) {
        if (file.entry.attr.names.type != ROMFS_TYPE_DIR) {
            return ROMFS_ERR_FILE_EXISTS;
        }
        if (out) {
            out->id = file.entry.attr.names.current;
            out->entry_index = file.nentry;
        }
        return ROMFS_NOERR;
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
    romfs_request_flush();
    romfs_operation_leave();

    if (out) {
        out->id = (uint8_t) new_id;
        out->entry_index = entry_index;
    }

    return ROMFS_NOERR;
}

uint32_t romfs_dir_remove(const romfs_dir *dir)
{
    if (!dir || dir->id == ROMFS_ROOT_DIR_ID) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_id_valid(dir->id)) {
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

    romfs_request_flush();
    romfs_operation_leave();

    return ROMFS_NOERR;
}

uint32_t romfs_list_dir(romfs_file *entry, bool first, const romfs_dir *dir, bool include_dirs)
{
    if (!dir || !romfs_dir_id_valid(dir->id)) {
        return ROMFS_ERR_DIR_INVALID;
    }

    uint8_t mask = ROMFS_LIST_INCLUDE_FILES;
    if (include_dirs) {
        mask |= ROMFS_LIST_INCLUDE_DIRS;
    }

    return romfs_list_internal(entry, first, false, dir->id, mask);
}

uint32_t romfs_create_file_in_dir(const romfs_dir *dir, const char *name, romfs_file *file, uint16_t mode, uint16_t type, uint8_t *io_buffer)
{
    if (!file || !dir || !name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!io_buffer) {
        return (file->err = ROMFS_ERR_NO_IO_BUFFER);
    }

    if (!romfs_dir_id_valid(dir->id)) {
        return (file->err = ROMFS_ERR_DIR_INVALID);
    }

    size_t name_len = strlen(name);
    if (name_len == 0 || name_len >= ROMFS_MAX_NAME_LEN) {
        return (file->err = ROMFS_ERR_FILE_DATA_TOO_BIG);
    }

    file->op = ROMFS_OP_WRITE;

    uint32_t res = romfs_find_file_internal(file, name, dir->id, false);
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
    file->pos = 0;
    file->read_offset = 0;
    file->io_buffer = io_buffer;
    file->nentry = entry_index;
    file->parent_dir_id = dir->id;
    file->dir_id = 0;
    file->buffer_base = 0;
    file->buffer_from_flash = false;

    return (file->err = ROMFS_NOERR);
}

uint32_t romfs_open_file_in_dir(const romfs_dir *dir, const char *name, romfs_file *file, uint8_t *io_buffer)
{
    if (!file || !dir || !name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!io_buffer) {
        return (file->err = ROMFS_ERR_NO_IO_BUFFER);
    }

    if (!romfs_dir_id_valid(dir->id)) {
        return (file->err = ROMFS_ERR_DIR_INVALID);
    }

    file->op = ROMFS_OP_READ;
    uint32_t res = romfs_find_file_internal(file, name, dir->id, false);
    if (res == ROMFS_NOERR) {
        file->pos = file->entry.start;
        file->offset = 0;
        file->read_offset = 0;
        file->io_buffer = io_buffer;
        file->buffer_base = 0;
        file->buffer_from_flash = false;
        return file->err;
    }

    return (file->err = res);
}

uint32_t romfs_open_append_in_dir(const romfs_dir *dir, const char *name, romfs_file *file, uint16_t type, uint8_t *io_buffer)
{
    if (!file || !dir || !name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!io_buffer) {
        return (file->err = ROMFS_ERR_NO_IO_BUFFER);
    }

    if (!romfs_dir_id_valid(dir->id)) {
        return (file->err = ROMFS_ERR_DIR_INVALID);
    }

    file->op = ROMFS_OP_WRITE;
    file->io_buffer = io_buffer;
    file->read_offset = 0;
    file->buffer_base = 0;
    file->buffer_from_flash = false;

    uint32_t res = romfs_find_file_internal(file, name, dir->id, false);
    if (res == ROMFS_NOERR) {
        if (file->entry.attr.names.type == ROMFS_TYPE_DIR) {
            return (file->err = ROMFS_ERR_OPERATION);
        }

        uint32_t size = file->entry.size;

        if (size == 0 || file->entry.start == 0xffff) {
            file->offset = 0;
            file->buffer_base = 0;
            file->buffer_from_flash = false;
            file->pos = (file->entry.start == 0xffff) ? 0 : romfs_last_sector(file->entry.start);
        } else {
            uint32_t last = romfs_last_sector(file->entry.start);
            file->pos = last;
            uint32_t tail = size % ROMFS_FLASH_SECTOR;
            if (tail == 0) {
                file->offset = 0;
                file->buffer_base = 0;
                file->buffer_from_flash = false;
            } else {
                romfs_flash_sector_read(last * ROMFS_FLASH_SECTOR, io_buffer, ROMFS_FLASH_SECTOR);
                file->offset = tail;
                file->buffer_base = tail;
                file->buffer_from_flash = true;
            }
        }

        return (file->err = ROMFS_NOERR);
    }

    if (res != ROMFS_ERR_NO_ENTRY) {
        return (file->err = res);
    }

    uint32_t create_res = romfs_create_file_in_dir(dir, name, file, ROMFS_MODE_READWRITE, type, io_buffer);
    return create_res;
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

    if (!romfs_dir_id_valid(dir->id)) {
        return ROMFS_ERR_DIR_INVALID;
    }

    romfs_file file = {0};
    uint32_t res = romfs_find_file_internal(&file, name, dir->id, true);
    if (res != ROMFS_NOERR) {
        return res;
    }

    if (file.entry.attr.names.type == ROMFS_TYPE_DIR) {
        if (!romfs_dir_is_empty_internal(file.entry.attr.names.current)) {
            return ROMFS_ERR_DIR_NOT_EMPTY;
        }
        romfs_dir_release_id(file.entry.attr.names.current);
    }

    romfs_operation_enter();
    ((romfs_entry *) flash_list_int)[file.nentry].name[0] = ROMFS_DELETED_ENTRY;
    romfs_request_flush();
    romfs_operation_leave();

    return file.err;
}

uint32_t romfs_rename_in_dir(const romfs_dir *src_dir, const char *src_name,
                             const romfs_dir *dst_dir, const char *dst_name)
{
    if (!src_dir || !dst_dir || !src_name || !dst_name) {
        return ROMFS_ERR_DIR_INVALID;
    }

    if (!romfs_dir_id_valid(src_dir->id) || !romfs_dir_id_valid(dst_dir->id)) {
        return ROMFS_ERR_DIR_INVALID;
    }

    size_t src_len = strlen(src_name);
    size_t dst_len = strlen(dst_name);
    if (!romfs_valid_entry_name(src_name, src_len) || !romfs_valid_entry_name(dst_name, dst_len)) {
        return ROMFS_ERR_FILE_DATA_TOO_BIG;
    }

    if (src_dir->id == dst_dir->id &&
            strncmp(src_name, dst_name, ROMFS_MAX_NAME_LEN) == 0) {
        return ROMFS_NOERR;
    }

    romfs_file src = {0};
    uint32_t res = romfs_find_file_internal(&src, src_name, src_dir->id, true);
    if (res != ROMFS_NOERR) {
        return res;
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

    romfs_request_flush();
    romfs_operation_leave();

    return ROMFS_NOERR;
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

uint32_t romfs_open_append_path(const char *path, romfs_file *file, uint16_t type, uint8_t *io_buffer, bool create_dirs)
{
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

uint32_t romfs_create_path(const char *path, romfs_file *file, uint16_t mode, uint16_t type, uint8_t *io_buffer, bool create_dirs)
{
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

    if (!romfs_dir_id_valid(dir->id)) {
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
    if (!path || !out_entry) {
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
