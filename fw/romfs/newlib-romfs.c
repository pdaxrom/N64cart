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
    romfs_file read_file;
    uint8_t *io_buffer;
    char path[ROMFS_MAX_PATH_LEN];
    bool readable;
    bool writable;
    bool append;
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
static int romfs_fs_ftruncate(void *file, int length);
static int romfs_fs_findfirst(char *path, dir_t *dir);
static int romfs_fs_findnext(dir_t *dir);
static int romfs_fs_findnext2(const char *path, dir_t *dir);
static int romfs_fs_mkdir(char *path, mode_t mode);
static int romfs_fs_utimes(const char *path, const struct timeval times[2]);

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
    .findnext2 = romfs_fs_findnext2,
    .ftruncate = romfs_fs_ftruncate,
    .mkdir = romfs_fs_mkdir,
    .ioctl = NULL,
    .utimes = romfs_fs_utimes,
};

static int errno_from_romfs(uint32_t err)
{
    switch (err) {
    case ROMFS_NOERR:
        return 0;
    case ROMFS_ERR_NO_IO_BUFFER:
        return ENOMEM;
    case ROMFS_ERR_NO_ENTRY:
    case ROMFS_ERR_DIR_INVALID:
        return ENOENT;
    case ROMFS_ERR_NO_FREE_ENTRIES:
    case ROMFS_ERR_NO_SPACE:
    case ROMFS_ERR_DIR_LIMIT:
        return ENOSPC;
    case ROMFS_ERR_FILE_EXISTS:
        return EEXIST;
    case ROMFS_ERR_FILE_DATA_TOO_BIG:
    case ROMFS_ERR_BUFFER_TOO_SMALL:
        return ENAMETOOLONG;
    case ROMFS_ERR_DIR_NOT_EMPTY:
        return ENOTEMPTY;
    case ROMFS_ERR_OPERATION:
        return EINVAL;
    case ROMFS_ERR_BUSY:
        return EBUSY;
    case ROMFS_ERR_IO:
        return EIO;
    default:
        return EIO;
    }
}

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

static int romfs_prepare_read_file(romfs_handle_t *handle, uint32_t *position)
{
    uint32_t pos = 0;
    if (romfs_tell_file(&handle->file, &pos) != ROMFS_NOERR) {
        errno = EINVAL;
        return -1;
    }

    romfs_close_file(&handle->read_file);
    memset(&handle->read_file, 0, sizeof(handle->read_file));
    uint32_t err = romfs_open_read_view(&handle->file, &handle->read_file, NULL);
    if (err != ROMFS_NOERR) {
        errno = errno_from_romfs(err);
        return -1;
    }

    if (position) {
        *position = pos;
    }

    if (pos >= handle->read_file.entry.size) {
        return 0;
    }

    if (pos > INT32_MAX || romfs_seek_file(&handle->read_file, (int32_t)pos, SEEK_SET) != ROMFS_NOERR) {
        romfs_close_file(&handle->read_file);
        errno = EINVAL;
        return -1;
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

    int access_mode = flags & O_ACCMODE;
    bool writable = (access_mode == O_WRONLY || access_mode == O_RDWR);
    bool readable = (access_mode == O_RDONLY || access_mode == O_RDWR);
    bool create = (flags & O_CREAT) != 0;
    bool append = (flags & O_APPEND) != 0;
    bool trunc = (flags & O_TRUNC) != 0;

    if ((!readable && !writable) || (trunc && !writable)) {
        errno = EINVAL;
        return NULL;
    }

    romfs_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        errno = ENOMEM;
        return NULL;
    }

    if (writable) {
        handle->io_buffer = malloc(ROMFS_FLASH_SECTOR);
        if (!handle->io_buffer) {
            free(handle);
            errno = ENOMEM;
            return NULL;
        }
    }

    strncpy(handle->path, abs_path, sizeof(handle->path) - 1);
    handle->path[sizeof(handle->path) - 1] = '\0';

    uint32_t err = ROMFS_NOERR;
    int open_errno = 0;
    bool created = false;

    handle->readable = readable;
    handle->writable = writable;
    handle->append = append;

    romfs_entry existing;
    bool exists = romfs_get_entry_path(abs_path, &existing) == ROMFS_NOERR;
    if (exists) {
        if (create && (flags & O_EXCL)) {
            err = ROMFS_ERR_FILE_EXISTS;
        } else if (existing.attr.names.type == ROMFS_TYPE_DIR) {
            open_errno = EISDIR;
        } else if (writable &&
                   (existing.attr.names.mode != ROMFS_MODE_READWRITE ||
                    existing.attr.names.type <= ROMFS_TYPE_FLASHMAP)) {
            open_errno = EACCES;
        }
        if (open_errno != 0) {
            err = ROMFS_ERR_OPERATION;
        }
    } else if (create) {
        /* A read-only create needs a writer buffer only until publication. */
        if (!writable) {
            handle->io_buffer = malloc(ROMFS_FLASH_SECTOR);
            if (!handle->io_buffer) {
                open_errno = ENOMEM;
                err = ROMFS_ERR_OPERATION;
            }
        }
        if (err == ROMFS_NOERR) {
            /* Create also checks names reserved by unpublished writers. */
            err = romfs_create_path(abs_path, &handle->file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC,
                                    handle->io_buffer, true);
        }
        created = err == ROMFS_NOERR;
        if (err == ROMFS_ERR_FILE_EXISTS && !(flags & O_EXCL)) {
            err = ROMFS_NOERR;
        }
    }

    if (err == ROMFS_NOERR && !created) {
        if (writable) {
            err = romfs_open_write_path(abs_path, &handle->file, handle->io_buffer);
        } else {
            err = romfs_open_path(abs_path, &handle->file, NULL);
        }
    }

    if (err == ROMFS_NOERR && created && !writable) {
        /* Publish the empty file and release the writer before opening a reader. */
        err = romfs_close_file(&handle->file);
        if (err == ROMFS_NOERR) {
            err = romfs_open_path(abs_path, &handle->file, NULL);
        }
    }
    if (err == ROMFS_NOERR && trunc) {
        err = romfs_truncate_file(&handle->file, 0);
    }

    if (err != ROMFS_NOERR) {
        if (open_errno == 0) {
            open_errno = errno_from_romfs(err);
        }
        romfs_close_file(&handle->read_file);
        romfs_close_file(&handle->file);
        free(handle->io_buffer);
        free(handle);
        errno = open_errno;
        return NULL;
    }

    if (!writable) {
        free(handle->io_buffer);
        handle->io_buffer = NULL;
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

    romfs_close_file(&handle->read_file);
    uint32_t err = romfs_close_file(&handle->file);
    free(handle->io_buffer);
    free(handle);
    if (err != ROMFS_NOERR) {
        errno = errno_from_romfs(err);
        return -1;
    }
    return 0;
}

static int romfs_fs_read(void *file, uint8_t *ptr, int len)
{
    romfs_handle_t *handle = (romfs_handle_t *)file;
    if (!handle) {
        errno = EBADF;
        return -1;
    }
    if (!handle->readable) {
        errno = EBADF;
        return -1;
    }
    if (len < 0) {
        errno = EINVAL;
        return -1;
    }

    romfs_file *read_file = &handle->file;
    bool read_from_shadow = handle->file.op == ROMFS_OP_WRITE;
    uint32_t position = 0;
    if (read_from_shadow) {
        if (romfs_prepare_read_file(handle, &position) != 0) {
            return -1;
        }
        if (position >= handle->read_file.entry.size) {
            romfs_close_file(&handle->read_file);
            return 0;
        }
        read_file = &handle->read_file;
    } else if (handle->file.op != ROMFS_OP_READ) {
        errno = EBADF;
        return -1;
    }

    int ret = (int)romfs_read_file(ptr, (uint32_t)len, read_file);
    uint32_t read_err = read_file->err;
    if (ret == 0 && read_err != ROMFS_NOERR && read_err != ROMFS_ERR_EOF) {
        if (read_from_shadow) {
            romfs_close_file(&handle->read_file);
        }
        errno = errno_from_romfs(read_err);
        return -1;
    }

    if (read_from_shadow) {
        uint32_t new_position = 0;
        uint32_t tell_err = romfs_tell_file(read_file, &new_position);
        romfs_close_file(read_file);
        if (tell_err != ROMFS_NOERR ||
                new_position > INT32_MAX ||
                romfs_seek_file(&handle->file, (int32_t)new_position, SEEK_SET) != ROMFS_NOERR) {
            errno = EINVAL;
            return -1;
        }
    }

    if (read_err != ROMFS_NOERR && read_err != ROMFS_ERR_EOF) {
        errno = errno_from_romfs(read_err);
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
    if (!handle->writable || handle->file.op != ROMFS_OP_WRITE) {
        errno = EBADF;
        return -1;
    }
    if (len < 0) {
        errno = EINVAL;
        return -1;
    }
    if (handle->append && romfs_seek_file(&handle->file, 0, SEEK_END) != ROMFS_NOERR) {
        errno = EINVAL;
        return -1;
    }

    int ret = (int)romfs_write_file(ptr, (uint32_t)len, &handle->file);
    if (handle->file.err != ROMFS_NOERR) {
        errno = errno_from_romfs(handle->file.err);
        return ret > 0 ? ret : -1;
    }

    return ret;
}

static int romfs_fs_ftruncate(void *file, int length)
{
    romfs_handle_t *handle = (romfs_handle_t *)file;
    if (!handle) {
        errno = EBADF;
        return -1;
    }
    if (!handle->writable || handle->file.op != ROMFS_OP_WRITE) {
        errno = EBADF;
        return -1;
    }
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }

    uint32_t err = romfs_truncate_file(&handle->file, (uint32_t)length);
    if (err != ROMFS_NOERR) {
        errno = errno_from_romfs(err);
        return -1;
    }

    return 0;
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
    uint32_t err = romfs_get_entry_path(abs_path, &entry);
    if (err != ROMFS_NOERR) {
        errno = errno_from_romfs(err);
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

    uint32_t err = romfs_delete_path(abs_path);
    if (err != ROMFS_NOERR) {
        errno = errno_from_romfs(err);
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

    uint32_t err = romfs_mkdir_path(abs_path, true, NULL);
    if (err != ROMFS_NOERR) {
        errno = errno_from_romfs(err);
        return -1;
    }

    return 0;
}

static int romfs_fs_utimes(const char *path, const struct timeval times[2])
{
    (void)times;

    char abs_path[ROMFS_MAX_PATH_LEN];
    if (build_romfs_path(path, abs_path, sizeof(abs_path)) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    romfs_entry entry;
    uint32_t err = romfs_get_entry_path(abs_path, &entry);
    if (err != ROMFS_NOERR) {
        errno = errno_from_romfs(err);
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

static int romfs_fs_findnext2(const char *path, dir_t *dir)
{
    (void)path;
    return romfs_fs_findnext(dir);
}

int newlib_romfs_init(void)
{
    memset(romfs_dir_cookies, 0, sizeof(romfs_dir_cookies));
    if (attach_filesystem(ROMFS_PREFIX, &romfs_fs) != 0) {
        return 0;
    }
    return 1;
}

int rename(const char *oldpath, const char *newpath)
{
    if (!oldpath || !newpath) {
        errno = EINVAL;
        return -1;
    }

    char old_abs_path[ROMFS_MAX_PATH_LEN];
    char new_abs_path[ROMFS_MAX_PATH_LEN];
    if (build_romfs_path(oldpath, old_abs_path, sizeof(old_abs_path)) != 0 ||
            build_romfs_path(newpath, new_abs_path, sizeof(new_abs_path)) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    uint32_t err = romfs_rename_path(old_abs_path, new_abs_path, false);
    if (err == ROMFS_ERR_FILE_EXISTS) {
        romfs_entry old_entry;
        romfs_entry new_entry;
        uint32_t old_err = romfs_get_entry_path(old_abs_path, &old_entry);
        uint32_t new_err = romfs_get_entry_path(new_abs_path, &new_entry);
        if (old_err != ROMFS_NOERR || new_err != ROMFS_NOERR) {
            errno = errno_from_romfs(old_err != ROMFS_NOERR ? old_err : new_err);
            return -1;
        }

        if (old_entry.attr.names.type == ROMFS_TYPE_DIR || new_entry.attr.names.type == ROMFS_TYPE_DIR) {
            errno = EEXIST;
            return -1;
        }

        err = romfs_delete_path(new_abs_path);
        if (err != ROMFS_NOERR) {
            errno = errno_from_romfs(err);
            return -1;
        }

        err = romfs_rename_path(old_abs_path, new_abs_path, false);
    }

    if (err != ROMFS_NOERR) {
        errno = errno_from_romfs(err);
        return -1;
    }

    return 0;
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

    errno = errno_from_romfs(err);
    return -1;
}
