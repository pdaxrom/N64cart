/**
 * Copyright (c) 2022-2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __ROMFS_H__
#define __ROMFS_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ROMFS_FLASH_SIZE
#define ROMFS_FLASH_SIZE	(64)
#endif

#define ROMFS_FLASH_SECTOR (4096) /* Flash sector size in bytes */

#define ROMFS_MAX_NAME_LEN (54) /* Maximum length of file name */

#define ROMFS_EMPTY_ENTRY   '\xff' /* Marker for empty entry */
#define ROMFS_DELETED_ENTRY '\xfe' /* Marker for deleted entry */

#define ROMFS_MODE_READWRITE	(0) /* Read-write mode */
#define ROMFS_MODE_READONLY	(1 << 0) /* Read-only mode */
#define ROMFS_MODE_SYSTEM	(1 << 1) /* System mode */
#define ROMFS_MODE_RESERVED	(1 << 2) /* Reserved mode */

#define ROMFS_TYPE_FIRMWARE	(0x00) /* Firmware type */
#define ROMFS_TYPE_FLASHLIST	(0x01) /* Flash list type */
#define ROMFS_TYPE_FLASHMAP	(0x02) /* Flash map type */
#define ROMFS_TYPE_DIR		(0x03) /* Flash map type */
#define ROMFS_TYPE_MISC		(0x1f) /* Miscellaneous type */

#define ROMFS_MB (1024 * 1024) /* One megabyte in bytes */
#define ROMFS_MODE_MASK ((1U << 3) - 1) /* Mask for mode bits (3 bits) */
#define ROMFS_TYPE_SHIFT 3 /* Shift for type bits */

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#define ROMFS_MAX_DIRS         (16)
#define ROMFS_ROOT_DIR_ID      (0)
#define ROMFS_INVALID_ENTRY_ID (0xffff)

#define ROMFS_OP_READ		(0)
#define ROMFS_OP_WRITE		(1)

enum {
    ROMFS_NOERR = 0,
    ROMFS_ERR_NO_IO_BUFFER,
    ROMFS_ERR_NO_ENTRY,
    ROMFS_ERR_NO_FREE_ENTRIES,
    ROMFS_ERR_NO_SPACE,
    ROMFS_ERR_FILE_EXISTS,
    ROMFS_ERR_FILE_DATA_TOO_BIG,
    ROMFS_ERR_BUFFER_TOO_SMALL,
    ROMFS_ERR_EOF,
    ROMFS_ERR_OPERATION,
    ROMFS_ERR_DIR_LIMIT,
    ROMFS_ERR_DIR_INVALID,
    ROMFS_ERR_DIR_NOT_EMPTY,
};

typedef struct __attribute__((packed)) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint16_t mode:3;
    uint16_t type:5;
    uint16_t parent:4;
    uint16_t current:4;
#else
    uint16_t current:4;
    uint16_t parent:4;
    uint16_t type:5;
    uint16_t mode:3;
#endif
} attr_by_names;

typedef struct __attribute__((packed)) {
    char name[ROMFS_MAX_NAME_LEN];
    union {
        attr_by_names names;
        uint16_t raw;
    } attr;
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
    uint8_t parent_dir_id;
    uint8_t dir_id;
    uint32_t buffer_base;
    bool buffer_from_flash;
} romfs_file;

typedef struct {
    uint8_t id;
    uint32_t entry_index;
} romfs_dir;

bool romfs_flash_sector_erase(uint32_t offset);
bool romfs_flash_sector_write(uint32_t offset, uint8_t * buffer);
bool romfs_flash_sector_read(uint32_t offset, uint8_t * buffer, uint32_t need);

void romfs_get_buffers_sizes(uint32_t rom_size, uint32_t * map_size, uint32_t * list_size);
bool romfs_start(uint32_t start, uint32_t rom_size, uint16_t * flash_map, uint8_t * flash_list);
bool romfs_format(void);
uint32_t romfs_free(void);
uint32_t romfs_list(romfs_file * entry, bool first);
uint32_t romfs_delete(const char *name);
uint32_t romfs_create_file(const char *name, romfs_file * file, uint16_t mode, uint16_t type, uint8_t * io_buffer);
uint32_t romfs_write_file(const void *buffer, uint32_t size, romfs_file * file);
uint32_t romfs_close_file(romfs_file * file);
uint32_t romfs_open_file(const char *name, romfs_file * file, uint8_t * io_buffer);
uint32_t romfs_read_map_table(uint16_t * map_buffer, uint32_t map_size, romfs_file * file);
uint32_t romfs_read_file(void *buffer, uint32_t size, romfs_file * file);
uint32_t romfs_tell_file(romfs_file *file, uint32_t *position);
uint32_t romfs_seek_file(romfs_file *file, int32_t offset, int whence);
uint32_t romfs_open_append(const char *name, romfs_file *file, uint16_t type, uint8_t *io_buffer);
uint32_t romfs_open_append_in_dir(const romfs_dir *dir, const char *name, romfs_file *file, uint16_t type, uint8_t *io_buffer);
uint32_t romfs_open_append_path(const char *path, romfs_file *file, uint16_t type, uint8_t *io_buffer, bool create_dirs);
uint32_t romfs_rename(const char *src_name, const char *dst_name);
uint32_t romfs_rename_in_dir(const romfs_dir *src_dir, const char *src_name, const romfs_dir *dst_dir, const char *dst_name);
uint32_t romfs_rename_path(const char *src_path, const char *dst_path, bool create_dirs);
uint32_t romfs_get_entry(const char *name, romfs_entry *out_entry);
uint32_t romfs_get_entry_in_dir(const romfs_dir *dir, const char *name, romfs_entry *out_entry);
uint32_t romfs_get_entry_path(const char *path, romfs_entry *out_entry);

uint32_t romfs_dir_root(romfs_dir *dir);
uint32_t romfs_dir_open(const romfs_dir *parent, const char *name, romfs_dir *out);
uint32_t romfs_dir_create(const romfs_dir *parent, const char *name, romfs_dir *out);
uint32_t romfs_dir_remove(const romfs_dir *dir);
uint32_t romfs_list_dir(romfs_file * entry, bool first, const romfs_dir *dir, bool include_dirs);
uint32_t romfs_create_file_in_dir(const romfs_dir *dir, const char *name, romfs_file * file, uint16_t mode, uint16_t type, uint8_t * io_buffer);
uint32_t romfs_open_file_in_dir(const romfs_dir *dir, const char *name, romfs_file * file, uint8_t * io_buffer);
uint32_t romfs_delete_in_dir(const romfs_dir *dir, const char *name);
uint32_t romfs_open_path(const char *path, romfs_file * file, uint8_t * io_buffer);
uint32_t romfs_create_path(const char *path, romfs_file * file, uint16_t mode, uint16_t type, uint8_t * io_buffer, bool create_dirs);
uint32_t romfs_mkdir_path(const char *path, bool create_parents, romfs_dir *out_dir);
uint32_t romfs_rmdir_path(const char *path);
uint32_t romfs_dir_open_path(const char *path, romfs_dir *out_dir);
uint32_t romfs_delete_path(const char *path);

const char *romfs_strerror(uint32_t err);

#ifdef __cplusplus
}
#endif

#endif
