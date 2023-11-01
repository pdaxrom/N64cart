/**
 * Copyright (c) 2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#define CART_UNKNOWN	0xdead
#define CART_INFO	0x2345
#define CART_READ_SEC	0x2346
#define CART_READ_SEC_CONT	0x2347
#define CART_WRITE_SEC	0x2348
#define CART_WRITE_SEC_CONT	0x2349
#define CART_ERASE_SEC	0x234A
#define CART_BREAK	0x234B
#define FLASH_SPI_MODE	0x234C
#define FLASH_QUAD_MODE 0x234D
#define DFU_MODE	0x234E

#define ACK_NOERROR	0x5432
#define ACK_ERROR	0x5433

struct __attribute__((__packed__)) cart_info {
    uint32_t	start;
    uint32_t	size;
    uint32_t	vers;
};

struct __attribute__((__packed__)) req_header {
    uint32_t	chksum;
    uint16_t	type;
    uint32_t	offset;
};

struct __attribute__((__packed__)) ack_header {
    uint32_t	chksum;
    uint16_t	type;
    union {
	struct cart_info info;
	uint32_t chksum;
    } data;
};
