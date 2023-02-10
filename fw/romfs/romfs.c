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

void romfs_flush()
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

bool romfs_format()
{
#ifdef DEBUG
    printf("%s: create list entry (%d)\n", __func__, sizeof(romfs_entry));
#endif
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

#ifdef DEBUG
    printf("%s: create map\n", __func__);
#endif
    memset((uint8_t *)flash_map, 0xff, map_size);

    for (int i = 0; i < (flash_start + list_size + map_size) / FLASH_SECTOR; i++) {
	flash_map[i] = i + 1;
    }

    romfs_flush();

    return true;
}

uint32_t romfs_free()
{
    return -1;
}

bool romfs_create_file(char *name, romfs_file *file)
{
    return false;
}

bool romfs_write_file(void *buffer, uint32_t size, romfs_file *file)
{
    return false;
}

bool romfs_open_file(char *name, romfs_file *file)
{
    return false;
}

bool romfs_read_file(void *buffer, uint32_t size, romfs_file *file)
{
    return false;
}

static bool romfs_list_internal(romfs_file *file, bool first, bool with_deleted)
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
	return false;
    }

    if (((romfs_entry *)flash_list)[file->nentry].name[0] == ROMFS_EMPTY_ENTRY) {
	return false;
    }

    memmove(&file->entry, &((romfs_entry *)flash_list)[file->nentry], sizeof(romfs_entry));

    file->nentry++;
    file->pos = 0;
    file->offset = 0;

    return true;
}

static bool romfs_find_internal(romfs_file *file, const char *name)
{
    if (romfs_list(file, true)) {
	bool next_file;
	do {
	    if (!strcmp(name, file->entry.name)) {
		return true;
	    }
	    next_file = romfs_list(file, false);
	} while (next_file);
    }

    return false;
}

bool romfs_list(romfs_file *file, bool first)
{
    return romfs_list_internal(file, first, false);
}

bool romfs_delete(const char *name)
{
    romfs_file file;
    if (romfs_find_internal(&file, name)) {
	((romfs_entry *)flash_list)[file.nentry - 1].name[0] = ROMFS_DELETED_ENTRY;

	uint32_t sectors = ((file.entry.size + (FLASH_SECTOR - 1)) & ~(FLASH_SECTOR - 1)) / FLASH_SECTOR;
	uint16_t sector = file.entry.start;
	for (int i = 0; i < sectors; i++) {
	    uint16_t next = flash_map[sector];
	    flash_map[sector] = 0xffff;
	    sector = next;
	}

	romfs_flush();

	return true;
    }

    return false;
}
