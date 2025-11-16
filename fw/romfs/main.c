/**
 * Copyright (c) 2022-2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

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

void save_romfs(char *name, uint8_t *mem, size_t len)
{
    FILE *out = fopen(name, "wb");
    if (out) {
        if (fwrite(mem, 1, len, out) != len) {
            fprintf(stderr, "Error write file!\n");
        }
        fclose(out);
    }
}

bool load_romfs(char *name, uint8_t *mem, size_t len, size_t *read_len)
{
    bool ret = true;
    FILE *out = fopen(name, "rb");
    if (out) {
        size_t rlen;
        if ((rlen = fread(mem, 1, len, out)) != len) {
            ret = false;
        }
        if (read_len) {
            *read_len = rlen;
        }
        fclose(out);
        return ret;
    }

    if (read_len) {
        *read_len = 0;
    }

    return false;
}

int main(int argc, char *argv[])
{
    if (argc <= 1) {
        fprintf(stderr, "No rom file defined!\n");
        return -1;
    }

    size_t read_len;
    if (!load_romfs(argv[1], memory, sizeof(memory), &read_len)) {
        if (read_len) {
            fprintf(stderr, "Cannot read %s, wrong rom image size (%zu)\n", argv[1], read_len);
            goto err;
        } else {
            fprintf(stderr, "Cannot open %s, create new image\n", argv[1]);
        }
    }

    flash_base = memory;

    uint32_t map_size = 0;
    uint32_t list_size = 0;

    romfs_get_buffers_sizes(sizeof(memory), &map_size, &list_size);

    uint16_t *flash_map = alloca(map_size);
    uint8_t *flash_list = alloca(list_size);

    if (!romfs_start(0x10000, sizeof(memory), flash_map, flash_list)) {
        printf("Cannot start romfs!\n");
        goto err;
    }

    uint8_t *romfs_io_buffer = alloca(ROMFS_FLASH_SECTOR);

    if (argc > 2) {
        if (!strcmp(argv[2], "format")) {
            romfs_format();
        } else if (!strcmp(argv[2], "list")) {
            romfs_dir target_dir;
            uint32_t err = ROMFS_NOERR;
            if (argc > 3) {
                err = romfs_dir_open_path(argv[3], &target_dir);
            } else {
                err = romfs_dir_root(&target_dir);
            }

            if (err != ROMFS_NOERR) {
                fprintf(stderr, "Error: [%s] %s!\n", (argc > 3) ? argv[3] : "/", romfs_strerror(err));
            } else {
                romfs_file file = {0};
                uint32_t list_err = romfs_list_dir(&file, true, &target_dir, true);
                if (list_err == ROMFS_ERR_NO_FREE_ENTRIES) {
                    printf("(empty)\n");
                } else if (list_err != ROMFS_NOERR) {
                    fprintf(stderr, "Error listing directory: %s\n", romfs_strerror(list_err));
                } else {
                    do {
                        bool is_dir = (file.entry.attr.names.type == ROMFS_TYPE_DIR);
                        printf("%s%s\t%u\t%02X %02X\n",
                               file.entry.name,
                               is_dir ? "/" : "",
                               is_dir ? 0u : file.entry.size,
                               file.entry.attr.names.mode,
                               file.entry.attr.names.type);
                    } while (romfs_list_dir(&file, false, &target_dir, true) == ROMFS_NOERR);
                }
            }
        } else if (!strcmp(argv[2], "delete")) {
            if (argc < 4) {
                fprintf(stderr, "Usage: %s delete <path>\n", argv[0]);
                goto err;
            }
            uint32_t err;
            if ((err = romfs_delete_path(argv[3])) != ROMFS_NOERR) {
                fprintf(stderr, "Error: [%s] %s!\n", argv[3], romfs_strerror(err));
            }
        } else if (!strcmp(argv[2], "push")) {
            if (argc < 5) {
                fprintf(stderr, "Usage: %s push <host_file> <romfs_path>\n", argv[0]);
                goto err;
            }
            FILE *inf = fopen(argv[3], "rb");
            if (inf) {
                uint8_t buffer[ROMFS_FLASH_SECTOR];
                int ret;
                romfs_file file;
                if (romfs_create_path(argv[4], &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, romfs_io_buffer, true) != ROMFS_NOERR) {
                    fprintf(stderr, "romfs error: %s\n", romfs_strerror(file.err));
                } else {
                    while ((ret = fread(buffer, 1, ROMFS_IO_CHUNK_SIZE, inf)) > 0) {
                        if (romfs_write_file(buffer, ret, &file) == 0) {
                            break;
                        }
                    }

                    if (file.err == ROMFS_NOERR) {
                        if (romfs_close_file(&file) != ROMFS_NOERR) {
                            fprintf(stderr, "romfs close error %s\n", romfs_strerror(file.err));
                        }
                    } else {
                        fprintf(stderr, "romfs write error %s\n", romfs_strerror(file.err));
                    }
                }
                fclose(inf);
            } else {
                fprintf(stderr, "Cannot open file %s\n", argv[3]);
            }
        } else if (!strcmp(argv[2], "pull")) {
            if (argc < 5) {
                fprintf(stderr, "Usage: %s pull <romfs_path> <host_file>\n", argv[0]);
                goto err;
            }
            romfs_file file;
            if (romfs_open_path(argv[3], &file, romfs_io_buffer) == ROMFS_NOERR) {
                FILE *outf = fopen(argv[4], "wb");
                if (outf) {
                    uint8_t buffer[ROMFS_FLASH_SECTOR];
                    int ret;
                    while ((ret = romfs_read_file(buffer, ROMFS_FLASH_SECTOR, &file)) > 0) {
                        fwrite(buffer, 1, ret, outf);
                    }

                    if (file.err != ROMFS_NOERR && file.err != ROMFS_ERR_EOF) {
                        fprintf(stderr, "romfs read error %s\n", romfs_strerror(file.err));
                    }
                    fclose(outf);
                } else {
                    fprintf(stderr, "Cannot open file %s\n", argv[3]);
                }
                romfs_close_file(&file);
            } else {
                fprintf(stderr, "romfs error: %s\n", romfs_strerror(file.err));
            }
        } else if (!strcmp(argv[2], "mkdir")) {
            if (argc < 4) {
                fprintf(stderr, "Usage: %s mkdir <path>\n", argv[0]);
                goto err;
            }
            romfs_dir created;
            uint32_t err = romfs_mkdir_path(argv[3], true, &created);
            if (err != ROMFS_NOERR) {
                fprintf(stderr, "Error creating directory [%s]: %s\n", argv[3], romfs_strerror(err));
            }
        } else if (!strcmp(argv[2], "rmdir")) {
            if (argc < 4) {
                fprintf(stderr, "Usage: %s rmdir <path>\n", argv[0]);
                goto err;
            }
            uint32_t err = romfs_rmdir_path(argv[3]);
            if (err != ROMFS_NOERR) {
                fprintf(stderr, "Error removing directory [%s]: %s\n", argv[3], romfs_strerror(err));
            }
        } else if (!strcmp(argv[2], "free")) {
            printf("Free space: %u bytes\n", romfs_free());
        } else {
            fprintf(stderr, "Error: Unknown command '%s'\n", argv[2]);
        }
    }

    save_romfs(argv[1], memory, sizeof(memory));

 err:

    return 0;
}
