#ifndef ROMFS_TEST_DIR_H
#define ROMFS_TEST_DIR_H

#include <stdint.h>

/* Minimal libdragon directory ABI for host tests of the actual bridge. */
#define DT_REG 1
#define DT_DIR 2
typedef struct {
    char d_name[256];
    int d_type;
    int64_t d_size;
    uint32_t d_cookie;
} dir_t;

#endif
