/**
 * Copyright (c) 2022 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __UTILS_H__
#define __UTILS_H__

#define DATA_UNKNOWN	0xdead
#define DATA_RETRY	0x2343
#define DATA_REPLY	0x2344
#define DATA_WRITE	0x2345
#define DATA_READ	0x2346
#define DATA_INFO	0x2347
#define DATA_ERROR	0x2348
#define DATA_LIST	0x2350
#define DATA_DEL	0x2351
#define DATA_FORMAT	0x2352

//#ifndef ROMFS_MAX_NAME_LEN
//#define ROMFS_MAX_NAME_LEN 54
//#endif

struct __attribute__((__packed__)) data_header {
    uint16_t	type;
    uint32_t	length;
    uint16_t	mode: 3;
    uint16_t	ftype: 13;
    char	name[ROMFS_MAX_NAME_LEN + 1];
};

#endif
