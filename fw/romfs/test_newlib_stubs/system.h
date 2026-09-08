#ifndef ROMFS_TEST_SYSTEM_H
#define ROMFS_TEST_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dir.h>

/* Callback signatures mirror libdragon's system.h. No filesystem logic is
 * duplicated here: test_newlib links the production newlib-romfs.c. */
typedef struct {
    bool thread_safe;
    void *(*open)(char *name, int flags);
    int (*fstat)(void *file, struct stat *st);
    int (*stat)(char *name, struct stat *st);
    int (*lseek)(void *file, int ptr, int dir);
    int (*read)(void *file, uint8_t *ptr, int len);
    int (*write)(void *file, uint8_t *ptr, int len);
    int (*close)(void *file);
    int (*unlink)(char *name);
    int (*findfirst)(char *path, dir_t *dir);
    int (*findnext)(dir_t *dir);
    int (*findnext2)(const char *path, dir_t *dir);
    int (*ftruncate)(void *file, int length);
    int (*mkdir)(char *path, mode_t mode);
    int (*ioctl)(void *file, unsigned long cmd, void *argp);
    int (*utimes)(const char *path, const struct timeval times[2]);
} filesystem_t;

int attach_filesystem(const char *const prefix, filesystem_t *filesystem);

#endif
