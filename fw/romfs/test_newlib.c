#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system.h>

#include "romfs.h"
#include "newlib-romfs.h"
#include "test_flash.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d (%s): %s (errno=%d)\n", __FILE__, __LINE__, case_name, #condition, errno); \
        exit(1); \
    } \
} while (0)

enum { IMAGE_SIZE = 2 * ROMFS_MB, START = 65536 };
static const char *case_name = "attach";
static uint16_t map[ROMFS_FLASH_SECTOR / sizeof(uint16_t)];
static uint8_t list[ROMFS_FLASH_SECTOR];
static uint8_t saved_image[IMAGE_SIZE], saved_list[sizeof(list)], saved_map[sizeof(map)];
static filesystem_t *fs;

int romfs_test_rename(const char *oldpath, const char *newpath);
int romfs_test_rmdir(const char *path);

int attach_filesystem(const char *const prefix, filesystem_t *filesystem)
{
    CHECK(strcmp(prefix, "romfs:/") == 0);
    fs = filesystem;
    return 0;
}

static void setup(const char *name)
{
    case_name = name;
    CHECK(test_flash_init(IMAGE_SIZE));
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    CHECK(romfs_format());
    CHECK(newlib_romfs_init() == 1 && fs != NULL && !fs->thread_safe);
}

static void create(char *name, char *text)
{
    void *handle = fs->open(name, O_CREAT | O_WRONLY);
    CHECK(handle != NULL);
    CHECK(fs->write(handle, (uint8_t *) text, (int) strlen(text)) == (int) strlen(text));
    CHECK(fs->close(handle) == 0);
}

static void check_contents(char *name, const char *text)
{
    void *handle = fs->open(name, O_RDONLY);
    CHECK(handle != NULL);
    uint8_t data[128];
    CHECK(fs->read(handle, data, sizeof(data)) == (int) strlen(text));
    CHECK(memcmp(data, text, strlen(text)) == 0);
    CHECK(fs->close(handle) == 0);
}

static void snapshot(void)
{
    memcpy(saved_image, test_flash_data(), sizeof(saved_image));
    memcpy(saved_list, list, sizeof(list));
    memcpy(saved_map, map, sizeof(map));
    test_flash_reset_counters();
}

static void unchanged(void)
{
    CHECK(memcmp(saved_image, test_flash_data(), sizeof(saved_image)) == 0);
    CHECK(memcmp(saved_list, list, sizeof(list)) == 0);
    CHECK(memcmp(saved_map, map, sizeof(map)) == 0);
    for (unsigned op = 0; op < TEST_FLASH_OPERATION_COUNT; op++) {
        CHECK(test_flash_get_stats()->calls[op] == 0);
    }
}

static uint32_t entry_info(const char *path, romfs_entry *entry)
{
    romfs_file file = {0};
    uint8_t io[ROMFS_FLASH_SECTOR];
    CHECK(romfs_open_path(path, &file, io) == ROMFS_NOERR);
    *entry = file.entry;
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    return file.nentry;
}

static uint32_t create_typed_file(const char *path, romfs_entry *entry)
{
    romfs_file file = {0};
    uint8_t io[ROMFS_FLASH_SECTOR];
    CHECK(romfs_create_path(path, &file, ROMFS_MODE_READWRITE, 7, io, true) == ROMFS_NOERR);
    CHECK(romfs_write_file("old", 3, &file) == 3 && file.err == ROMFS_NOERR);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    *entry = file.entry;
    return file.nentry;
}

static void open_flag_matrix(void)
{
    const int access_modes[] = {O_RDONLY, O_WRONLY, O_RDWR, O_ACCMODE};
    unsigned cases = 0;
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        bool exists = scenario == 2;
        for (unsigned access = 0; access < 4; access++) {
            for (unsigned bits = 0; bits < 16; bits++) {
                char label[96];
                snprintf(label, sizeof(label), "open matrix scenario=%u access=%u flags=%u", scenario, access, bits);
                setup(label);
                if (scenario == 1) {
                    CHECK(fs->mkdir("parent", 0777) == 0);
                }
                bool create_flag = bits & 1, excl = bits & 2, trunc = bits & 4, append = bits & 8;
                int flags = access_modes[access] | (create_flag ? O_CREAT : 0) | (excl ? O_EXCL : 0) |
                            (trunc ? O_TRUNC : 0) | (append ? O_APPEND : 0);
                romfs_entry before = {0}, after;
                uint32_t slot = exists ? create_typed_file("parent/data", &before) : 0;
                int expected_errno = 0;
                if (access == 3 || (access == 0 && trunc)) {
                    expected_errno = EINVAL;
                } else if (exists && create_flag && excl) {
                    expected_errno = EEXIST;
                } else if (!exists && !create_flag) {
                    expected_errno = ENOENT;
                }
                snapshot();
                errno = EDOM; /* An earlier errno must not override an open failure. */
                void *handle = fs->open("parent/data", flags);
                cases++;
                if (expected_errno != 0) {
                    CHECK(handle == NULL && errno == expected_errno);
                    unchanged();
                    if (exists) {
                        check_contents("parent/data", "old");
                    } else if (scenario == 0) {
                        CHECK(romfs_get_entry_path("parent", &after) == ROMFS_ERR_NO_ENTRY);
                    } else {
                        CHECK(romfs_get_entry_path("parent/data", &after) == ROMFS_ERR_NO_ENTRY);
                    }
                    continue;
                }
                CHECK(handle != NULL);
                CHECK(fs->lseek(handle, 0, SEEK_CUR) == 0);
                struct stat st;
                int original_size = exists && !trunc ? 3 : 0;
                CHECK(fs->fstat(handle, &st) == 0 && st.st_size == original_size);
                uint8_t output[8];
                if (access == 1) {
                    CHECK(fs->read(handle, output, 1) == -1 && errno == EBADF);
                } else {
                    CHECK(fs->read(handle, output, sizeof(output)) == original_size);
                    CHECK(memcmp(output, "old", original_size) == 0);
                    CHECK(fs->lseek(handle, 0, SEEK_SET) == 0);
                }
                const char *expected = original_size ? "old" : "";
                if (access == 0) {
                    CHECK(fs->write(handle, (uint8_t *) "X", 1) == -1 && errno == EBADF);
                    CHECK(fs->ftruncate(handle, 0) == -1 && errno == EBADF);
                } else {
                    CHECK(fs->write(handle, (uint8_t *) "X", 1) == 1);
                    expected = original_size ? (append ? "oldX" : "Xld") : "X";
                    CHECK(fs->lseek(handle, 0, SEEK_CUR) == (append ? original_size + 1 : 1));
                }
                CHECK(fs->close(handle) == 0);
                CHECK(romfs_start(START, IMAGE_SIZE, map, list));
                check_contents("parent/data", expected);
                uint32_t after_slot = entry_info("parent/data", &after);
                if (exists) {
                    CHECK(after_slot == slot && after.attr.raw == before.attr.raw);
                    CHECK(memcmp(after.name, before.name, sizeof(before.name)) == 0);
                    if (!trunc) {
                        CHECK(after.start == before.start);
                    }
                } else {
                    CHECK(after.attr.names.type == ROMFS_TYPE_MISC && after.attr.names.parent != 0);
                }
            }
        }
    }
    printf("PASS newlib %u open flag cases: existence, access, create/excl/trunc/append, bytes and attributes\n", cases);
}

static void append_after_seek_and_read(void)
{
    for (unsigned trunc = 0; trunc < 2; trunc++) {
        setup("append follows EOF after seek, read and truncate");
        create("data", "old");
        void *handle = fs->open("data", O_RDWR | O_APPEND | (trunc ? O_TRUNC : 0));
        CHECK(handle != NULL && fs->lseek(handle, 0, SEEK_CUR) == 0);
        CHECK(fs->lseek(handle, 100, SEEK_SET) == 100);
        CHECK(fs->write(handle, (uint8_t *) "X", 1) == 1);
        CHECK(fs->lseek(handle, 0, SEEK_CUR) == (trunc ? 1 : 4));
        CHECK(fs->lseek(handle, 0, SEEK_SET) == 0);
        uint8_t ch;
        CHECK(fs->read(handle, &ch, 1) == 1 && ch == (trunc ? 'X' : 'o'));
        CHECK(fs->write(handle, (uint8_t *) "Y", 1) == 1);
        CHECK(fs->close(handle) == 0);
        check_contents("data", trunc ? "XY" : "oldXY");
        handle = fs->open("data", O_WRONLY | O_APPEND);
        CHECK(handle != NULL);
        CHECK(fs->ftruncate(handle, 1) == 0);
        CHECK(fs->lseek(handle, 50, SEEK_SET) == 50);
        CHECK(fs->write(handle, (uint8_t *) "Z", 1) == 1);
        CHECK(fs->close(handle) == 0);
        check_contents("data", trunc ? "XZ" : "oZ");
    }
    puts("PASS newlib append at every write after seek/read/truncate, including O_APPEND | O_TRUNC");
}

static void exclusive_pending_and_truncate_busy(void)
{
    setup("exclusive create of pending file and busy truncate");
    void *writer = fs->open("pending", O_CREAT | O_RDWR);
    CHECK(writer != NULL && fs->write(writer, (uint8_t *) "kept", 4) == 4);
    const int access_modes[] = {O_RDONLY, O_WRONLY, O_RDWR};
    for (unsigned access = 0; access < 3; access++) {
        for (unsigned bits = 0; bits < 8; bits++) {
            int flags = access_modes[access] | (bits & 1 ? O_CREAT : 0) |
                        (bits & 2 ? O_EXCL : 0) | (bits & 4 ? O_APPEND : 0);
            snapshot();
            int expected = (bits & 3) == 3 ? EEXIST : EBUSY;
            CHECK(fs->open("pending", flags) == NULL && errno == expected);
            unchanged();
        }
    }
    CHECK(fs->close(writer) == 0);
    check_contents("pending", "kept");
    for (unsigned access = 0; access < 3; access++) {
        void *owner = fs->open("pending", access_modes[access]);
        CHECK(owner != NULL);
        snapshot();
        CHECK(fs->open("pending", O_RDWR | O_CREAT | O_TRUNC | O_APPEND) == NULL && errno == EBUSY);
        unchanged();
        CHECK(fs->close(owner) == 0);
        check_contents("pending", "kept");
    }
    puts("PASS newlib pending O_EXCL returns EEXIST; rejected opens/truncate leave owners and data intact");
}

static void protected_open_flags(void)
{
    setup("protected file and directory flag checks");
    const uint16_t modes[] = {ROMFS_MODE_READONLY, ROMFS_MODE_SYSTEM, ROMFS_MODE_RESERVED};
    char *names[] = {"readonly", "system", "reserved", "firmware", "flashlist", "flashmap", "dir", "/"};
    for (unsigned i = 0; i < 3; i++) {
        romfs_entry entry;
        uint32_t slot = create_typed_file(names[i], &entry);
        /* Fixture edit of the persisted little-endian mode bits, then remount. */
        test_flash_data()[START + slot * sizeof(romfs_entry) + ROMFS_MAX_NAME_LEN] |= modes[i];
        CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    }
    CHECK(fs->mkdir("dir", 0777) == 0);
    const int access_modes[] = {O_WRONLY, O_RDWR};
    for (unsigned n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        for (unsigned access = 0; access < 2; access++) {
            for (unsigned bits = 0; bits < 16; bits++) {
                int flags = access_modes[access] | (bits & 1 ? O_CREAT : 0) | (bits & 2 ? O_EXCL : 0) |
                            (bits & 4 ? O_TRUNC : 0) | (bits & 8 ? O_APPEND : 0);
                int expected = (bits & 3) == 3 ? EEXIST : (n >= 6 ? EISDIR : EACCES);
                snapshot();
                CHECK(fs->open(names[n], flags) == NULL && errno == expected);
                unchanged();
            }
        }
        snapshot();
        CHECK(fs->open(names[n], O_RDONLY | O_CREAT | O_EXCL) == NULL && errno == EEXIST);
        unchanged();
        void *reader = fs->open(names[n], O_RDONLY | O_CREAT);
        if (n >= 6) {
            CHECK(reader == NULL && errno == EISDIR);
        } else {
            CHECK(reader != NULL && fs->close(reader) == 0);
        }
        unchanged();
        if (n < 3) {
            check_contents(names[n], "old");
        }
    }
    puts("PASS newlib protected modes/services and directories reject opens before any flash or metadata change");
}

static void open_io_failure_cleanup(void)
{
    for (unsigned operation = TEST_FLASH_ERASE; operation <= TEST_FLASH_WRITE; operation++) {
        for (unsigned nth = 1; nth <= 2; nth++) {
            setup("readonly create metadata failure releases temporary writer");
            CHECK(test_flash_fail_on(operation, nth));
            errno = EDOM;
            CHECK(fs->open("empty", O_RDONLY | O_CREAT | O_EXCL) == NULL && errno == EIO);
            CHECK(test_flash_get_stats()->injected == 1);
            CHECK(romfs_sync() == ROMFS_NOERR);
            check_contents("empty", "");
            CHECK(fs->unlink("empty") == 0);
            setup("truncate metadata failure releases writer and preserves errno");
            create("data", "old");
            CHECK(test_flash_fail_on(operation, nth));
            errno = EDOM;
            CHECK(fs->open("data", O_RDWR | O_TRUNC | O_APPEND) == NULL && errno == EIO);
            CHECK(test_flash_get_stats()->injected == 1);
            CHECK(romfs_sync() == ROMFS_NOERR);
            CHECK(romfs_start(START, IMAGE_SIZE, map, list));
            check_contents("data", ""); /* I/O failure does not roll back the RAM truncation. */
            void *handle = fs->open("data", O_RDWR);
            CHECK(handle != NULL && fs->close(handle) == 0);
        }
    }
    puts("PASS newlib open reports metadata EIO and releases handles after readonly create/truncate failures");
}

static void mixed_read_write(void)
{
    setup("O_RDWR read views and alternating operations");
    void *handle = fs->open("mixed", O_CREAT | O_RDWR);
    CHECK(handle != NULL);
    uint8_t expected[2 * ROMFS_FLASH_SECTOR + 17], actual[sizeof(expected)];
    for (unsigned i = 0; i < sizeof(expected); i++) {
        expected[i] = (uint8_t) (i * 37 + 11);
    }
    CHECK(fs->write(handle, expected, sizeof(expected)) == sizeof(expected));
    CHECK(fs->read(handle, actual, sizeof(actual)) == 0);
    for (unsigned i = 0; i < 24; i++) {
        unsigned offset = (i * 397) % (sizeof(expected) - 13);
        CHECK(fs->lseek(handle, (int) offset, SEEK_SET) == (int) offset);
        for (unsigned j = 0; j < 13; j++) {
            expected[offset + j] = (uint8_t) (i + j);
        }
        CHECK(fs->write(handle, expected + offset, 13) == 13);
        CHECK(fs->lseek(handle, 0, SEEK_SET) == 0);
        CHECK(fs->read(handle, actual, sizeof(actual)) == sizeof(actual));
        CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
        CHECK(fs->read(handle, actual, 1) == 0);
        CHECK(fs->read(handle, actual, 0) == 0);
    }
    CHECK(fs->lseek(handle, 0, SEEK_SET) == 0);
    CHECK(fs->read(handle, NULL, 1) == -1 && errno == EINVAL);
    CHECK(fs->write(handle, expected, 1) == 1);
    CHECK(fs->ftruncate(handle, 7) == 0);
    CHECK(fs->lseek(handle, 0, SEEK_SET) == 0);
    CHECK(fs->read(handle, actual, sizeof(actual)) == 7 && memcmp(actual, expected, 7) == 0);
    struct stat st;
    CHECK(fs->fstat(handle, &st) == 0 && st.st_size == 7);
    CHECK(fs->close(handle) == 0);
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    handle = fs->open("mixed", O_RDONLY);
    CHECK(handle != NULL);
    CHECK(fs->read(handle, actual, sizeof(actual)) == 7 && memcmp(actual, expected, 7) == 0);
    CHECK(fs->close(handle) == 0);
    puts("PASS newlib O_RDWR alternating read/write/seek, EOF, read error, truncate and remount");
}

static void busy_handles(void)
{
    setup("newlib EBUSY and rename replacement");
    create("source", "source-data");
    create("target", "target-data");
    void *writer = fs->open("source", O_RDWR);
    CHECK(writer != NULL);
    for (unsigned i = 0; i < 32; i++) {
        CHECK(fs->open("source", O_RDONLY) == NULL && errno == EBUSY);
        CHECK(fs->open("source", O_WRONLY) == NULL && errno == EBUSY);
        CHECK(fs->unlink("source") == -1 && errno == EBUSY);
        CHECK(romfs_test_rename("source", "target") == -1 && errno == EBUSY);
        CHECK(romfs_test_rename("target", "source") == -1 && errno == EBUSY);
        check_contents("target", "target-data");
        CHECK(fs->open("/invalid-parent/.", O_CREAT | O_WRONLY) == NULL);
        struct stat st;
        CHECK(fs->stat("/invalid-parent", &st) == -1 && errno == ENOENT);
    }
    void *other = fs->open("target", O_WRONLY);
    CHECK(other != NULL && fs->close(other) == 0);
    CHECK(fs->close(writer) == 0);
    check_contents("source", "source-data");
    void *reader = fs->open("source", O_RDONLY);
    void *reader2 = fs->open("source", O_RDONLY);
    CHECK(reader != NULL && reader2 != NULL);
    CHECK(fs->open("source", O_WRONLY) == NULL && errno == EBUSY);
    CHECK(fs->close(reader) == 0);
    CHECK(fs->open("source", O_WRONLY) == NULL && errno == EBUSY);
    CHECK(fs->close(reader2) == 0);
    CHECK(romfs_test_rename("source", "target") == 0);
    check_contents("target", "source-data");
    CHECK(fs->open("source", O_RDONLY) == NULL && errno == ENOENT);
    puts("PASS newlib EBUSY preserves both rename endpoints and permits shared readers");
}

static void pending_and_failed_handles(void)
{
    setup("newlib pending files and failed close cleanup");
    void *first = fs->open("first", O_CREAT | O_RDWR);
    void *second = fs->open("second", O_CREAT | O_RDWR);
    CHECK(first != NULL && second != NULL);
    CHECK(fs->open("first", O_CREAT | O_RDWR) == NULL && errno == EBUSY);
    CHECK(fs->mkdir("first", 0777) == -1 && errno == EEXIST);
    CHECK(fs->write(first, (uint8_t *) "one", 3) == 3);
    CHECK(fs->write(second, (uint8_t *) "two", 3) == 3);
    CHECK(fs->close(second) == 0 && fs->close(first) == 0);
    check_contents("first", "one");
    check_contents("second", "two");
    for (unsigned i = 0; i < 32; i++) {
        void *bad = fs->open("first", O_WRONLY);
        CHECK(bad != NULL);
        CHECK(fs->write(bad, NULL, 1) == -1 && errno == EINVAL);
        CHECK(fs->close(bad) == -1 && errno == EINVAL);
        check_contents("first", "one");
        bad = fs->open("failed", O_CREAT | O_WRONLY);
        CHECK(bad != NULL);
        CHECK(fs->write(bad, NULL, 1) == -1 && errno == EINVAL);
        CHECK(fs->close(bad) == -1 && errno == EINVAL);
    }
    create("failed", "recovered");
    check_contents("failed", "recovered");
    puts("PASS newlib pending slots and failed close cleanup leave no dangling registrations");
}

static void stale_directory_cookie(void)
{
    setup("newlib stale directory cursor");
    CHECK(fs->mkdir("dir", 0777) == 0);
    create("dir/first", "one");
    create("dir/second", "two");
    dir_t dir = {0};
    CHECK(fs->findfirst("/dir", &dir) == 0);
    CHECK(fs->unlink("dir/first") == 0 && fs->unlink("dir/second") == 0);
    CHECK(romfs_test_rmdir("dir") == 0);
    CHECK(fs->mkdir("replacement", 0777) == 0);
    create("replacement/foreign", "three");
    CHECK(fs->findnext2("/dir", &dir) == -1 && errno == EIO);
    check_contents("replacement/foreign", "three");
    puts("PASS newlib directory cursor rejects a removed directory after ID reuse");
}

static void io_errors_and_partial_transfers(void)
{
    setup("newlib partial read and EOF");
    void *handle = fs->open("data", O_CREAT | O_RDWR);
    CHECK(handle != NULL);
    uint8_t data[8 * ROMFS_FLASH_SECTOR], output[sizeof(data)];
    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t) (i * 17 + i / ROMFS_FLASH_SECTOR);
    }
    CHECK(fs->write(handle, data, sizeof(data)) == sizeof(data));
    CHECK(fs->lseek(handle, 0, SEEK_SET) == 0);
    CHECK(test_flash_fail_on(TEST_FLASH_READ, 2));
    CHECK(fs->read(handle, output, sizeof(output)) == ROMFS_FLASH_SECTOR && errno == EIO);
    CHECK(fs->lseek(handle, 0, SEEK_CUR) == ROMFS_FLASH_SECTOR);
    CHECK(test_flash_fail_on(TEST_FLASH_READ, 1));
    CHECK(fs->read(handle, output + ROMFS_FLASH_SECTOR, sizeof(output) - ROMFS_FLASH_SECTOR) == -1 && errno == EIO);
    CHECK(fs->lseek(handle, 0, SEEK_CUR) == ROMFS_FLASH_SECTOR);
    CHECK(fs->read(handle, output + ROMFS_FLASH_SECTOR, sizeof(output) - ROMFS_FLASH_SECTOR) == sizeof(output) - ROMFS_FLASH_SECTOR);
    CHECK(memcmp(data, output, sizeof(data)) == 0);
    CHECK(fs->close(handle) == 0);

    setup("newlib write and metadata close failures");
    handle = fs->open("data", O_CREAT | O_RDWR);
    CHECK(handle != NULL);
    CHECK(test_flash_fail_on(TEST_FLASH_WRITE, 1));
    CHECK(fs->write(handle, data, sizeof(data)) == ROMFS_FLASH_SECTOR && errno == EIO);
    CHECK(fs->close(handle) == 0); /* Retry the retained dirty buffer. */
    handle = fs->open("data", O_RDWR);
    CHECK(handle != NULL);
    CHECK(fs->write(handle, data, 17) == 17);
    CHECK(test_flash_fail_on(TEST_FLASH_WRITE, 2)); /* Data succeeds, catalog fails. */
    CHECK(fs->close(handle) == -1 && errno == EIO);
    CHECK(romfs_sync() == ROMFS_NOERR);
    handle = fs->open("data", O_RDONLY);
    CHECK(handle != NULL);
    CHECK(fs->read(handle, output, sizeof(output)) == ROMFS_FLASH_SECTOR);
    CHECK(memcmp(data, output, ROMFS_FLASH_SECTOR) == 0);
    CHECK(fs->close(handle) == 0);

    setup("newlib ENOSPC retains prefix after close and remount");
    CHECK(test_flash_init(sizeof(data)));
    CHECK(romfs_start(0, sizeof(data), map, list));
    CHECK(romfs_format());
    uint32_t capacity = romfs_free();
    handle = fs->open("data", O_CREAT | O_RDWR);
    CHECK(handle != NULL);
    CHECK(fs->write(handle, data, sizeof(data)) == (int) capacity && errno == ENOSPC);
    CHECK(fs->write(handle, data, 1) == -1 && errno == ENOSPC);
    CHECK(fs->close(handle) == 0);
    CHECK(romfs_start(0, sizeof(data), map, list));
    handle = fs->open("data", O_RDONLY);
    CHECK(handle != NULL);
    CHECK(fs->read(handle, output, sizeof(output)) == (int) capacity);
    CHECK(memcmp(data, output, capacity) == 0);
    CHECK(fs->close(handle) == 0);
    puts("PASS newlib short read/write counts, EIO/ENOSPC, close errors and partial-file persistence");
}

int main(void)
{
    open_flag_matrix();
    append_after_seek_and_read();
    exclusive_pending_and_truncate_busy();
    protected_open_flags();
    open_io_failure_cleanup();
    mixed_read_write();
    busy_handles();
    pending_and_failed_handles();
    stale_directory_cookie();
    io_errors_and_partial_transfers();
    test_flash_destroy();
    return 0;
}
