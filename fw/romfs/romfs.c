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
    "End of file",
    "Operation error",
};

static uint32_t flash_start = 0;
static uint32_t mem_size = 0;
static uint32_t flash_map_size = 0;
static uint32_t flash_list_size = 0;

static uint16_t *flash_map_int;
static uint8_t *flash_list_int;

static bool romfs_garbage_collect(void);

void romfs_get_buffers_sizes(uint32_t rom_size, uint32_t *map_size, uint32_t *list_size)
{
    flash_map_size = ((rom_size / ROMFS_FLASH_SECTOR) * sizeof(uint16_t) + (ROMFS_FLASH_SECTOR - 1)) & ~(ROMFS_FLASH_SECTOR - 1);
    if (flash_map_size <  ROMFS_FLASH_SECTOR) {
        flash_map_size = ROMFS_FLASH_SECTOR;
    }

    flash_list_size = ((rom_size / (1024 * 1024)) * sizeof(romfs_entry) + (ROMFS_FLASH_SECTOR - 1)) & ~(ROMFS_FLASH_SECTOR - 1);
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

    if (flash_map_size && flash_list_size) {
        for (uint32_t i = 0; i < flash_list_size; i += ROMFS_FLASH_SECTOR) {
            romfs_flash_sector_read(flash_start + i, &flash_list_int[i], ROMFS_FLASH_SECTOR);
        }
        for (uint32_t i = 0; i < flash_map_size; i += ROMFS_FLASH_SECTOR) {
            romfs_flash_sector_read(flash_start + flash_list_size + i, &((uint8_t *) flash_map_int)[i], ROMFS_FLASH_SECTOR);
        }
        return true;
    }

    return false;
}

void romfs_flush(void)
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
    memset(flash_list_int, 0xff, flash_list_size);

    romfs_entry *entry = (romfs_entry *) flash_list_int;

    romfs_entry tmp;

    strncpy(entry[0].name, "firmware", ROMFS_MAX_NAME_LEN - 1);
    entry[0].name[ROMFS_MAX_NAME_LEN - 1] = '\0';
    tmp.attr.names.mode = ROMFS_MODE_READONLY | ROMFS_MODE_SYSTEM;
    tmp.attr.names.type = ROMFS_TYPE_FIRMWARE;
    uint16_t raw = (tmp.attr.names.mode & 0x07) | (tmp.attr.names.type << 3);
    entry[0].attr.raw = to_lsb16(raw);
    entry[0].start = to_lsb32(0);
    entry[0].size = to_lsb32(flash_start);

    strncpy(entry[1].name, "flashlist", ROMFS_MAX_NAME_LEN - 1);
    entry[1].name[ROMFS_MAX_NAME_LEN - 1] = '\0';
    tmp.attr.names.mode = ROMFS_MODE_READONLY | ROMFS_MODE_SYSTEM;
    tmp.attr.names.type = ROMFS_TYPE_FLASHLIST;
    raw = (tmp.attr.names.mode & 0x07) | (tmp.attr.names.type << 3);
    entry[1].attr.raw = to_lsb16(raw);
    entry[1].start = to_lsb32(flash_start / ROMFS_FLASH_SECTOR);
    entry[1].size = to_lsb32(flash_list_size);

    strncpy(entry[2].name, "flashmap", ROMFS_MAX_NAME_LEN - 1);
    entry[2].name[ROMFS_MAX_NAME_LEN - 1] = '\0';
    tmp.attr.names.mode = ROMFS_MODE_READONLY | ROMFS_MODE_SYSTEM;
    tmp.attr.names.type = ROMFS_TYPE_FLASHMAP;
    raw = (tmp.attr.names.mode & 0x07) | (tmp.attr.names.type << 3);
    entry[2].attr.raw = to_lsb16(raw);
    entry[2].start = to_lsb32((flash_start + flash_list_size) / ROMFS_FLASH_SECTOR);
    entry[2].size = to_lsb32(flash_map_size);

    memset((uint8_t *) flash_map_int, 0xff, flash_map_size);

    for (uint32_t i = 0; i < (flash_start + flash_list_size + flash_map_size) / ROMFS_FLASH_SECTOR; i++) {
        flash_map_int[i] = to_lsb16(i + 1);
    }

    romfs_flush();

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

static uint32_t romfs_list_internal(romfs_file *file, bool first, bool with_deleted)
{
    if (first) {
        file->nentry = 0;
    }

    while ((file->nentry < flash_list_size / sizeof(romfs_entry)) &&
           ((!with_deleted && ((romfs_entry *) flash_list_int)[file->nentry].name[0] == ROMFS_DELETED_ENTRY) ||
            ((romfs_entry *) flash_list_int)[file->nentry].name[0] == ROMFS_EMPTY_ENTRY)) {
        file->nentry++;
    }

    if (file->nentry >= flash_list_size / sizeof(romfs_entry)) {
        return (file->err = ROMFS_ERR_NO_FREE_ENTRIES);
    }

    romfs_entry *_entry = &((romfs_entry *) flash_list_int)[file->nentry];
    memmove(&file->entry.name, _entry->name, ROMFS_MAX_NAME_LEN);
    uint16_t raw = from_lsb16(_entry->attr.raw);
    file->entry.attr.names.mode = raw & 0x07;
    file->entry.attr.names.type = raw >> 3;
    file->entry.start = from_lsb32(_entry->start);
    file->entry.size = from_lsb32(_entry->size);

    file->nentry++;
    file->pos = 0;
    file->offset = 0;

    return (file->err = ROMFS_NOERR);
}

static uint32_t romfs_find_file_internal(romfs_file *file, const char *name)
{
    if (romfs_list_internal(file, true, false) == ROMFS_NOERR) {
        do {
            if (!strncmp(name, file->entry.name, ROMFS_MAX_NAME_LEN)) {
                file->nentry--;
                return (file->err = ROMFS_NOERR);
            }
        } while (romfs_list_internal(file, false, false) == ROMFS_NOERR);
    }

    return file->err;
}

static uint32_t romfs_find_entry_internal(romfs_file *file, bool reclaim)
{
    romfs_entry *entries = (romfs_entry *) flash_list_int;

    for (file->nentry = 0; file->nentry < flash_list_size / sizeof(romfs_entry); file->nentry++) {
        if (entries[file->nentry].name[0] == ROMFS_EMPTY_ENTRY) {
            return ROMFS_ERR_NO_ENTRY;
        }
    }

    if (reclaim && romfs_garbage_collect()) {
        return romfs_find_entry_internal(file, false);
    }

    return ROMFS_ERR_NO_FREE_ENTRIES;
}

uint32_t romfs_list(romfs_file *file, bool first)
{
    return romfs_list_internal(file, first, false);
}

static void romfs_unallocate_sectors_chain(romfs_file *file)
{
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
    romfs_file file;
    if (romfs_find_file_internal(&file, name) == ROMFS_NOERR) {
        ((romfs_entry *) flash_list_int)[file.nentry].name[0] = ROMFS_DELETED_ENTRY;

        romfs_flush();

        return file.err;
    }

    return (file.err = ROMFS_ERR_NO_ENTRY);
}

static bool romfs_garbage_collect(void)
{
    bool freed = false;
    romfs_file file;
    romfs_entry *entries = (romfs_entry *) flash_list_int;

    for (uint32_t i = 0; i < flash_list_size / sizeof(romfs_entry); i++) {
        if (entries[i].name[0] == ROMFS_DELETED_ENTRY) {
            uint16_t raw = from_lsb16(entries[i].attr.raw);
            file.entry.attr.names.mode = raw & 0x07;
            file.entry.attr.names.type = raw >> 3;
            file.entry.start = from_lsb32(entries[i].start);
            file.entry.size = from_lsb32(entries[i].size);

            romfs_unallocate_sectors_chain(&file);

            entries[i].name[0] = ROMFS_EMPTY_ENTRY;
            freed = true;
            break;
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
    if (!io_buffer) {
        return (file->err = ROMFS_ERR_NO_IO_BUFFER);
    }

    file->op = ROMFS_OP_WRITE;

    if (romfs_find_file_internal(file, name) == ROMFS_NOERR) {
        return (file->err = ROMFS_ERR_FILE_EXISTS);
    }

    if (romfs_find_entry_internal(file, true) == ROMFS_ERR_NO_ENTRY) {
        strncpy(file->entry.name, name, ROMFS_MAX_NAME_LEN - 1);
        file->entry.name[ROMFS_MAX_NAME_LEN - 1] = '\0';
        file->entry.attr.names.mode = mode;
        file->entry.attr.names.type = type;
        file->entry.size = 0;
        file->entry.start = 0xffff;
        file->offset = 0;
        file->io_buffer = io_buffer;
        return (file->err = ROMFS_NOERR);
    }

    return (file->err = ROMFS_ERR_NO_FREE_ENTRIES);
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

    if (size > ROMFS_FLASH_SECTOR) {
        file->err = ROMFS_ERR_FILE_DATA_TOO_BIG;

        return 0;
    }

    uint32_t bytes = size % ROMFS_FLASH_SECTOR;

    if (file->offset + bytes >= ROMFS_FLASH_SECTOR) {
        uint32_t need = ROMFS_FLASH_SECTOR - file->offset;
        memmove(&file->io_buffer[file->offset], (char *)buffer, need);
        if (romfs_allocate_and_write_sector_internal(file->io_buffer, file) != ROMFS_NOERR) {
            return 0;
        }
        file->entry.size += ROMFS_FLASH_SECTOR;
        uint32_t offset = need;
        need = bytes - need;
        if (need > 0) {
            memmove(file->io_buffer, &((char *)buffer)[offset], need);
            file->offset = need;
        } else {
            file->offset = 0;
        }

        return size;
    } else if (bytes > 0) {
        memmove(&file->io_buffer[file->offset], (char *)buffer, bytes);
        file->offset += bytes;

        return size;
    }

    if (romfs_allocate_and_write_sector_internal(buffer, file) != ROMFS_NOERR) {
        return 0;
    }

    file->entry.size += ROMFS_FLASH_SECTOR;

    return size;
}

uint32_t romfs_close_file(romfs_file *file)
{
    if (file->op == ROMFS_OP_WRITE) {
        if (file->offset > 0) {
            if (romfs_allocate_and_write_sector_internal(file->io_buffer, file) != ROMFS_NOERR) {
                return file->err;
            }
            file->entry.size += file->offset;
        }

        romfs_entry *_entry = &((romfs_entry *) flash_list_int)[file->nentry];
        memmove(_entry->name, file->entry.name, ROMFS_MAX_NAME_LEN);
        uint16_t raw = (file->entry.attr.names.mode & 0x07) | (file->entry.attr.names.type << 3);
        _entry->attr.raw = to_lsb16(raw);
        _entry->start = to_lsb32(file->entry.start);
        _entry->size = to_lsb32(file->entry.size);

        romfs_flush();
    }

    return ROMFS_NOERR;
}

uint32_t romfs_open_file(const char *name, romfs_file *file, uint8_t *io_buffer)
{
    if (!io_buffer) {
        return (file->err = ROMFS_ERR_NO_IO_BUFFER);
    }

    file->op = ROMFS_OP_READ;
    if (romfs_find_file_internal(file, name) == ROMFS_NOERR) {
        file->pos = file->entry.start;
        file->offset = 0;
        file->read_offset = 0;
        file->io_buffer = io_buffer;
        return file->err;
    }

    return (file->err = ROMFS_ERR_NO_ENTRY);
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

    if (size > ROMFS_FLASH_SECTOR) {
        file->err = ROMFS_ERR_FILE_DATA_TOO_BIG;

        return 0;
    }

    if (file->read_offset >= file->entry.size) {
        file->err = ROMFS_ERR_EOF;
        return 0;
    }

    size = (file->read_offset + size > file->entry.size) ? (file->entry.size - file->read_offset) : size;

    //uint32_t bytes = size % ROMFS_FLASH_SECTOR;

    if (file->offset + size == ROMFS_FLASH_SECTOR) {
        romfs_flash_sector_read(file->pos * ROMFS_FLASH_SECTOR + file->offset, buffer, size);
        file->read_offset += size;
        file->offset = 0;
        file->pos = from_lsb16(flash_map_int[file->pos]);

        return size;
    } else if (file->offset + size > ROMFS_FLASH_SECTOR) {
        uint32_t need = ROMFS_FLASH_SECTOR - file->offset;
        romfs_flash_sector_read(file->pos * ROMFS_FLASH_SECTOR + file->offset, buffer, need);
        file->pos = from_lsb16(flash_map_int[file->pos]);
        file->read_offset += need;
        need = size - need;
        if (need > 0) {
            romfs_flash_sector_read(file->pos * ROMFS_FLASH_SECTOR, &((uint8_t *) buffer)[ROMFS_FLASH_SECTOR - file->offset], need);
            file->offset = need;
            file->read_offset += need;
        } else {
            file->offset = 0;
        }
        return size;
    } else if (size > 0) {
        romfs_flash_sector_read(file->pos * ROMFS_FLASH_SECTOR + file->offset, buffer, size);
        file->offset += size;
        file->read_offset += size;

        return size;
    }

    return 0;
}

const char *romfs_strerror(uint32_t err)
{
    if (err < sizeof(romfs_errlist) / sizeof(const char *)) {
        return romfs_errlist[err];
    }

    return "Unknown";
}
