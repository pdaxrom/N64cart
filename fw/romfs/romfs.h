/**
 * Copyright (c) 2022-2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __ROMFS_H__
#define __ROMFS_H__

#define ROMFS_FLASH_SECTOR (4096)

#define ROMFS_MAX_NAME_LEN (54)

#define ROMFS_EMPTY_ENTRY   '\xff'
#define ROMFS_DELETED_ENTRY '\xfe'

#define ROMFS_MODE_READWRITE	(0)
#define ROMFS_MODE_READONLY	(1 << 0)
#define ROMFS_MODE_SYSTEM	(1 << 1)
#define ROMFS_MODE_RESERVED	(1 << 2)

#define ROMFS_TYPE_FIRMWARE	(0x00)
#define ROMFS_TYPE_FLASHLIST	(0x01)
#define ROMFS_TYPE_FLASHMAP	(0x02)
#define ROMFS_TYPE_MISC		(0xff)

#define ROMFS_OP_READ		(0)
#define ROMFS_OP_WRITE		(1)

enum {
    ROMFS_NOERR = 0,
    ROMFS_ERR_NO_ENTRY,
    ROMFS_ERR_NO_FREE_ENTRIES,
    ROMFS_ERR_NO_SPACE,
    ROMFS_ERR_FILE_EXISTS,
    ROMFS_ERR_FILE_DATA_TOO_BIG,
    ROMFS_ERR_EOF,
};

typedef struct __attribute__((packed)){
    char     name[ROMFS_MAX_NAME_LEN];
    uint16_t mode: 3;
    uint16_t type: 13;
    uint32_t start;
    uint32_t size;
} romfs_entry;

typedef struct {
    uint32_t op;
    romfs_entry entry;
    uint32_t nentry;
    uint32_t pos;
    uint32_t offset;
    uint32_t read_offset;
    uint32_t err;
    uint8_t *io_buffer;
} romfs_file;

bool romfs_flash_sector_erase(uint32_t offset);
bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer);
bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need);

bool romfs_start(uint32_t start, uint32_t size);
bool romfs_format(void);
uint32_t romfs_free(void);
uint32_t romfs_list(romfs_file *entry, bool first);
uint32_t romfs_delete(const char *name);
uint32_t romfs_create_file(char *name, romfs_file *file, uint16_t mode, uint16_t type, uint8_t *io_buffer);
uint32_t romfs_write_file(void *buffer, uint32_t size, romfs_file *file);
uint32_t romfs_close_file(romfs_file *file);
uint32_t romfs_open_file(char *name, romfs_file *file, uint8_t *io_buffer);
bool romfs_read_map_table(uint16_t *map_buffer, uint32_t map_size, romfs_file *file);
uint32_t romfs_read_file(void *buffer, uint32_t size, romfs_file *file);
const char *romfs_strerror(uint32_t err);

void save_romfs(char *name, uint8_t *mem, size_t len);
bool read_romfs(char *name, uint8_t *mem, size_t len);

#endif
