#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "romfs.h"

#define DEBUG

#define ROM_SIZE (64 * 1024 * 1024)
#define FLASH_SECTOR (4096)
#define MAP_SIZE (ROM_SIZE / FLASH_SECTOR)
#define LIST_MAX (128)

#define ROMFS_EMPTY_ENTRY   (-1)
#define ROMFS_DELETED_ENTRY (-2)

static const char *romfs_errlist[] = {
    "No error",
    "No list entry",
    "No free list entries",
    "No free space",
    "File exists",
    "File data too long",
    "End of file",
};

static uint8_t *flash_base = NULL;
static uint32_t flash_start = 0;
static uint32_t mem_size = 0;
static uint32_t map_size = 0;
static uint32_t list_size = 0;

static uint8_t flash_buffer[FLASH_SECTOR];
static uint16_t flash_map[FLASH_SECTOR * 4];
static uint8_t flash_list[FLASH_SECTOR];

static bool romfs_flash_sector_erase(uint32_t offset)
{
#ifdef DEBUG
    printf("flash erase %08X\n", offset);
#endif
    memset(&flash_base[offset], 0xff, FLASH_SECTOR);
    return true;
}

static bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
#ifdef DEBUG
    printf("flash write %08X (%p)\n", offset, (void *)buffer);
#endif
    memmove(&flash_base[offset], buffer, FLASH_SECTOR);
    return true;
}

bool romfs_start(uint8_t *base, uint32_t start, uint32_t size)
{
    flash_base = base;
    flash_start = start;
    mem_size = size;

    map_size = ((size / FLASH_SECTOR) * sizeof(uint16_t) + (FLASH_SECTOR - 1)) & ~(FLASH_SECTOR - 1);
    if (map_size > sizeof(flash_map)) {
	map_size = sizeof(flash_map);
    }
    list_size = ((size / (1024 * 1024)) * sizeof(romfs_entry) + (FLASH_SECTOR - 1)) & ~(FLASH_SECTOR - 1);
    if (list_size > sizeof(flash_list)) {
	list_size = sizeof(flash_list);
    }

    printf("romfs memory size %d\n", mem_size);
    printf("romfs map size %d\n", map_size);
    printf("romfs list size %d\n", list_size);

    if (map_size && list_size) {
	for (int i = 0; i < list_size; i += FLASH_SECTOR) {
	    memmove(&flash_list[i], &flash_base[flash_start + i], FLASH_SECTOR);
	}
	for (int i = 0; i < map_size; i += FLASH_SECTOR) {
	    memmove(&((uint8_t *)flash_map)[i], &flash_base[flash_start + list_size + i], FLASH_SECTOR);
	}
	return true;
    }

    return false;
}

void romfs_flush(void)
{
    for (int i = 0; i < list_size; i += FLASH_SECTOR) {
	romfs_flash_sector_erase(flash_start + i);
	romfs_flash_sector_write(flash_start + i, &flash_list[i]);
    }

    for (int i = 0; i < map_size; i += FLASH_SECTOR) {
	romfs_flash_sector_erase(flash_start + list_size + i);
	romfs_flash_sector_write(flash_start + list_size + i, &((uint8_t *)flash_map)[i]);
    }
}

bool romfs_format(void)
{
    memset(flash_list, 0xff, list_size);

    romfs_entry *entry = (romfs_entry *)flash_list;

    strncpy(entry[0].name, "firmware", ROMFS_MAX_NAME_LEN - 1);
    entry[0].name[ROMFS_MAX_NAME_LEN - 1] = '\0';
    entry[0].mode = ROMFS_MODE_READONLY | ROMFS_MODE_SYSTEM;
    entry[0].type = ROMFS_TYPE_FIRMWARE;
    entry[0].start = 0;
    entry[0].size = flash_start;

    strncpy(entry[1].name, "flashlist", ROMFS_MAX_NAME_LEN - 1);
    entry[1].name[ROMFS_MAX_NAME_LEN - 1] = '\0';
    entry[1].mode = ROMFS_MODE_READONLY | ROMFS_MODE_SYSTEM;
    entry[1].type = ROMFS_TYPE_FLASHLIST;
    entry[1].start = flash_start / FLASH_SECTOR;
    entry[1].size = list_size;

    strncpy(entry[2].name, "flashmap", ROMFS_MAX_NAME_LEN - 1);
    entry[2].name[ROMFS_MAX_NAME_LEN - 1] = '\0';
    entry[2].mode = ROMFS_MODE_READONLY | ROMFS_MODE_SYSTEM;
    entry[2].type = ROMFS_TYPE_FLASHMAP;
    entry[2].start = (flash_start + list_size) / FLASH_SECTOR;
    entry[2].size = map_size;

    memset((uint8_t *)flash_map, 0xff, map_size);

    for (int i = 0; i < (flash_start + list_size + map_size) / FLASH_SECTOR; i++) {
	flash_map[i] = i + 1;
    }

    romfs_flush();

    return true;
}

uint32_t romfs_free(void)
{
    return -1;
}

static uint32_t romfs_list_internal(romfs_file *file, bool first, bool with_deleted)
{
    if (first) {
	file->nentry = 0;
    }

    if (!with_deleted) {
	while (((romfs_entry *)flash_list)[file->nentry].name[0] == ROMFS_DELETED_ENTRY) {
	    file->nentry++;
	}
    }

    if (file->nentry > list_size / sizeof(romfs_entry)) {
	return (file->err = ROMFS_ERR_NO_FREE_ENTRIES);
    }

    if (((romfs_entry *)flash_list)[file->nentry].name[0] == ROMFS_EMPTY_ENTRY ||
	((romfs_entry *)flash_list)[file->nentry].name[0] == ROMFS_DELETED_ENTRY) {
	return (file->err = ROMFS_ERR_NO_ENTRY);
    }

    memmove(&file->entry, &((romfs_entry *)flash_list)[file->nentry], sizeof(romfs_entry));

    file->nentry++;
    file->pos = 0;
    file->offset = 0;

    return (file->err = ROMFS_NOERR);
}

static uint32_t romfs_find_internal(romfs_file *file, const char *name)
{
    if (romfs_list(file, true) == ROMFS_NOERR) {
	do {
	    if (!strcmp(name, file->entry.name)) {
		file->nentry--;
		return (file->err = ROMFS_NOERR);
	    }
	} while (romfs_list(file, false) == ROMFS_NOERR);
    }

    return file->err;
}

uint32_t romfs_list(romfs_file *file, bool first)
{
    return romfs_list_internal(file, first, false);
}

static void romfs_unallocate_sectors_chain(uint32_t sector, uint32_t size)
{
    uint32_t sectors = ((size + (FLASH_SECTOR - 1)) & ~(FLASH_SECTOR - 1)) / FLASH_SECTOR;

    for (int i = 0; i < sectors; i++) {
	uint32_t next = flash_map[sector];
	flash_map[sector] = 0xffff;
	sector = next;
    }
}

uint32_t romfs_delete(const char *name)
{
    romfs_file file;
    if (romfs_find_internal(&file, name) == ROMFS_NOERR) {
	((romfs_entry *)flash_list)[file.nentry].name[0] = ROMFS_DELETED_ENTRY;

	romfs_unallocate_sectors_chain(file.entry.start, file.entry.size);

	romfs_flush();
    }

    return file.err;
}

static uint32_t romfs_find_free_sector(uint32_t start)
{
    for (int i = start; i < map_size / sizeof(uint16_t); i++) {
	if (flash_map[i] == 0xffff) {
	    return i;
	}
    }

    for (int i = 0; i < start; i++) {
	if (flash_map[i] == 0xffff) {
	    return i;
	}
    }

    return 0xffff;
}

uint32_t romfs_create_file(char *name, romfs_file *file, uint16_t mode, uint16_t type, uint8_t *io_buffer)
{
    file->op = ROMFS_OP_WRITE;

    if (romfs_find_internal(file, name) == ROMFS_NOERR) {
	return (file->err = ROMFS_ERR_FILE_EXISTS);
    }

    if (file->err == ROMFS_ERR_NO_ENTRY) {
	strncpy(file->entry.name, name, ROMFS_MAX_NAME_LEN - 1);
	file->entry.name[ROMFS_MAX_NAME_LEN - 1] = '\0';
	file->entry.mode = mode;
	file->entry.type = type;
	file->entry.size = 0;
	file->entry.start = 0xffff;
	file->offset = 0;
	file->io_buffer = (io_buffer) ? io_buffer : flash_buffer;

	return (file->err = ROMFS_NOERR);
    }

    return (file->err = ROMFS_ERR_NO_FREE_ENTRIES);
}

static uint32_t romfs_allocate_and_write_sector_internal(void *buffer, romfs_file *file)
{
    if (file->entry.start == 0xffff) {
	file->entry.start = romfs_find_free_sector(0);
	if (file->entry.start == 0xffff) {
	    return (file->err = ROMFS_ERR_NO_SPACE);
	}
	file->pos = file->entry.start;
	flash_map[file->pos] = file->pos;
    } else {
	uint32_t pos = romfs_find_free_sector(file->pos);
	if (pos == 0xffff) {
	    romfs_unallocate_sectors_chain(file->entry.start, file->entry.size);
	    return (file->err = ROMFS_ERR_NO_SPACE);
	}
	flash_map[file->pos] = pos;
	flash_map[pos] = pos;
	file->pos = pos;
    }

    romfs_flash_sector_erase(file->pos * FLASH_SECTOR);
    romfs_flash_sector_write(file->pos * FLASH_SECTOR, (uint8_t *)buffer);
    
    return (file->err = ROMFS_NOERR);
}

uint32_t romfs_write_file(void *buffer, uint32_t size, romfs_file *file)
{
    file->err = ROMFS_NOERR;

    if (size == 0) {

	return 0;
    }

    if (size > FLASH_SECTOR) {
	file->err = ROMFS_ERR_FILE_DATA_TOO_BIG;

	return 0;
    }

    uint32_t bytes = size % FLASH_SECTOR;

    if (file->offset + bytes >= FLASH_SECTOR) {
	uint32_t need = FLASH_SECTOR - file->offset;
	memmove(&file->io_buffer[file->offset], (char *)buffer, need);
	if (romfs_allocate_and_write_sector_internal(file->io_buffer, file) != ROMFS_NOERR) {
	    return 0;
	}
	file->entry.size += FLASH_SECTOR;
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

    file->entry.size += FLASH_SECTOR;

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

	memmove(&((romfs_entry *)flash_list)[file->nentry], &file->entry, sizeof(romfs_entry));

	romfs_flush();
    }

    return ROMFS_NOERR;
}

uint32_t romfs_open_file(char *name, romfs_file *file, uint8_t *io_buffer)
{
    file->op = ROMFS_OP_READ;
    if (romfs_find_internal(file, name) == ROMFS_NOERR) {
	file->pos = file->entry.start;
	file->offset = 0;
	file->read_offset = 0;
	file->io_buffer = (io_buffer) ? io_buffer : flash_buffer;
	return file->err;
    }

    return (file->err = ROMFS_ERR_NO_ENTRY);
}

uint32_t romfs_read_file(void *buffer, uint32_t size, romfs_file *file)
{
    file->err = ROMFS_NOERR;

    if (size == 0) {
	file->err = ROMFS_NOERR;

	return 0;
    }

    if (size > FLASH_SECTOR) {
	file->err = ROMFS_ERR_FILE_DATA_TOO_BIG;

	return 0;
    }

    if (file->read_offset >= file->entry.size) {
	file->err = ROMFS_ERR_EOF;
	return 0;
    }

    size = (file->read_offset + size > file->entry.size) ? (file->entry.size - file->read_offset) : size;

    uint32_t bytes = size % FLASH_SECTOR;

    if (file->offset + bytes >= FLASH_SECTOR) {
	uint32_t need = FLASH_SECTOR - file->offset;
	memmove(buffer, &flash_base[file->pos * FLASH_SECTOR + file->offset], need);
	file->pos = flash_map[file->pos];
	file->read_offset += need;
	need = bytes - need;
	if (need > 0) {
	    memmove(&((char *)buffer)[FLASH_SECTOR - file->offset], &flash_base[flash_map[file->pos] * FLASH_SECTOR], need);
	    file->offset = need;
	    file->read_offset += need;
	} else {
	    file->offset = 0;
	}
	return size;
    } else if (bytes > 0) {
	memmove(buffer, &flash_base[file->pos * FLASH_SECTOR + file->offset], bytes);
	file->offset += bytes;
	file->read_offset += bytes;
	
	return size;
    }

    memmove(buffer, &flash_base[file->pos * FLASH_SECTOR], size);
    file->read_offset += FLASH_SECTOR;
    file->pos = flash_map[file->pos];

    return size;
}

const char *romfs_strerror(uint32_t err)
{
    if (err < sizeof(romfs_errlist) / sizeof(const char *)) {
	return romfs_errlist[err];
    }
    
    return "Unknown";
}
