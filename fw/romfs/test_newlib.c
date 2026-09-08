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
    CHECK(test_flash_fail_on(TEST_FLASH_WRITE, 2));
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
    mixed_read_write();
    busy_handles();
    pending_and_failed_handles();
    stale_directory_cookie();
    io_errors_and_partial_transfers();
    test_flash_destroy();
    return 0;
}
