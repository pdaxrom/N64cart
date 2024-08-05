/**
 * Copyright (c) 2022-2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include "romfs.h"

static uint8_t memory[ROMFS_FLASH_SIZE * 1024 * 1024];

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
        int rlen;
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

    if (!romfs_start(0x10000, sizeof(memory))) {
        printf("Cannot start romfs!\n");
        goto err;
    }

    if (argc > 2) {
        if (!strcmp(argv[2], "format")) {
            romfs_format();
        } else if (!strcmp(argv[2], "list")) {
            romfs_file file;
            if (romfs_list(&file, true) == ROMFS_NOERR) {
                do {
                    printf("%s\t%d\t%0X %4X\n", file.entry.name, file.entry.size, file.entry.attr.names.mode, file.entry.attr.names.type);
                } while (romfs_list(&file, false) == ROMFS_NOERR);
            }
        } else if (!strcmp(argv[2], "delete")) {
            uint32_t err;
            if ((err = romfs_delete(argv[3])) != ROMFS_NOERR) {
                fprintf(stderr, "Error: [%s] %s!\n", argv[3], romfs_strerror(err));
            }
        } else if (!strcmp(argv[2], "push")) {
            FILE *inf = fopen(argv[3], "rb");
            if (inf) {
                uint8_t buffer[4096];
                int ret;
                romfs_file file;
                if (romfs_create_file(argv[4], &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, NULL) != ROMFS_NOERR) {
                    fprintf(stderr, "romfs error: %s\n", romfs_strerror(file.err));
                } else {
                    while ((ret = fread(buffer, 1, 64, inf)) > 0) {
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
            romfs_file file;
            if (romfs_open_file(argv[3], &file, NULL) == ROMFS_NOERR) {
                FILE *outf = fopen(argv[4], "wb");
                if (outf) {
                    uint8_t buffer[4096];
                    int ret;
                    while ((ret = romfs_read_file(buffer, 4096, &file)) > 0) {
                        fwrite(buffer, 1, ret, outf);
                    }

                    if (file.err != ROMFS_NOERR && file.err != ROMFS_ERR_EOF) {
                        fprintf(stderr, "romfs read error %s\n", romfs_strerror(file.err));
                    }
                } else {
                    fprintf(stderr, "Cannot open file %s\n", argv[3]);
                }
            } else {
                fprintf(stderr, "romfs error: %s\n", romfs_strerror(file.err));
            }
        } else {
            fprintf(stderr, "Error: Unknown command '%s'\n", argv[2]);
        }
    }

    save_romfs(argv[1], memory, sizeof(memory));

 err:

    return 0;
}
