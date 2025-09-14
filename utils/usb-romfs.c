#include <inttypes.h>
#ifndef ENABLE_REMOTE
#include <libusb.h>
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) || defined(__linux__) || defined(sgi)
#include <alloca.h>
#endif

#include "romfs.h"
#include "utils2.h"

#ifdef sgi
#define strtoimax strtoll
#endif

#ifdef ENABLE_REMOTE
#include "proxy-romfs.h"

#include "simple-connection-lib/src/tcp.h"

static tcp_channel *server;

int tcp_read_all(tcp_channel *c, void *buf, size_t len)
{
    char *ptr = buf;
    size_t all = 0;

    while (len > 0) {
        size_t r = tcp_read(c, &ptr[all], len);
        if (r <= 0) {
            break;
        }
        len -= r;
        all += r;
    }

    return all;
}

int tcp_write_all(tcp_channel *c, void *buf, size_t len)
{
    char *ptr = buf;
    size_t all = 0;

    while (len > 0) {
        size_t r = tcp_write(c, &ptr[all], len);
        if (r <= 0) {
            break;
        }
        len -= r;
        all += r;
    }

    return all;
}
#else
#define RETRY_MAX 50

// #define DEBUG

static libusb_context *ctx = NULL;
static libusb_device_handle *dev_handle;

static int bulk_transfer(struct libusb_device_handle *devh, unsigned char endpoint, unsigned char *data, int length, int *transferred, unsigned int timeout)
{
    int ret;
    int try = 0;
    do {
        ret = libusb_bulk_transfer(devh, endpoint, data, length, transferred, timeout);
        if (ret == LIBUSB_ERROR_PIPE) {
            //      fprintf(stderr, "usb stalled, retry\n");
            libusb_clear_halt(devh, endpoint);
        }
        try++;
    } while ((ret == LIBUSB_ERROR_PIPE) && (try < RETRY_MAX));
    return ret;
}

#endif

bool romfs_flash_sector_erase(uint32_t offset)
{
#ifdef DEBUG
    printf("flash erase %08X\n", offset);
#endif
#ifdef ENABLE_REMOTE
    struct __attribute__((__packed__)) {
        uint16_t c;
        struct sector_info s;
    } cmd;

    cmd.c = htons(USB_ERASE_SECTOR);
    cmd.s.offset = htonl(offset);

    if (tcp_write_all(server, &cmd, sizeof(cmd)) != sizeof(cmd)) {
        fprintf(stderr, "Write flash sector erase request error\n");
        return false;
    }

    uint8_t ok = 0;
    if (tcp_read_all(server, &ok, sizeof(ok)) != sizeof(ok)) {
        fprintf(stderr, "Read flash sector erase status error\n");
        return false;
    }

    if (!ok) {
        return false;
    }
#else
    int actual;
    struct req_header romfs_req;
    struct ack_header romfs_ack;

    romfs_req.type = CART_ERASE_SEC;
    romfs_req.offset = offset;

    bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
    if (actual != sizeof(romfs_req)) {
        fprintf(stderr, "Header error transfer\n");
        return false;
    }

    bulk_transfer(dev_handle, 0x82, (void *)&romfs_ack, sizeof(romfs_ack), &actual, 5000);
    if (actual != sizeof(romfs_ack)) {
        fprintf(stderr, "Header reply error transfer\n");
        return false;
    }

    if (romfs_ack.type != ACK_NOERROR) {
        return false;
    }
#endif
    return true;
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
#ifdef DEBUG
    printf("flash write %08X (%p)\n", offset, (void *)buffer);
#endif
#ifdef ENABLE_REMOTE
    struct __attribute__((__packed__)) {
        uint16_t c;
        struct sector_info s;
    } cmd;

    cmd.c = htons(USB_WRITE_SECTOR);
    cmd.s.offset = htonl(offset);
    cmd.s.length = htonl(4096);

    if (tcp_write_all(server, &cmd, sizeof(cmd)) != sizeof(cmd)) {
        fprintf(stderr, "Write flash sector write request error\n");
        return false;
    }

    if (tcp_write_all(server, buffer, 4096) != 4096) {
        fprintf(stderr, "Write flash sector write data error\n");
        return false;
    }

    uint8_t ok = 0;
    if (tcp_read_all(server, &ok, sizeof(ok)) != sizeof(ok)) {
        fprintf(stderr, "Read flash sector write status error\n");
        return false;
    }

    if (!ok) {
        return false;
    }
#else
    int actual;
    struct req_header romfs_req;
    struct ack_header romfs_ack;

    romfs_req.type = CART_WRITE_SEC;
    romfs_req.offset = offset;

    bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
    if (actual != sizeof(romfs_req)) {
        fprintf(stderr, "Header error transfer\n");
        return false;
    }

    bulk_transfer(dev_handle, 0x82, (void *)&romfs_ack, sizeof(romfs_ack), &actual, 5000);
    if (actual != sizeof(romfs_ack)) {
        fprintf(stderr, "Header reply error transfer\n");
        return false;
    }

    if (romfs_ack.type != ACK_NOERROR) {
        return false;
    }

    for (int i = 0; i < 4096; i += 64) {
        uint8_t tmp[64];
        memmove(tmp, &buffer[i], 64);

        bulk_transfer(dev_handle, 0x01, (void *)tmp, sizeof(tmp), &actual, 5000);
        if (actual != sizeof(tmp)) {
            fprintf(stderr, "Write data error transfer\n");
            return false;
        }

        bulk_transfer(dev_handle, 0x82, (void *)&romfs_ack, sizeof(romfs_ack), &actual, 5000);
        if (actual != sizeof(romfs_ack)) {
            fprintf(stderr, "Write ack error transfer\n");
            return false;
        }

        if (romfs_ack.type != ACK_NOERROR) {
            return false;
        }
    }
#endif
    return true;
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)
{
#ifdef DEBUG
    printf("flash read %08X (%p) %d\n", offset, (void *)buffer, need);
#endif
#ifdef ENABLE_REMOTE
    struct __attribute__((__packed__)) {
        uint16_t c;
        struct sector_info s;
    } cmd;

    cmd.c = htons(USB_READ_SECTOR);
    cmd.s.offset = htonl(offset);
    cmd.s.length = htonl(need);

    if (tcp_write_all(server, &cmd, sizeof(cmd)) != sizeof(cmd)) {
        fprintf(stderr, "Write flash sector read request error\n");
        return false;
    }

    if (tcp_read_all(server, buffer, need) != need) {
        fprintf(stderr, "Read flash sector data error\n");
        return false;
    }
#else
    int actual;
    struct req_header romfs_req;

    romfs_req.type = CART_READ_SEC;
    romfs_req.offset = offset;

    bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
    if (actual != sizeof(romfs_req)) {
        fprintf(stderr, "Read request error transfer\n");
        return false;
    }

    int pos = 0;

    while (true) {
        uint8_t tmp[64];        // 32 bytes + 4 bytes checksum
        bulk_transfer(dev_handle, 0x82, (void *)tmp, sizeof(tmp), &actual, 5000);
        if (actual != sizeof(tmp)) {
            fprintf(stderr, "Read reply error transfer\n");
            return false;
        }

        memmove(&buffer[pos], tmp, (need > 64) ? 64 : need);

        if (need < 64) {
            break;
        }

        pos += 64;
        need -= 64;

        romfs_req.type = CART_READ_SEC_CONT;
        romfs_req.offset = pos;

        bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
        if (actual != sizeof(romfs_req)) {
            fprintf(stderr, "Read continue request error transfer\n");
            return false;
        }
    }
#endif
    return true;
}

static bool send_usb_cmd(uint16_t type, struct ack_header *ack)
{
    struct ack_header romfs_ack;

#ifdef ENABLE_REMOTE
    struct __attribute__((__packed__)) {
        uint16_t c;
        struct req_header romfs_req;
    } cmd;

    cmd.c = htons(USB_CMD);
    cmd.romfs_req.type = htons(type);

    if (tcp_write_all(server, &cmd, sizeof(cmd)) != sizeof(cmd)) {
        fprintf(stderr, "Command header error transfer\n");
        return false;
    }

    if (tcp_read_all(server, ack ? ack : &romfs_ack, sizeof(romfs_ack)) != sizeof(romfs_ack)) {
        fprintf(stderr, "Command reply error transfer\n");
        return false;
    }

    if (ack) {
        ack->type = ntohs(ack->type);
        ack->info.start = ntohl(ack->info.start);
        ack->info.size = ntohl(ack->info.size);
        ack->info.vers = ntohl(ack->info.vers);
    }
#else
    int actual;
    struct req_header romfs_req;
    romfs_req.type = type;

    bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
    if (actual != sizeof(romfs_req)) {
        fprintf(stderr, "Command header error transfer\n");
        return false;
    }

    bulk_transfer(dev_handle, 0x82, ack ? ack : (void *)&romfs_ack, sizeof(romfs_ack), &actual, 5000);
    if (actual != sizeof(romfs_ack)) {
        fprintf(stderr, "Command reply error transfer\n");
        return false;
    }
#endif
    return true;
}

static char *find_filename(char *path)
{
    char *pos = strrchr(path, '/');
    if (!pos) {
        return path;
    }
    return pos + 1;
}

static const char *human_readable_size(double bytes, char *buf, size_t bufsize)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int unit_index = 0;

    while (bytes >= 1024 && unit_index < 5) {
        bytes /= 1024.0;
        unit_index++;
    }

    snprintf(buf, bufsize, "%.2f %s", bytes, units[unit_index]);
    return buf;
}

static void usage(void)
{
#ifdef ENABLE_REMOTE
    static const char *str = "remote-romfs <proxy ip>";
#else
    static const char *str = "usb-romfs";
#endif
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "%s help\n", str);
    fprintf(stderr, "%s bootloader\n", str);
    fprintf(stderr, "%s reboot\n", str);
    fprintf(stderr, "%s format\n", str);
    fprintf(stderr, "%s list\n", str);
    fprintf(stderr, "%s delete <remote filename>\n", str);
    fprintf(stderr, "%s push [--fix-rom][--fix-pi-bus-speed[=12..FF]] <local filename>[ <remote filename>]\n", str);
    fprintf(stderr, "%s pull <remote filename>[ <local filename>]\n", str);
    fprintf(stderr, "%s free\n", str);
}

int main(int argc, char *argv[])
{
    int retval = 1;
#ifdef ENABLE_REMOTE
    if (argc < 2) {
        usage();
        return 1;
    }

    server = tcp_open(TCP_CLIENT, argv[1], TCP_PORT, NULL, NULL);
    if (!server) {
        fprintf(stderr, "Cannot connect to romfs proxy!\n");
        return 1;
    }

    argv[1] = argv[0];
    argv++;
    argc--;
#else
    uint32_t r = libusb_init(&ctx);

    if (r < 0) {
        fprintf(stderr, "Error init libusb\n");
        return 1;
    }

    libusb_set_debug(ctx, 3);

    dev_handle = libusb_open_device_with_vid_pid(ctx, 0x1209, 0x6800);

    if (dev_handle == NULL) {
        fprintf(stderr, "Cannot open device, ensure N64cart is attached\n");
        return 1;
    }

    if (libusb_kernel_driver_active(dev_handle, 0) == 1) {
        fprintf(stderr, "Kernel has hold of this device, detaching kernel driver\n");
        libusb_detach_kernel_driver(dev_handle, 0);
    }

    libusb_claim_interface(dev_handle, 0);
#endif

    struct ack_header romfs_info;

    if (!send_usb_cmd(CART_INFO, &romfs_info)) {
        goto err;
    }

    printf("firmware version  : %d.%d\n", romfs_info.info.vers >> 8, romfs_info.info.vers & 0xff);
    printf("ROMFS start offset: %08X\n", romfs_info.info.start);
    printf("ROMFS flash size  : %d\n", romfs_info.info.size);

    if (argc > 1 && strcmp(argv[1], "help")) {
        if (!strcmp(argv[1], "bootloader")) {
            if (send_usb_cmd(BOOTLOADER_MODE, NULL)) {
                retval = 0;
            }
        } else if (!strcmp(argv[1], "reboot")) {
            if (send_usb_cmd(CART_REBOOT, NULL)) {
                retval = 0;
            }
        } else {
            if (!send_usb_cmd(FLASH_SPI_MODE, NULL)) {
                fprintf(stderr, "cannot switch flash to spi mode, error!\n");
                goto err_io;
            }

            uint32_t flash_map_size, flash_list_size;
            romfs_get_buffers_sizes(romfs_info.info.size, &flash_map_size, &flash_list_size);
            uint16_t *romfs_flash_map = alloca(flash_map_size);
            uint8_t *romfs_flash_list = alloca(flash_list_size);
            uint8_t *romfs_flash_buffer = alloca(ROMFS_FLASH_SECTOR);

            if (!romfs_start(romfs_info.info.start, romfs_info.info.size, romfs_flash_map, romfs_flash_list)) {
                printf("Cannot start romfs!\n");
                goto err_io;
            }

            if (!strcmp(argv[1], "format")) {
                if (romfs_format()) {
                    retval = 0;
                }
            } else if (!strcmp(argv[1], "free")) {
                uint32_t free_mem = romfs_free();
                char free_txt[128];
                printf("Free %d bytes (%s)\n", free_mem, human_readable_size(free_mem, free_txt, sizeof(free_txt)));
            } else if (!strcmp(argv[1], "list")) {
                romfs_file file;
                char num_buf[128];
                bool conv = (argc > 2 && !strcmp(argv[2], "-h")) ? true : false;
                printf("\n");
                if (romfs_list(&file, true) == ROMFS_NOERR) {
                    do {
                        if (conv) {
                            printf("%0X %03X %10s %s\n", file.entry.attr.names.mode, file.entry.attr.names.type, human_readable_size(file.entry.size, num_buf, sizeof(num_buf)), file.entry.name);
                        } else {
                            printf("%0X %03X %10d %s\n", file.entry.attr.names.mode, file.entry.attr.names.type, file.entry.size, file.entry.name);
                        }
                    } while (romfs_list(&file, false) == ROMFS_NOERR);
                    retval = 0;
                }
                uint32_t free_mem = romfs_free();
                printf("\nFree %d bytes (%s)\n", free_mem, human_readable_size(free_mem, num_buf, sizeof(num_buf)));
            } else if (!strcmp(argv[1], "delete")) {
                uint32_t err;
                if ((err = romfs_delete(argv[2])) != ROMFS_NOERR) {
                    fprintf(stderr, "Error: [%s] %s!\n", argv[3], romfs_strerror(err));
                } else {
                    retval = 0;
                }
            } else if (!strcmp(argv[1], "push")) {
                bool fix_endian = false;
                int rom_type = -1;
                bool fix_pi_freq = false;
                uint16_t pi_freq = 0xff;

                if (argc > 2 && !strcmp(argv[2], "--fix-rom")) {
                    fix_endian = true;
                    argv++;
                    argc--;
                }

                char *param = argv[2];

                if (argc > 2 && !strncmp(param, "--fix-pi-bus-speed", 18)) {
                    fix_pi_freq = true;
                    if (param[18] == '=') {
                        pi_freq = strtoimax(&param[19], NULL, 16) & 0xff;
                        if (pi_freq < 0x12) {
                            pi_freq = 0x12;
                        }
                    }
                    argv++;
                    argc--;
                }

                if (argc > 2) {
                    FILE *inf = fopen(argv[2], "rb");
                    if (inf) {
                        uint8_t buffer[4096];
                        int ret;
                        romfs_file file;
                        if (romfs_create_file((argc > 3) ? argv[3] : find_filename(argv[2]), &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, romfs_flash_buffer) != ROMFS_NOERR) {
                            fprintf(stderr, "romfs error: %s\n", romfs_strerror(file.err));
                        } else {
                            fseek(inf, 0, SEEK_END);
                            int file_size = ftell(inf);
                            fseek(inf, 0, SEEK_SET);
                            int total = 0;
                            printf("\n");
                            while ((ret = fread(buffer, 1, 4096, inf)) > 0) {
                                if (fix_endian) {
                                    if (rom_type == -1) {
                                        fprintf(stderr, "Detected ROM type: ");
                                        if (buffer[0] == 0x80 && buffer[1] == 0x37 && buffer[2] == 0x12 && buffer[3] == 0x40) {
                                            rom_type = 0;
                                            fprintf(stderr, "Z64\n");
                                        } else if (buffer[0] == 0x40 && buffer[1] == 0x12 && buffer[2] == 0x37 && buffer[3] == 0x80) {
                                            rom_type = 1;
                                            fprintf(stderr, "N64\n");
                                        } else if (buffer[0] == 0x37 && buffer[1] == 0x80 && buffer[2] == 0x40 && buffer[3] == 0x12) {
                                            rom_type = 2;
                                            fprintf(stderr, "V64\n");
                                        } else {
                                            fprintf(stderr, "Unknown\n\nError!\n");
                                            break;
                                        }
                                    }

                                    if (ret % 4 != 0) {
                                        fprintf(stderr, "Unaligned read from local file, error!\n");
                                        break;
                                    }

                                    if (rom_type) {
                                        for (int i = 0; i < ret; i += 4) {
                                            uint8_t tmp;
                                            if (rom_type == 1) {
                                                tmp = buffer[i + 0];
                                                buffer[i + 0] = buffer[i + 3];
                                                buffer[i + 3] = tmp;
                                                tmp = buffer[i + 2];
                                                buffer[i + 2] = buffer[i + 1];
                                                buffer[i + 1] = tmp;
                                            } else {
                                                tmp = buffer[i + 0];
                                                buffer[i + 0] = buffer[i + 1];
                                                buffer[i + 1] = tmp;
                                                tmp = buffer[i + 2];
                                                buffer[i + 2] = buffer[i + 3];
                                                buffer[i + 3] = tmp;
                                            }
                                        }
                                    }
                                }

                                if (fix_pi_freq) {
                                    if (buffer[0] == 0x80 && buffer[1] == 0x37 && buffer[3] == 0x40) {
                                        printf("PI bus freq set to %02X\n", pi_freq);
                                        buffer[2] = pi_freq;
                                    } else {
                                        fprintf(stderr, "Rom type is not Z64, use --fix-rom to convert to Z64 type!\n");
                                        break;
                                    }
                                    fix_pi_freq = false;
                                }

                                if (romfs_write_file(buffer, ret, &file) == 0) {
                                    break;
                                }
                                total += ret;
                                printf("\rWrite %.1f%%", (double)total / (double)file_size * 100.);
                                fflush(stdout);
                            }
                            printf("\n");

                            if (file.err == ROMFS_NOERR) {
                                if (romfs_close_file(&file) != ROMFS_NOERR) {
                                    fprintf(stderr, "romfs close error %s\n", romfs_strerror(file.err));
                                } else {
                                    retval = 0;
                                }
                            } else {
                                fprintf(stderr, "romfs write error %s\n", romfs_strerror(file.err));
                            }
                        }
                        fclose(inf);
                    } else {
                        fprintf(stderr, "Cannot open file %s\n", argv[3]);
                    }
                } else {
                    fprintf(stderr, "Missed local filename\n");
                }
            } else if (!strcmp(argv[1], "pull")) {
                if (argc > 2) {
                    romfs_file file;
                    if (romfs_open_file(argv[2], &file, romfs_flash_buffer) == ROMFS_NOERR) {
                        FILE *outf = fopen((argc > 3) ? argv[3] : find_filename(argv[2]), "wb");
                        if (outf) {
                            uint8_t buffer[4096];
                            int ret;
                            printf("\n");
                            while ((ret = romfs_read_file(buffer, 4096, &file)) > 0) {
                                fwrite(buffer, 1, ret, outf);
                                printf("\rRead %.1f%%", (double)file.read_offset / (double)file.entry.size * 100.);
                                fflush(stdout);
                            }
                            printf("\n");

                            if (file.err != ROMFS_NOERR && file.err != ROMFS_ERR_EOF) {
                                fprintf(stderr, "romfs read error %s\n", romfs_strerror(file.err));
                            } else {
                                retval = 0;
                            }
                        } else {
                            fprintf(stderr, "Cannot open file %s\n", argv[3]);
                        }
                    } else {
                        fprintf(stderr, "romfs error: %s\n", romfs_strerror(file.err));
                    }
                }
            } else {
                fprintf(stderr, "Error: Unknown command '%s'\n", argv[1]);
            }

        err_io:
            if (!send_usb_cmd(FLASH_QUAD_MODE, NULL)) {
                fprintf(stderr, "cannot switch flash to quad mode, error!\n");
                retval = 1;
            }
        }
    } else {
        usage();
    }

err:
#ifdef ENABLE_REMOTE
    tcp_close(server);
#else
    libusb_release_interface(dev_handle, 0);
#endif
    return retval;
}
