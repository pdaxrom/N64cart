/**
 * Copyright (c) 2022-2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include "romfs.h"

#define ROMFS_IO_CHUNK_SIZE 64 /* I/O chunk size for reading/writing */

static uint8_t memory[ROMFS_FLASH_SIZE * ROMFS_MB];

static uint8_t *flash_base = NULL;

bool romfs_flash_sector_erase(uint32_t offset)
{
#ifdef DEBUG
    printf("flash erase %08X\n", offset);
#endif
    memset(&flash_base[offset], 0xff, ROMFS_FLASH_SECTOR);
    return true;
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
#ifdef DEBUG
    printf("flash write %08X (%p)\n", offset, (void *)buffer);
#endif
    memmove(&flash_base[offset], buffer, ROMFS_FLASH_SECTOR);
    return true;
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)
{
#ifdef DEBUG
    printf("flash read %08X (%p)\n", offset, (void *)buffer);
#endif
    memmove(buffer, &flash_base[offset], need);
    return true;
}

static bool save_romfs(const char *name, const uint8_t *mem, size_t len)
{
    FILE *out = fopen(name, "wb");
    if (!out) {
        perror(name);
        return false;
    }
    bool ok = fwrite(mem, 1, len, out) == len;
    if (fclose(out) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "Cannot save image %s\n", name);
    }
    return ok;
}

static bool load_romfs(const char *name, uint8_t *mem, size_t len, size_t *read_len, bool *missing)
{
    *read_len = 0;
    *missing = false;
    FILE *in = fopen(name, "rb");
    if (!in) {
        *missing = errno == ENOENT;
        return false;
    }
    *read_len = fread(mem, 1, len, in);
    bool ok = *read_len == len && fgetc(in) == EOF && !ferror(in);
    if (fclose(in) != 0) {
        ok = false;
    }
    return ok;
}

static bool transfer_file(const char *local_path, const char *remote_path, bool upload, uint8_t *io_buffer)
{
    FILE *local = upload ? fopen(local_path, "rb") : NULL;
    if (upload && !local) {
        perror(local_path);
        return false;
    }
    romfs_file file;
    uint32_t err = upload ? romfs_create_path(remote_path, &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer, true)
                          : romfs_open_path(remote_path, &file, io_buffer);
    bool ok = err == ROMFS_NOERR;
    if (!ok) {
        fprintf(stderr, "ROMFS open %s: %s\n", remote_path, romfs_strerror(err));
    } else {
        if (!upload) {
            local = fopen(local_path, "wb");
            if (!local) {
                perror(local_path);
                romfs_close_file(&file);
                return false;
            }
        }
        uint8_t buffer[ROMFS_FLASH_SECTOR];
        if (upload) {
            size_t count;
            while ((count = fread(buffer, 1, ROMFS_IO_CHUNK_SIZE, local)) > 0) {
                uint32_t written = romfs_write_file(buffer, (uint32_t) count, &file);
                if (written != count || file.err != ROMFS_NOERR) {
                    fprintf(stderr, "ROMFS write: %u/%zu bytes, %s\n", written, count, romfs_strerror(file.err));
                    ok = false;
                    break;
                }
            }
            if (ferror(local)) {
                fprintf(stderr, "Cannot read local file %s\n", local_path);
                ok = false;
            }
        } else {
            while (true) {
                uint32_t count = romfs_read_file(buffer, sizeof(buffer), &file);
                if (count && fwrite(buffer, 1, count, local) != count) {
                    fprintf(stderr, "Cannot write local file %s\n", local_path);
                    ok = false;
                    break;
                }
                if (file.err != ROMFS_NOERR && file.err != ROMFS_ERR_EOF) {
                    fprintf(stderr, "ROMFS read: %s\n", romfs_strerror(file.err));
                    ok = false;
                    break;
                }
                if (!count || file.err == ROMFS_ERR_EOF) {
                    break;
                }
            }
        }
        err = romfs_close_file(&file);
        if (err != ROMFS_NOERR) {
            fprintf(stderr, "ROMFS close: %s\n", romfs_strerror(err));
            ok = false;
        }
    }
    if (local && fclose(local) != 0) {
        fprintf(stderr, "Cannot close local file %s\n", local_path);
        ok = false;
    }
    return ok;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <image> <format|list|free|push|pull|delete|mkdir|rmdir> [arguments]\n", argv[0]);
        return 2;
    }
    const char *command = argv[2];
    bool format = strcmp(command, "format") == 0;
    bool list = strcmp(command, "list") == 0;
    bool free_space = strcmp(command, "free") == 0;
    bool upload = strcmp(command, "push") == 0;
    bool download = strcmp(command, "pull") == 0;
    bool delete = strcmp(command, "delete") == 0;
    bool mkdir = strcmp(command, "mkdir") == 0;
    bool rmdir = strcmp(command, "rmdir") == 0;
    if ((!format && !list && !free_space && !upload && !download && !delete && !mkdir && !rmdir) ||
            ((upload || download) && argc != 5) ||
            ((delete || mkdir || rmdir) && argc != 4) ||
            ((format || free_space) && argc != 3) || (list && argc > 4)) {
        fprintf(stderr, "Invalid command or arguments: %s\n", command);
        return 2;
    }

    size_t read_len = 0;
    bool missing;
    if (!load_romfs(argv[1], memory, sizeof(memory), &read_len, &missing)) {
        if (!format || !missing) {
            fprintf(stderr, "Cannot read image %s (expected %zu bytes, read %zu)\n", argv[1], sizeof(memory), read_len);
            return 1;
        }
        memset(memory, 0xff, sizeof(memory));
    }
    flash_base = memory;
    uint32_t map_size, list_size;
    romfs_get_buffers_sizes(sizeof(memory), &map_size, &list_size);
    uint16_t *flash_map = alloca(map_size);
    uint8_t *flash_list = alloca(list_size);
    if (!romfs_start(0x10000, sizeof(memory), flash_map, flash_list)) {
        fprintf(stderr, "Cannot start ROMFS\n");
        return 1;
    }
    uint8_t io_buffer[ROMFS_FLASH_SECTOR];
    bool ok = true;
    uint32_t err = ROMFS_NOERR;
    if (format) {
        ok = romfs_format();
        if (!ok) {
            fprintf(stderr, "ROMFS format failed\n");
        }
    } else if (list) {
        romfs_dir dir;
        err = argc > 3 && strcmp(argv[3], "/") != 0 ? romfs_dir_open_path(argv[3], &dir) : romfs_dir_root(&dir);
        if (err == ROMFS_NOERR) {
            romfs_file entry = {0};
            err = romfs_list_dir(&entry, true, &dir, true);
            if (err == ROMFS_ERR_NO_FREE_ENTRIES) {
                puts("(empty)");
            }
            while (err == ROMFS_NOERR) {
                bool is_dir = entry.entry.attr.names.type == ROMFS_TYPE_DIR;
                printf("%s%s\t%u\t%02X %02X\n", entry.entry.name, is_dir ? "/" : "",
                       is_dir ? 0u : entry.entry.size, entry.entry.attr.names.mode, entry.entry.attr.names.type);
                err = romfs_list_dir(&entry, false, &dir, true);
            }
            if (err == ROMFS_ERR_NO_FREE_ENTRIES) {
                err = ROMFS_NOERR;
            }
        }
    } else if (free_space) {
        printf("Free space: %u bytes\n", romfs_free());
    } else if (upload || download) {
        ok = transfer_file(upload ? argv[3] : argv[4], upload ? argv[4] : argv[3], upload, io_buffer);
    } else if (delete) {
        err = romfs_delete_path(argv[3]);
    } else if (mkdir) {
        err = romfs_mkdir_path(argv[3], true, NULL);
    } else if (rmdir) {
        err = romfs_rmdir_path(argv[3]);
    }
    if (err != ROMFS_NOERR) {
        fprintf(stderr, "ROMFS %s: %s\n", command, romfs_strerror(err));
        ok = false;
    }
    if (format || upload || delete || mkdir || rmdir) {
        err = romfs_sync();
        if (err != ROMFS_NOERR) {
            fprintf(stderr, "ROMFS sync: %s\n", romfs_strerror(err));
            return 1;
        }
        /* Preserve an accepted partial upload even when the transfer failed. */
        if (!save_romfs(argv[1], memory, sizeof(memory))) {
            ok = false;
        }
    }
    if (fflush(stdout) != 0) {
        ok = false;
    }
    return ok ? 0 : 1;
}
