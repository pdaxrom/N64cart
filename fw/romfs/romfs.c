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
	    memmove(&flash_map[i], &flash_base[flash_start + list_size + i], FLASH_SECTOR);
	}
	return true;
    }

    return false;
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

    for (int i = 0; i < list_size; i += FLASH_SECTOR) {
	romfs_flash_sector_erase(flash_start + i);
	romfs_flash_sector_write(flash_start + i, &flash_list[i]);
    }

#ifdef DEBUG
    printf("%s: create map\n", __func__);
#endif
    memset((uint8_t *)flash_map, 0xff, map_size);

    for (int i = 0; i < (flash_start + list_size + map_size) / FLASH_SECTOR; i++) {
	flash_map[i] = i + 1;
    }

    for (int i = 0; i < map_size; i += FLASH_SECTOR) {
	romfs_flash_sector_erase(flash_start + list_size + i);
	romfs_flash_sector_write(flash_start + list_size + i, &((uint8_t *)flash_map)[i]);
    }

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

bool romfs_list(romfs_file *file, bool first)
{
    if (first) {
	file->nentry = 0;
    }

    if (((romfs_entry *)flash_list)[file->nentry].name[0] == -1) {
	return false;
    }

    memmove(&file->entry, &((romfs_entry *)flash_list)[file->nentry], sizeof(romfs_entry));

    file->nentry++;
    file->pos = 0;
    file->offset = 0;

    return true;
}

bool romfs_delete(char *name)
{
    return false;
}
