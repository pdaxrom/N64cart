/*
 * TCP server
 */

#include <inttypes.h>
#include <libusb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifndef _WIN32
#include <sys/select.h>
#endif
#if defined(__APPLE__) || defined(__linux__)
#include <alloca.h>
#endif

#include "utils2.h"
#include "usb-romfs-proxy.h"

#include "simple-connection-lib/src/base64.c"
#include "simple-connection-lib/src/getrandom.c"
#include "simple-connection-lib/src/tcp.c"

#define TCP_PORT	6464

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

bool romfs_flash_sector_erase(uint32_t offset)
{
#ifdef DEBUG
    printf("flash erase %08X\n", offset);
#endif

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

    return true;
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
#ifdef DEBUG
    printf("flash write %08X (%p)\n", offset, (void *)buffer);
#endif

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

    return true;
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)
{
#ifdef DEBUG
    printf("flash read %08X (%p) %d\n", offset, (void *)buffer, need);
#endif

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

    return true;
}

static int usb_romfs(tcp_channel *client)
{
    int retval = 1;
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

    int actual;
    struct req_header romfs_req;
    struct ack_header romfs_info;

    while(true) {
        uint16_t cmd;
        if ((r = tcp_read(client, &cmd, sizeof(cmd))) < 0) {
            fprintf(stderr, "tcp_read() error %s\n", __FILE__);
            goto err;
        }

        if (cmd == USB_CMD) {
            if ((r = tcp_read(client, &romfs_req, sizeof(romfs_req))) != sizeof(romfs_req)) {
                fprintf(stderr, "tcp_read() error %s\n", __FILE__);
                goto err;
            }

            bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
            if (actual != sizeof(romfs_req)) {
                fprintf(stderr, "Header error transfer\n");
                goto err;
            }

            bulk_transfer(dev_handle, 0x82, (void *)&romfs_info, sizeof(romfs_info), &actual, 5000);
            if (actual != sizeof(romfs_info)) {
                fprintf(stderr, "Header reply error transfer\n");
                goto err;
            }

            if ((r = tcp_write(client, &romfs_info, sizeof(romfs_info))) != sizeof(romfs_info)) {
                fprintf(stderr, "tcp_write() error %s\n", __FILE__);
                goto err;
            }
        } else if (cmd == USB_ERASE_SECTOR) {
            struct sector_info sec;
            if ((r = tcp_read(client, &sec, sizeof(sec))) != sizeof(sec)) {
                fprintf(stderr, "tcp_read() error %s\n", __FILE__);
                goto err;
            }
            uint8_t ok = romfs_flash_sector_erase(sec.offset);
            if ((r = tcp_write(client, &ok, sizeof(ok))) != sizeof(ok)) {
                fprintf(stderr, "tcp_write() error %s\n", __FILE__);
                goto err;
            }
        } else if (cmd == USB_READ_SECTOR) {
            struct sector_info sec;
            if ((r = tcp_read(client, &sec, sizeof(sec))) != sizeof(sec)) {
                fprintf(stderr, "tcp_read() error %s\n", __FILE__);
                goto err;
            }
            uint8_t *buf = alloca(sec.length);
            uint8_t ok = romfs_flash_sector_read(sec.offset, buf, sec.length);
            if ((r = tcp_write(client, &ok, sizeof(ok))) != sizeof(ok)) {
                fprintf(stderr, "tcp_write() error %s\n", __FILE__);
                goto err;
            }
            if (ok) {
                if ((r = tcp_write(client, &buf, sec.length)) != sec.length) {
                    fprintf(stderr, "tcp_write() error %s\n", __FILE__);
                    goto err;
                }
            }
        } else if (cmd == USB_WRITE_SECTOR) {
            struct sector_info sec;
            if ((r = tcp_read(client, &sec, sizeof(sec))) != sizeof(sec)) {
                fprintf(stderr, "tcp_read() error %s\n", __FILE__);
                goto err;
            }
            uint8_t *buf = alloca(sec.length);
            if ((r = tcp_read(client, &buf, sec.length)) != sec.length) {
                fprintf(stderr, "tcp_read() error %s\n", __FILE__);
                goto err;
            }
            uint8_t ok = romfs_flash_sector_write(sec.offset, buf);
            if (!ok) {
                fprintf(stderr, "flash sector write error!\n");
                goto err;
            }
            //	    if ((r = tcp_write(client, &ok, sizeof(ok))) != sizeof(ok)) {
            //		fprintf(stderr, "tcp_write() error %s\n", __FILE__);
            //		goto err;
            //	    }
        }
    }

err:

    libusb_release_interface(dev_handle, 0);

    return retval;
}

int main(int argc, char *argv[])
{
    tcp_channel *server = tcp_open(TCP_SERVER, NULL, TCP_PORT, NULL, NULL);
    if (!server) {
        fprintf(stderr, "tcp_open()\n");
        return -1;
    }

    while(true) {
        tcp_channel *client = tcp_accept(server);
        if (!client) {
            fprintf(stderr, "tcp_accept()\n");
            return -1;
        }

        printf("New connection accepted\n");

        usb_romfs(client);

        tcp_close(client);
    }

    tcp_close(server);

    return 0;
}
