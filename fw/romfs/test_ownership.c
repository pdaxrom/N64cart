#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "romfs.h"
#include "test_flash.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d (%s): %s\n", __FILE__, __LINE__, case_name, #condition); \
        exit(1); \
    } \
} while (0)

enum { IMAGE_SIZE = 2 * ROMFS_MB, START = 65536, SLOTS = ROMFS_FLASH_SECTOR / sizeof(romfs_entry) };
static const char *case_name;
static uint16_t map[ROMFS_FLASH_SECTOR / sizeof(uint16_t)], saved_map[ROMFS_FLASH_SECTOR / sizeof(uint16_t)];
static uint8_t list[ROMFS_FLASH_SECTOR], saved_list[ROMFS_FLASH_SECTOR];
static uint8_t io[3][ROMFS_FLASH_SECTOR], *saved_flash;
static romfs_dir root;

static void setup(const char *name)
{
    case_name = name;
    CHECK(test_flash_init(IMAGE_SIZE));
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    CHECK(romfs_format());
    CHECK(romfs_dir_root(&root) == ROMFS_NOERR);
}

static void snapshot(void)
{
    memcpy(saved_map, map, sizeof(map));
    memcpy(saved_list, list, sizeof(list));
    memcpy(saved_flash, test_flash_data(), IMAGE_SIZE);
    test_flash_reset_counters();
}

static void unchanged(void)
{
    CHECK(memcmp(saved_map, map, sizeof(map)) == 0);
    CHECK(memcmp(saved_list, list, sizeof(list)) == 0);
    CHECK(memcmp(saved_flash, test_flash_data(), IMAGE_SIZE) == 0);
    for (unsigned op = 0; op < TEST_FLASH_OPERATION_COUNT; op++) {
        CHECK(test_flash_get_stats()->calls[op] == 0);
    }
}

static void create(const char *name, romfs_file *file, unsigned buffer)
{
    CHECK(romfs_create_file(name, file, 0, ROMFS_TYPE_MISC, io[buffer]) == ROMFS_NOERR);
}

static void check_contents(const char *name, const char *expected)
{
    romfs_file file;
    uint8_t data[128];
    CHECK(romfs_open_file(name, &file, io[2]) == ROMFS_NOERR);
    CHECK(file.entry.size == strlen(expected));
    CHECK(romfs_read_file(data, sizeof(data), &file) == strlen(expected));
    CHECK(memcmp(data, expected, strlen(expected)) == 0);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
}

static void pending_files(void)
{
    setup("pending files reserve slots and names");
    romfs_file a, b, attempt;
    create("alpha", &a, 0);
    create("beta", &b, 1);
    CHECK(a.nentry != b.nentry);
    snapshot();
    CHECK(romfs_create_file("alpha", &attempt, 0, ROMFS_TYPE_MISC, io[2]) == ROMFS_ERR_FILE_EXISTS);
    CHECK(romfs_open_append("alpha", &attempt, ROMFS_TYPE_MISC, io[2]) == ROMFS_ERR_BUSY);
    CHECK(romfs_open_file("alpha", &attempt, io[2]) == ROMFS_ERR_BUSY);
    CHECK(romfs_dir_create(&root, "alpha", NULL) == ROMFS_ERR_FILE_EXISTS);
    CHECK(romfs_delete("alpha") == ROMFS_ERR_BUSY);
    CHECK(romfs_rename("alpha", "gamma") == ROMFS_ERR_BUSY);
    CHECK(romfs_close_file(&attempt) == ROMFS_NOERR);
    unchanged();
    CHECK(romfs_write_file("AAAA", 4, &a) == 4);
    CHECK(romfs_write_file("BBBBBB", 6, &b) == 6);
    CHECK(a.entry.start != b.entry.start);
    CHECK(romfs_close_file(&b) == ROMFS_NOERR);
    CHECK(romfs_rename("beta", "alpha") == ROMFS_ERR_FILE_EXISTS);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    check_contents("alpha", "AAAA");
    check_contents("beta", "BBBBBB");
    puts("PASS pending files survive reverse close and remount");
}

static void exclusive_writers(void)
{
    setup("exclusive writers, shared readers and stale descriptors");
    romfs_file a, b, c;
    create("data", &a, 0);
    CHECK(romfs_write_file("original", 8, &a) == 8);
    CHECK(romfs_flush_file(&a) == ROMFS_NOERR);
    romfs_file copy = a;
    snapshot();
    CHECK(romfs_open_append("data", &b, ROMFS_TYPE_MISC, io[1]) == ROMFS_ERR_BUSY);
    CHECK(romfs_open_file("data", &b, io[1]) == ROMFS_ERR_BUSY);
    CHECK(romfs_delete("data") == ROMFS_ERR_BUSY);
    CHECK(romfs_rename("data", "renamed") == ROMFS_ERR_BUSY);
    CHECK(romfs_create_path("/new-parent/child", &a, 0, ROMFS_TYPE_MISC, io[1], true) == ROMFS_ERR_BUSY);
    CHECK(romfs_open_file("data", &a, io[1]) == ROMFS_ERR_BUSY);
    CHECK(romfs_open_append("data", &a, ROMFS_TYPE_MISC, io[1]) == ROMFS_ERR_BUSY);
    CHECK(romfs_list(&a, true) == ROMFS_ERR_BUSY);
    CHECK(a.err == ROMFS_NOERR && a.io_buffer == io[0]);
    CHECK(romfs_write_file("bad", 3, &copy) == 0 && copy.err == ROMFS_ERR_OPERATION);
    CHECK(romfs_close_file(&copy) == ROMFS_NOERR);
    unchanged();
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    CHECK(romfs_open_file("data", &a, io[0]) == ROMFS_NOERR);
    CHECK(romfs_open_file("data", &b, io[1]) == ROMFS_NOERR);
    snapshot();
    CHECK(romfs_open_append("data", &c, ROMFS_TYPE_MISC, io[2]) == ROMFS_ERR_BUSY);
    CHECK(romfs_delete("data") == ROMFS_ERR_BUSY);
    CHECK(romfs_rename("data", "renamed") == ROMFS_ERR_BUSY);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    CHECK(romfs_open_append("data", &c, ROMFS_TYPE_MISC, io[2]) == ROMFS_ERR_BUSY);
    CHECK(romfs_close_file(&b) == ROMFS_NOERR);
    unchanged();
    CHECK(romfs_rename("data", "renamed") == ROMFS_NOERR);
    snapshot();
    CHECK(romfs_close_file(&copy) == ROMFS_NOERR);
    CHECK(romfs_flush_file(&copy) == ROMFS_ERR_OPERATION);
    CHECK(romfs_truncate_file(&copy, 0) == ROMFS_ERR_OPERATION);
    unchanged();
    check_contents("renamed", "original");
    CHECK(romfs_delete("renamed") == ROMFS_NOERR);
    create("replacement", &c, 2);
    CHECK(romfs_close_file(&copy) == ROMFS_NOERR);
    CHECK(romfs_close_file(&c) == ROMFS_NOERR);
    CHECK(romfs_get_entry("data", &copy.entry) == ROMFS_ERR_NO_ENTRY);
    puts("PASS handle conflicts and stale copies cannot change catalog or data");
}

static void read_views(void)
{
    setup("temporary read views");
    romfs_file writer, reader, attempt;
    create("data", &writer, 0);
    CHECK(romfs_write_file("abc", 3, &writer) == 3);
    CHECK(romfs_open_read_view(&writer, &reader, io[1]) == ROMFS_NOERR);
    uint8_t data[8];
    CHECK(romfs_read_file(data, sizeof(data), &reader) == 3 && memcmp(data, "abc", 3) == 0);
    snapshot();
    CHECK(romfs_write_file("d", 1, &writer) == 0 && writer.err == ROMFS_ERR_BUSY);
    CHECK(romfs_flush_file(&writer) == ROMFS_ERR_BUSY);
    CHECK(romfs_truncate_file(&writer, 0) == ROMFS_ERR_BUSY);
    CHECK(romfs_open_read_view(&writer, &attempt, io[2]) == ROMFS_ERR_BUSY);
    CHECK(romfs_open_read_view(&writer, &writer, io[2]) == ROMFS_ERR_BUSY);
    CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    unchanged();
    CHECK(romfs_write_file("d", 1, &writer) == 1);
    CHECK(romfs_open_read_view(&writer, &reader, io[1]) == ROMFS_NOERR);
    CHECK(romfs_close_file(&writer) == ROMFS_ERR_BUSY);
    CHECK(romfs_open_append("data", &attempt, ROMFS_TYPE_MISC, io[2]) == ROMFS_ERR_BUSY);
    CHECK(romfs_read_file(data, sizeof(data), &reader) == 4 && memcmp(data, "abcd", 4) == 0);
    CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    CHECK(romfs_open_append("data", &attempt, ROMFS_TYPE_MISC, io[2]) == ROMFS_NOERR);
    CHECK(romfs_close_file(&attempt) == ROMFS_NOERR);
    check_contents("data", "abcd");
    puts("PASS read views freeze the writer and release ownership on close");
}

static void failed_handles(void)
{
    setup("failed opens and closes release reservations");
    romfs_file a, b, pending[SLOTS];
    snapshot();
    CHECK(romfs_create_file("failed", &a, 0, ROMFS_TYPE_MISC, NULL) == ROMFS_ERR_NO_IO_BUFFER);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    unchanged();
    create("failed", &a, 0);
    uint32_t slot = a.nentry;
    CHECK(romfs_write_file(NULL, 1, &a) == 0 && a.err == ROMFS_ERR_OPERATION);
    CHECK(romfs_close_file(&a) == ROMFS_ERR_OPERATION);
    create("failed", &b, 1);
    CHECK(b.nentry == slot);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    CHECK(romfs_close_file(&b) == ROMFS_NOERR);
    unsigned count = 0;
    for (; count < SLOTS; count++) {
        char name[32];
        snprintf(name, sizeof(name), "pending-%u", count);
        uint32_t err = romfs_create_file(name, &pending[count], 0, ROMFS_TYPE_MISC, io[0]);
        if (err == ROMFS_ERR_NO_FREE_ENTRIES) {
            break;
        }
        CHECK(err == ROMFS_NOERR);
        for (unsigned j = 0; j < count; j++) {
            CHECK(pending[j].nentry != pending[count].nentry);
        }
    }
    CHECK(count == SLOTS - 4);
    CHECK(romfs_dir_create(&root, "no-slot", NULL) == ROMFS_ERR_NO_FREE_ENTRIES);
    CHECK(romfs_write_file(NULL, 1, &pending[0]) == 0);
    CHECK(romfs_close_file(&pending[0]) == ROMFS_ERR_OPERATION);
    romfs_dir dir;
    CHECK(romfs_dir_create(&root, "now-free", &dir) == ROMFS_NOERR && dir.id == 1);
    for (unsigned i = count; i > 1; i--) {
        CHECK(romfs_close_file(&pending[i - 1]) == ROMFS_NOERR);
    }
    puts("PASS failed create/close and full catalog release slots, names and directory IDs");
}

static void fill_catalog(void)
{
    for (unsigned i = 0; i < SLOTS; i++) {
        if (((romfs_entry *) list)[i].name[0] == ROMFS_EMPTY_ENTRY) {
            char name[32];
            snprintf(name, sizeof(name), "filler-%u", i);
            romfs_file file;
            create(name, &file, 0);
            CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        }
    }
}

static void directory_ownership(void)
{
    setup("directory IDs survive tombstone GC");
    romfs_dir old, current, extra, next;
    CHECK(romfs_dir_create(&root, "old", &old) == ROMFS_NOERR);
    CHECK(romfs_delete("old") == ROMFS_NOERR);
    CHECK(romfs_dir_create(&root, "current", &current) == ROMFS_NOERR);
    CHECK(current.id == old.id);
    fill_catalog();
    CHECK(romfs_dir_create(&root, "extra", &extra) == ROMFS_NOERR);
    CHECK(extra.id != current.id);
    CHECK(romfs_dir_open(&root, "current", &next) == ROMFS_NOERR);
    CHECK(next.id == current.id && next.generation == current.generation);
    snapshot();
    CHECK(romfs_dir_remove(&old) == ROMFS_ERR_DIR_INVALID);
    CHECK(romfs_dir_create(&old, "misplaced", NULL) == ROMFS_ERR_DIR_INVALID);
    unchanged();
    CHECK(romfs_dir_remove(&extra) == ROMFS_NOERR);
    CHECK(romfs_dir_create(&root, "next", &next) == ROMFS_NOERR);
    CHECK(next.id == extra.id && next.entry_index == extra.entry_index);
    CHECK(next.generation != extra.generation);
    CHECK(romfs_dir_remove(&extra) == ROMFS_ERR_DIR_INVALID);
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    CHECK(romfs_dir_remove(&current) == ROMFS_ERR_DIR_INVALID);
    CHECK(romfs_dir_open(&root, "current", &current) == ROMFS_NOERR);
    CHECK(romfs_dir_open(&root, "next", &next) == ROMFS_NOERR);
    CHECK(current.id != next.id);
    puts("PASS GC preserves reused directory IDs; stale handles fail after slot reuse and remount");

    setup("pending children and directory moves");
    romfs_file child, attempt;
    CHECK(romfs_mkdir_path("/parent/nested", true, &current) == ROMFS_NOERR);
    CHECK(romfs_create_file_in_dir(&current, "child", &child, 0, ROMFS_TYPE_MISC, io[0]) == ROMFS_NOERR);
    snapshot();
    CHECK(romfs_dir_remove(&current) == ROMFS_ERR_DIR_NOT_EMPTY);
    CHECK(romfs_rmdir_path("/parent/nested") == ROMFS_ERR_DIR_NOT_EMPTY);
    CHECK(romfs_delete_path("/parent/nested") == ROMFS_ERR_DIR_NOT_EMPTY);
    CHECK(romfs_rename_path("/parent", "/moved", false) == ROMFS_ERR_BUSY);
    unchanged();
    CHECK(romfs_close_file(&child) == ROMFS_NOERR);
    CHECK(romfs_open_path("/parent/nested/child", &child, io[0]) == ROMFS_NOERR);
    CHECK(romfs_rename_path("/parent", "/moved", false) == ROMFS_ERR_BUSY);
    CHECK(romfs_close_file(&child) == ROMFS_NOERR);
    CHECK(romfs_rename_path("/parent", "/moved", false) == ROMFS_NOERR);
    CHECK(romfs_open_path("/moved/nested/child", &attempt, io[0]) == ROMFS_NOERR);
    CHECK(romfs_close_file(&attempt) == ROMFS_NOERR);
    puts("PASS pending children prevent removal and open descendants prevent directory rename");
}

static void invalid_names(void)
{
    setup("uniform names and file/directory collisions");
    romfs_file file, attempt;
    romfs_dir dir;
    romfs_entry entry;
    create("source", &file, 0);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    char long_name[ROMFS_MAX_NAME_LEN + 1];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    const char *invalid[] = {"", ".", "..", "a/b", "\xffhidden", "\xfehidden", long_name};
    snapshot();
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        const char *name = invalid[i];
        CHECK(romfs_create_file(name, &attempt, 0, ROMFS_TYPE_MISC, io[0]) == ROMFS_ERR_FILE_DATA_TOO_BIG);
        CHECK(romfs_open_append(name, &attempt, ROMFS_TYPE_MISC, io[0]) == ROMFS_ERR_FILE_DATA_TOO_BIG);
        CHECK(romfs_open_file(name, &attempt, io[0]) == ROMFS_ERR_FILE_DATA_TOO_BIG);
        CHECK(romfs_dir_create(&root, name, &dir) == ROMFS_ERR_FILE_DATA_TOO_BIG);
        CHECK(romfs_dir_open(&root, name, &dir) == ROMFS_ERR_FILE_DATA_TOO_BIG);
        CHECK(romfs_delete(name) == ROMFS_ERR_FILE_DATA_TOO_BIG);
        CHECK(romfs_get_entry(name, &entry) == ROMFS_ERR_FILE_DATA_TOO_BIG);
        CHECK(romfs_rename("source", name) == ROMFS_ERR_FILE_DATA_TOO_BIG);
        CHECK(romfs_rename(name, "dest") == ROMFS_ERR_FILE_DATA_TOO_BIG);
    }
    CHECK(romfs_rename("absent", "absent") == ROMFS_ERR_NO_ENTRY);
    const char *paths[] = {"/new/.", "/new/..", "/new//child", "/new/child/", "/new/\xffhidden"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        CHECK(romfs_create_path(paths[i], &attempt, 0, ROMFS_TYPE_MISC, io[0], true) == ROMFS_ERR_FILE_DATA_TOO_BIG);
        CHECK(romfs_mkdir_path(paths[i], true, &dir) == ROMFS_ERR_FILE_DATA_TOO_BIG);
        CHECK(romfs_rename_path("/source", paths[i], true) == ROMFS_ERR_FILE_DATA_TOO_BIG);
    }
    unchanged();
    CHECK(romfs_dir_create(&root, "directory", &dir) == ROMFS_NOERR);
    snapshot();
    CHECK(romfs_create_file("directory", &attempt, 0, ROMFS_TYPE_MISC, io[0]) == ROMFS_ERR_FILE_EXISTS);
    CHECK(romfs_open_append("directory", &attempt, ROMFS_TYPE_MISC, io[0]) == ROMFS_ERR_OPERATION);
    CHECK(romfs_dir_create(&root, "source", NULL) == ROMFS_ERR_FILE_EXISTS);
    CHECK(romfs_create_file("fake-directory", &attempt, 0, ROMFS_TYPE_DIR, io[0]) == ROMFS_ERR_OPERATION);
    unchanged();
    long_name[ROMFS_MAX_NAME_LEN - 1] = '\0';
    create(long_name, &file, 0);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    CHECK(romfs_get_entry(long_name, &entry) == ROMFS_NOERR);
    puts("PASS invalid names have no side effects; 53-byte names and shared namespace work");
}

static void mount_invalidation(void)
{
    setup("mount and format invalidate file ownership");
    romfs_file a, b;
    create("data", &a, 0);
    CHECK(romfs_flush_file(&a) == ROMFS_NOERR);
    CHECK(!romfs_start(START, 1, map, list));
    CHECK(romfs_open_file("data", &b, io[1]) == ROMFS_ERR_BUSY);
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    snapshot();
    CHECK(romfs_write_file("bad", 3, &a) == 0 && a.err == ROMFS_ERR_OPERATION);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    unchanged();
    CHECK(romfs_open_file("data", &a, io[0]) == ROMFS_NOERR);
    CHECK(romfs_format());
    snapshot();
    uint8_t data;
    CHECK(romfs_read_file(&data, 1, &a) == 0 && a.err == ROMFS_ERR_OPERATION);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    unchanged();
    create("data", &b, 1);
    CHECK(romfs_close_file(&b) == ROMFS_NOERR);
    puts("PASS invalid mount preserves locks; successful mount/format invalidates old files");
}

int main(void)
{
    saved_flash = malloc(IMAGE_SIZE);
    if (!saved_flash) {
        return 1;
    }
    pending_files();
    exclusive_writers();
    read_views();
    failed_handles();
    directory_ownership();
    invalid_names();
    mount_invalidation();
    free(saved_flash);
    test_flash_destroy();
    return 0;
}
