#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <libusb.h>
#include "romfs.h"
#include "utils2.h"
#include "crc32.h"

#define RETRY_MAX	50 

//#define DEBUG

static libusb_context *ctx = NULL;
static libusb_device_handle *dev_handle;

static int bulk_transfer(struct libusb_device_handle *devh, unsigned char endpoint,
		  unsigned char *data, int length, int *transferred, unsigned int timeout)
{
    int ret;
    int try = 0;
    do {
	ret = libusb_bulk_transfer(devh, endpoint, data, length, transferred, timeout);
	if (ret == LIBUSB_ERROR_PIPE) {
//	    fprintf(stderr, "usb stalled, retry\n");
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
    uint32_t chksum;
    struct req_header romfs_req;
    struct ack_header romfs_ack;

    romfs_req.type = CART_ERASE_SEC;
    romfs_req.offset = offset;
    romfs_req.chksum = 0;
    romfs_req.chksum = crc32(&romfs_req, sizeof(romfs_req));

    bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
    if (actual != sizeof(romfs_req)) {
	fprintf(stderr, "Header error transfer\n");
	return false;
    }

    bulk_transfer(dev_handle, 0x82, (void *)&romfs_ack, sizeof(romfs_ack), &actual, 5000);
    chksum = romfs_ack.chksum;
    romfs_ack.chksum = 0;
    if (actual != sizeof(romfs_ack) && chksum != crc32(&romfs_ack, sizeof(romfs_ack))) {
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
    uint32_t chksum;
    struct req_header romfs_req;
    struct ack_header romfs_ack;

    romfs_req.type = CART_WRITE_SEC;
    romfs_req.offset = offset;
    romfs_req.chksum = 0;
    romfs_req.chksum = crc32(&romfs_req, sizeof(romfs_req));

    bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
    if (actual != sizeof(romfs_req)) {
	fprintf(stderr, "Header error transfer\n");
	return false;
    }

    bulk_transfer(dev_handle, 0x82, (void *)&romfs_ack, sizeof(romfs_ack), &actual, 5000);
    chksum = romfs_ack.chksum;
    romfs_ack.chksum = 0;
    if (actual != sizeof(romfs_ack) && chksum != crc32(&romfs_ack, sizeof(romfs_ack))) {
	fprintf(stderr, "Header reply error transfer\n");
	return false;
    }

    if (romfs_ack.type != ACK_NOERROR) {
	return false;
    }

    for (int i = 0; i < 4096; i += 32) {
	uint8_t tmp[36];
	memmove(tmp, &buffer[i], 32);
	*((uint32_t *)&tmp[32]) = crc32(tmp, 32);

	bulk_transfer(dev_handle, 0x01, (void *)tmp, sizeof(tmp), &actual, 5000);
	if (actual != sizeof(tmp)) {
	    fprintf(stderr, "Write data error transfer\n");
	    return false;
	}

	bulk_transfer(dev_handle, 0x82, (void *)&romfs_ack, sizeof(romfs_ack), &actual, 5000);
	chksum = romfs_ack.chksum;
	romfs_ack.chksum = 0;
	if (actual != sizeof(romfs_ack) && chksum != crc32(&romfs_ack, sizeof(romfs_ack))) {
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
    printf("flash read %08X (%p)\n", offset, (void *)buffer);
#endif

    int actual;
    uint32_t chksum;
    struct req_header romfs_req;

    romfs_req.type = CART_READ_SEC;
    romfs_req.offset = offset;
    romfs_req.chksum = 0;
    romfs_req.chksum = crc32(&romfs_req, sizeof(romfs_req));

    bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
    if (actual != sizeof(romfs_req)) {
	fprintf(stderr, "Read request error transfer\n");
	return false;
    }

    int pos = 0;

    while(true) {
	uint8_t tmp[36]; // 32 bytes + 4 bytes checksum
	bulk_transfer(dev_handle, 0x82, (void *)tmp, sizeof(tmp), &actual, 5000);
	chksum = *((uint32_t *)&tmp[32]);
	if (actual != sizeof(tmp) || chksum != crc32(tmp, sizeof(tmp) - 4)) {
	    fprintf(stderr, "Read reply error transfer\n");
	    return false;
	}

	memmove(&buffer[pos], tmp, 32);

	pos += 32;
	need -= 32;

	if (need == 0) {
	    break;
	}

	romfs_req.type = CART_READ_SEC_CONT;
	romfs_req.offset = pos;
	romfs_req.chksum = 0;
	romfs_req.chksum = crc32(&romfs_req, sizeof(romfs_req));

	bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
	if (actual != sizeof(romfs_req)) {
	    fprintf(stderr, "Read continue request error transfer\n");
	    return false;
	}
    }

    return true;
}

static bool flash_spi_mode(void)
{
    int actual;
    uint32_t chksum;
    struct req_header romfs_req;
    struct ack_header romfs_ack;

    romfs_req.type = FLASH_SPI_MODE;
    romfs_req.chksum = 0;
    romfs_req.chksum = crc32(&romfs_req, sizeof(romfs_req));

    bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
    if (actual != sizeof(romfs_req)) {
	fprintf(stderr, "Flash spi mode header error transfer\n");
	return false;
    }

    bulk_transfer(dev_handle, 0x82, (void *)&romfs_ack, sizeof(romfs_ack), &actual, 5000);
    chksum = romfs_ack.chksum;
    romfs_ack.chksum = 0;
    if (actual != sizeof(romfs_ack) && chksum != crc32(&romfs_ack, sizeof(romfs_ack))) {
	fprintf(stderr, "Flash spi mode reply error transfer\n");
	return false;
    }

    return true;
}

static bool flash_quad_mode(void)
{
    int actual;
    uint32_t chksum;
    struct req_header romfs_req;
    struct ack_header romfs_ack;

    romfs_req.type = FLASH_QUAD_MODE;
    romfs_req.chksum = 0;
    romfs_req.chksum = crc32(&romfs_req, sizeof(romfs_req));

    bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
    if (actual != sizeof(romfs_req)) {
	fprintf(stderr, "Flash quad mode header error transfer\n");
	return false;
    }

    bulk_transfer(dev_handle, 0x82, (void *)&romfs_ack, sizeof(romfs_ack), &actual, 5000);
    chksum = romfs_ack.chksum;
    romfs_ack.chksum = 0;
    if (actual != sizeof(romfs_ack) && chksum != crc32(&romfs_ack, sizeof(romfs_ack))) {
	fprintf(stderr, "Flash quad mode reply error transfer\n");
	return false;
    }

    return true;
}

static bool dfu_mode(void)
{
    int actual;
    uint32_t chksum;
    struct req_header romfs_req;
    struct ack_header romfs_ack;

    romfs_req.type = DFU_MODE;
    romfs_req.chksum = 0;
    romfs_req.chksum = crc32(&romfs_req, sizeof(romfs_req));

    bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
    if (actual != sizeof(romfs_req)) {
	fprintf(stderr, "DFU mode header error transfer\n");
	return false;
    }

    bulk_transfer(dev_handle, 0x82, (void *)&romfs_ack, sizeof(romfs_ack), &actual, 5000);
    chksum = romfs_ack.chksum;
    romfs_ack.chksum = 0;
    if (actual != sizeof(romfs_ack) && chksum != crc32(&romfs_ack, sizeof(romfs_ack))) {
	fprintf(stderr, "DFU mode reply error transfer\n");
	return false;
    }

    return true;
}

int main(int argc, char *argv[])
{
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
    uint32_t chksum;
    struct req_header romfs_req;
    struct ack_header romfs_info;

    romfs_req.type = CART_INFO;
    romfs_req.chksum = 0;
    romfs_req.chksum = crc32(&romfs_req, sizeof(romfs_req));

    bulk_transfer(dev_handle, 0x01, (void *)&romfs_req, sizeof(romfs_req), &actual, 5000);
    if (actual != sizeof(romfs_req)) {
	fprintf(stderr, "Header error transfer\n");
	goto err;
    }

    bulk_transfer(dev_handle, 0x82, (void *)&romfs_info, sizeof(romfs_info), &actual, 5000);
    chksum = romfs_info.chksum;
    romfs_info.chksum = 0;
    if (actual != sizeof(romfs_info) && chksum != crc32(&romfs_info, sizeof(romfs_info))) {
	fprintf(stderr, "Header reply error transfer\n");
	goto err;
    }

    flash_spi_mode();

    if (!romfs_start(romfs_info.data.info.start, romfs_info.data.info.size)) {
	printf("Cannot start romfs!\n");
	goto err;
    }

    if (argc > 1) {
	if (!strcmp(argv[1], "dfu")) {
	    dfu_mode();

	    goto finish;
	} else if (!strcmp(argv[1], "format")) {
	    romfs_format();
	} else if (!strcmp(argv[1], "list")) {
	    romfs_file file;
	    if (romfs_list(&file, true) == ROMFS_NOERR) {
		do {
		    printf("%s\t%d\t%0X %4X\n", file.entry.name, file.entry.size, file.entry.mode, file.entry.type);
		} while (romfs_list(&file, false) == ROMFS_NOERR);
	    }
	} else if (!strcmp(argv[1], "delete")) {
	    uint32_t err;
	    if ((err = romfs_delete(argv[2])) != ROMFS_NOERR) {
		fprintf(stderr, "Error: [%s] %s!\n", argv[3], romfs_strerror(err));
	    }
	} else if (!strcmp(argv[1], "push")) {
	    FILE *inf = fopen(argv[2], "rb");
	    if (inf) {
		uint8_t buffer[4096];
		int ret;
		romfs_file file;
		if (romfs_create_file(argv[3], &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, NULL) != ROMFS_NOERR) {
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
	} else if (!strcmp(argv[1], "pull")) {
	    romfs_file file;
	    if (romfs_open_file(argv[2], &file, NULL) == ROMFS_NOERR) {
		FILE *outf = fopen(argv[3], "wb");
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

 err:

    flash_quad_mode();

 finish:

    libusb_release_interface(dev_handle, 0);

    return 0;
}
