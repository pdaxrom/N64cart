/**
* Copyright (c) 2022-2024 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "syslog.h"
#include "settings.h"

#define ROMFS_POSIX_PREFIX "romfs:/"
#define ROMFS_PATH_MAX 256

static void build_boot_settings_prefixed_path(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    if (!path || path[0] == '\0') {
        snprintf(out, out_size, "%s", ROMFS_POSIX_PREFIX);
    } else if (path[0] == '/') {
        snprintf(out, out_size, "romfs:%s", path);
    } else {
        snprintf(out, out_size, ROMFS_POSIX_PREFIX "%s", path);
    }
}

bool boot_settings_load(boot_settings* settings) {
    memset(settings, 0, sizeof(boot_settings));

    char path[ROMFS_PATH_MAX + 8];
    const char *file_name = "settings.txt";
    build_boot_settings_prefixed_path(file_name, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        syslog(LOG_ERR, "cannot open settings %s (errno %d)", path, errno);
        return false;
    }

    char line[32] = {0};
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (!strcmp(line, "auto_boot")) settings->auto_boot = true;
        else if (!strcmp(line, "consumer_mode")) settings->consumer_mode = true;
        memset(line, 0, sizeof(line));
    }
    settings->auto_boot = true;
    fclose(fp);
    return true;
}