/**
 * @file newlib-romfs.c
 * @brief Bridge ROMFS into newlib via attach_filesystem
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <system.h>
#include <dir.h>
#include <unistd.h>

#include "newlib-romfs.h"
#include "romfs.h"

#define ROMFS_PREFIX "romfs:/"
#define ROMFS_MAX_PATH_LEN 256
#define ROMFS_DIR_COOKIE_SLOTS 8

typedef struct {
    romfs_file file;
    uint8_t *io_buffer;
} romfs_handle_t;

typedef struct {
    bool in_use;
    romfs_dir dir;
    romfs_file iter;
} romfs_dir_cookie_t;

static romfs_dir_cookie_t romfs_dir_cookies[ROMFS_DIR_COOKIE_SLOTS];

static void *romfs_fs_open(char *name, int flags);
static int romfs_fs_fstat(void *file, struct stat *st);
static int romfs_fs_stat(char *name, struct stat *st);
static int romfs_fs_lseek(void *file, int ptr, int dir);
static int romfs_fs_read(void *file, uint8_t *ptr, int len);
static int romfs_fs_write(void *file, uint8_t *ptr, int len);
static int romfs_fs_close(void *file);
static int romfs_fs_unlink(char *name);
static int romfs_fs_findfirst(char *path, dir_t *dir);
static int romfs_fs_findnext(dir_t *dir);
static int romfs_fs_mkdir(char *path, mode_t mode);

static filesystem_t romfs_fs = {
    .open = romfs_fs_open,
    .fstat = romfs_fs_fstat,
    .stat = romfs_fs_stat,
    .lseek = romfs_fs_lseek,
    .read = romfs_fs_read,
    .write = romfs_fs_write,
    .close = romfs_fs_close,
    .unlink = romfs_fs_unlink,
    .findfirst = romfs_fs_findfirst,
    .findnext = romfs_fs_findnext,
    .ftruncate = NULL,
    .mkdir = romfs_fs_mkdir,
    .ioctl = NULL,
};

static const char *strip_prefix(const char *path)
{
    size_t prefix_len = strlen(ROMFS_PREFIX);
    if (strncmp(path, ROMFS_PREFIX, prefix_len) == 0) {
        return path + prefix_len;
    }
    return path;
}

static int build_romfs_path(const char *path, char *out, size_t out_size)
{
    if (!path || !out) {
        return -1;
    }

    const char *trimmed = strip_prefix(path);
    if (trimmed[0] == '\0') {
        trimmed = "/";
    }

    if (trimmed[0] != '/') {
        int written = snprintf(out, out_size, "/%s", trimmed);
        if (written < 0 || (size_t)written >= out_size) {
            return -1;
        }
    } else {
        if (strlen(trimmed) >= out_size) {
            return -1;
        }
        strcpy(out, trimmed);
    }

    return 0;
}

static void to_stat(const romfs_entry *entry, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_size = entry->size;
    st->st_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    if (entry->attr.names.type == ROMFS_TYPE_DIR) {
        st->st_mode |= S_IFDIR;
    } else {
        st->st_mode |= S_IFREG;
    }
}

static void fill_dir_entry(dir_t *dir, const romfs_file *entry)
{
    const romfs_entry *e = &entry->entry;
    strncpy(dir->d_name, e->name, sizeof(dir->d_name) - 1);
    dir->d_name[sizeof(dir->d_name) - 1] = '\0';
    dir->d_type = (e->attr.names.type == ROMFS_TYPE_DIR) ? DT_DIR : DT_REG;
    dir->d_size = (dir->d_type == DT_DIR) ? -1 : (int64_t)e->size;
}

static romfs_dir_cookie_t *allocate_cookie(void)
{
    for (int i = 0; i < ROMFS_DIR_COOKIE_SLOTS; i++) {
        if (!romfs_dir_cookies[i].in_use) {
            romfs_dir_cookies[i].in_use = true;
            memset(&romfs_dir_cookies[i].iter, 0, sizeof(romfs_dir_cookies[i].iter));
            return &romfs_dir_cookies[i];
        }
    }
    return NULL;
}

static void release_cookie(uint32_t cookie)
{
    if (cookie == 0) {
        return;
    }
    uint32_t idx = cookie - 1;
    if (idx < ROMFS_DIR_COOKIE_SLOTS) {
        romfs_dir_cookies[idx].in_use = false;
        memset(&romfs_dir_cookies[idx].iter, 0, sizeof(romfs_dir_cookies[idx].iter));
    }
}

static romfs_dir_cookie_t *get_cookie(uint32_t cookie)
{
    if (cookie == 0) {
        return NULL;
    }
    uint32_t idx = cookie - 1;
    if (idx >= ROMFS_DIR_COOKIE_SLOTS) {
        return NULL;
    }
    return romfs_dir_cookies[idx].in_use ? &romfs_dir_cookies[idx] : NULL;
}

static void *romfs_fs_open(char *name, int flags)
{
    char abs_path[ROMFS_MAX_PATH_LEN];
    if (build_romfs_path(name, abs_path, sizeof(abs_path)) != 0) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    romfs_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        errno = ENOMEM;
        return NULL;
    }

    handle->io_buffer = malloc(ROMFS_FLASH_SECTOR);
    if (!handle->io_buffer) {
        free(handle);
        errno = ENOMEM;
        return NULL;
    }

    uint32_t err = ROMFS_NOERR;
    bool create = (flags & O_CREAT) != 0;
    bool append = (flags & O_APPEND) != 0;
    bool trunc = (flags & O_TRUNC) != 0;

    if (trunc && !create) {
        if (romfs_delete_path(abs_path) != ROMFS_NOERR) {
            err = ROMFS_ERR_NO_ENTRY;
        } else {
            create = true;
        }
    }

    if (err == ROMFS_NOERR) {
        if (append) {
            err = romfs_open_append_path(abs_path, &handle->file, ROMFS_TYPE_MISC, handle->io_buffer, create);
        } else if (create) {
            err = romfs_create_path(abs_path, &handle->file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, handle->io_buffer, true);
            if (err == ROMFS_ERR_FILE_EXISTS) {
                if (trunc) {
                    romfs_delete_path(abs_path);
                    err = romfs_create_path(abs_path, &handle->file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, handle->io_buffer, true);
                } else if (flags & O_EXCL) {
                    errno = EEXIST;
                    err = ROMFS_ERR_FILE_EXISTS;
                } else {
                    err = romfs_open_path(abs_path, &handle->file, handle->io_buffer);
                }
            }
        } else {
            err = romfs_open_path(abs_path, &handle->file, handle->io_buffer);
        }
    }

    if (err != ROMFS_NOERR) {
        free(handle->io_buffer);
        free(handle);
        errno = (err == ROMFS_ERR_NO_ENTRY) ? ENOENT : EIO;
        return NULL;
    }

    return handle;
}

static int romfs_fs_close(void *file)
{
    romfs_handle_t *handle = (romfs_handle_t *)file;
    if (!handle) {
        errno = EBADF;
        return -1;
    }

    romfs_close_file(&handle->file);
    free(handle->io_buffer);
    free(handle);
    return 0;
}

static int romfs_fs_read(void *file, uint8_t *ptr, int len)
{
    romfs_handle_t *handle = (romfs_handle_t *)file;
    if (!handle) {
        errno = EBADF;
        return -1;
    }

    int ret = (int)romfs_read_file(ptr, (uint32_t)len, &handle->file);
    if (ret < 0 || (handle->file.err != ROMFS_NOERR && handle->file.err != ROMFS_ERR_EOF)) {
        errno = EIO;
        return -1;
    }

    return ret;
}

static int romfs_fs_write(void *file, uint8_t *ptr, int len)
{
    romfs_handle_t *handle = (romfs_handle_t *)file;
    if (!handle) {
        errno = EBADF;
        return -1;
    }

    int ret = (int)romfs_write_file(ptr, (uint32_t)len, &handle->file);
    if ((ret == 0 && len > 0) || handle->file.err != ROMFS_NOERR) {
        errno = EIO;
        return -1;
    }

    return ret;
}

static int romfs_fs_lseek(void *file, int ptr, int dir)
{
    romfs_handle_t *handle = (romfs_handle_t *)file;
    if (!handle) {
        errno = EBADF;
        return -1;
    }

    if (romfs_seek_file(&handle->file, ptr, dir) != ROMFS_NOERR) {
        errno = EINVAL;
        return -1;
    }

    uint32_t position = 0;
    romfs_tell_file(&handle->file, &position);
    return (int)position;
}

static int romfs_fs_fstat(void *file, struct stat *st)
{
    romfs_handle_t *handle = (romfs_handle_t *)file;
    if (!handle) {
        errno = EBADF;
        return -1;
    }

    to_stat(&handle->file.entry, st);
    return 0;
}

static int romfs_fs_stat(char *name, struct stat *st)
{
    char abs_path[ROMFS_MAX_PATH_LEN];
    if (build_romfs_path(name, abs_path, sizeof(abs_path)) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    romfs_entry entry;
    if (romfs_get_entry_path(abs_path, &entry) != ROMFS_NOERR) {
        errno = ENOENT;
        return -1;
    }

    to_stat(&entry, st);
    return 0;
}

static int romfs_fs_unlink(char *name)
{
    char abs_path[ROMFS_MAX_PATH_LEN];
    if (build_romfs_path(name, abs_path, sizeof(abs_path)) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (romfs_delete_path(abs_path) != ROMFS_NOERR) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int romfs_fs_mkdir(char *path, mode_t mode)
{
    (void)mode;

    char abs_path[ROMFS_MAX_PATH_LEN];
    if (build_romfs_path(path, abs_path, sizeof(abs_path)) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (romfs_mkdir_path(abs_path, true, NULL) != ROMFS_NOERR) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int romfs_fs_findfirst(char *path, dir_t *dir)
{
    char abs_path[ROMFS_MAX_PATH_LEN];
    if (build_romfs_path(path, abs_path, sizeof(abs_path)) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    romfs_dir_cookie_t *cookie = allocate_cookie();
    if (!cookie) {
        errno = ENFILE;
        return -1;
    }

    uint32_t err;
    if (strcmp(abs_path, "/") == 0) {
        err = romfs_dir_root(&cookie->dir);
    } else {
        err = romfs_dir_open_path(abs_path, &cookie->dir);
    }

    if (err != ROMFS_NOERR) {
        cookie->in_use = false;
        errno = ENOENT;
        return -1;
    }

    err = romfs_list_dir(&cookie->iter, true, &cookie->dir, true);
    if (err == ROMFS_ERR_NO_FREE_ENTRIES) {
        cookie->in_use = false;
        return -1;
    }
    if (err != ROMFS_NOERR) {
        cookie->in_use = false;
        errno = EIO;
        return -1;
    }

    fill_dir_entry(dir, &cookie->iter);
    dir->d_cookie = (uint32_t)(cookie - romfs_dir_cookies + 1);
    return 0;
}

static int romfs_fs_findnext(dir_t *dir)
{
    romfs_dir_cookie_t *cookie = get_cookie(dir->d_cookie);
    if (!cookie) {
        errno = EINVAL;
        return -1;
    }

    uint32_t err = romfs_list_dir(&cookie->iter, false, &cookie->dir, true);
    if (err == ROMFS_NOERR) {
        fill_dir_entry(dir, &cookie->iter);
        return 0;
    }

    release_cookie(dir->d_cookie);
    if (err == ROMFS_ERR_NO_FREE_ENTRIES) {
        return -1;
    }

    errno = EIO;
    return -1;
}

int newlib_romfs_init(void)
{
    memset(romfs_dir_cookies, 0, sizeof(romfs_dir_cookies));
    if (attach_filesystem(ROMFS_PREFIX, &romfs_fs) != 0) {
        return 0;
    }
    return 1;
}

int rmdir(const char *path)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    char abs_path[ROMFS_MAX_PATH_LEN];
    if (build_romfs_path(path, abs_path, sizeof(abs_path)) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    uint32_t err = romfs_rmdir_path(abs_path);
    if (err == ROMFS_NOERR) {
        return 0;
    }

    switch (err) {
    case ROMFS_ERR_NO_ENTRY:
    case ROMFS_ERR_DIR_INVALID:
        errno = ENOENT;
        break;
    case ROMFS_ERR_DIR_NOT_EMPTY:
        errno = ENOTEMPTY;
        break;
    default:
        errno = EIO;
        break;
    }

    return -1;
}
