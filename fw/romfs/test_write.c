#include <inttypes.h>
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

enum { SECTOR = ROMFS_FLASH_SECTOR, IMAGE_SIZE = 16 * ROMFS_MB, DATA_SIZE = ROMFS_MB + 17 };
static const char *case_name = "setup";
static uint32_t image_size, start, data_start;
static uint16_t map[2 * SECTOR / sizeof(uint16_t)];
static uint8_t list[SECTOR], io[SECTOR], second_io[SECTOR];
static uint8_t payload[DATA_SIZE], expected[DATA_SIZE], actual[DATA_SIZE];
static uint64_t calls[2][TEST_FLASH_OPERATION_COUNT];
static struct {
    uint32_t sector;
    const uint8_t *contents;
} commit_guards[2];
static unsigned guard_count;

/* Link the ordinary NOR emulator with renamed callbacks, counting requests
 * here by region and checking data ordering before a metadata erase/program. */
bool test_write_backend_read(uint32_t offset, uint8_t *buffer, uint32_t size);
bool test_write_backend_erase(uint32_t offset);
bool test_write_backend_write(uint32_t offset, uint8_t *buffer);

static void observe(test_flash_operation operation, uint32_t offset)
{
    bool data = offset >= data_start;
    calls[data][operation]++;
    if (!data && operation != TEST_FLASH_READ) {
        for (unsigned i = 0; i < guard_count; i++) {
            CHECK(memcmp(test_flash_data() + commit_guards[i].sector * SECTOR,
                         commit_guards[i].contents, SECTOR) == 0);
        }
    }
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t size)
{
    observe(TEST_FLASH_READ, offset);
    return test_write_backend_read(offset, buffer, size);
}

bool romfs_flash_sector_erase(uint32_t offset)
{
    observe(TEST_FLASH_ERASE, offset);
    return test_write_backend_erase(offset);
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
    observe(TEST_FLASH_WRITE, offset);
    return test_write_backend_write(offset, buffer);
}

static void reset_counters(void)
{
    memset(calls, 0, sizeof(calls));
    test_flash_reset_counters();
}

static void setup(const char *name, bool small)
{
    case_name = name;
    guard_count = 0;
    image_size = small ? 8 * SECTOR : IMAGE_SIZE;
    start = small ? 0 : 65536;
    uint32_t map_size, list_size;
    romfs_get_buffers_sizes(image_size, &map_size, &list_size);
    CHECK(map_size <= sizeof(map) && list_size <= sizeof(list));
    data_start = start + map_size + list_size;
    CHECK(test_flash_init(image_size));
    CHECK(romfs_start(start, image_size, map, list));
    CHECK(romfs_format());
    reset_counters();
}

static void create(romfs_file *file, const char *name, uint8_t *buffer)
{
    CHECK(romfs_create_file(name, file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, buffer) == ROMFS_NOERR);
}

static void write_data(romfs_file *file, const uint8_t *data, uint32_t size)
{
    CHECK(romfs_write_file(data, size, file) == size && file->err == ROMFS_NOERR);
}

static void readback(const char *name, uint32_t size)
{
    romfs_file reader = {0};
    CHECK(romfs_open_file(name, &reader, io) == ROMFS_NOERR && reader.entry.size == size);
    CHECK(romfs_read_file(actual, sizeof(actual), &reader) == size);
    CHECK(memcmp(actual, expected, size) == 0);
    CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    CHECK(test_flash_get_stats()->rejected == 0);
}

static void remount(void)
{
    guard_count = 0;
    CHECK(romfs_start(start, image_size, map, list));
}

static void check_zero_tail(uint32_t sector, uint32_t used)
{
    for (unsigned i = used; i < SECTOR; i++) {
        CHECK(test_flash_data()[sector * SECTOR + i] == 0);
    }
}

static void measurements(bool measure_only)
{
    const char *names[] = {"new-1MiB", "new-1MiB+17", "append-partial", "overwrite", "append-aligned"};
    const uint32_t data_writes[] = {256, 257, 2, 1, 1};
    const uint32_t data_reads[] = {0, 0, 1, 1, 0};
    const uint32_t metadata_writes[] = {2, 2, 2, 0, 2};
    for (unsigned action = 0; action < 5; action++) {
        setup(names[action], false);
        romfs_file file = {0};
        create(&file, "data", io);
        uint32_t initial = action == 2 ? 17 : action == 3 ? 2 * SECTOR : action == 4 ? SECTOR : 0;
        uint32_t length = action == 0 ? ROMFS_MB : action == 1 ? DATA_SIZE : action == 2 ? SECTOR : 17;
        if (initial) {
            write_data(&file, payload, initial);
            CHECK(romfs_close_file(&file) == ROMFS_NOERR);
            CHECK(romfs_open_append("data", &file, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
        }
        memcpy(expected, payload, initial);
        uint32_t position = action == 3 ? 7 : initial;
        CHECK(romfs_seek_file(&file, position, SEEK_SET) == ROMFS_NOERR);
        reset_counters();
        write_data(&file, payload, length);
        memcpy(expected + position, payload, length);
        uint32_t size = position + length > initial ? position + length : initial;
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        printf("%s data r/e/w=%" PRIu64 "/%" PRIu64 "/%" PRIu64
               " metadata r/e/w=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n", names[action],
               calls[1][0], calls[1][1], calls[1][2], calls[0][0], calls[0][1], calls[0][2]);
        if (!measure_only) {
            CHECK(calls[1][TEST_FLASH_READ] == data_reads[action]);
            CHECK(calls[1][TEST_FLASH_ERASE] == data_writes[action]);
            CHECK(calls[1][TEST_FLASH_WRITE] == data_writes[action]);
            CHECK(calls[0][TEST_FLASH_ERASE] == metadata_writes[action] &&
                  calls[0][TEST_FLASH_WRITE] == metadata_writes[action]);
        }
        remount();
        readback("data", size);
    }
}

static void reused_sectors(void)
{
    for (unsigned action = 0; action < 3; action++) {
        setup("reused dirty sectors: partial write, gap and truncate extension", true);
        uint32_t capacity = romfs_free();
        romfs_file file = {0};
        create(&file, "old", io);
        write_data(&file, payload, capacity);
        uint32_t old_start = file.entry.start;
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        CHECK(romfs_delete("old") == ROMFS_NOERR);
        create(&file, "new", io);
        reset_counters();
        uint32_t size = action ? 2 * SECTOR + 17 : 17;
        memset(expected, 0, size);
        if (action == 0) {
            write_data(&file, payload, size);
            memcpy(expected, payload, size);
            CHECK(calls[1][TEST_FLASH_ERASE] == 0 && calls[1][TEST_FLASH_WRITE] == 0);
            CHECK(memcmp(test_flash_data() + old_start * SECTOR, payload, SECTOR) == 0);
        } else if (action == 1) {
            CHECK(romfs_seek_file(&file, size - 1, SEEK_SET) == ROMFS_NOERR);
            write_data(&file, payload, 1);
            expected[size - 1] = payload[0];
        } else {
            CHECK(romfs_truncate_file(&file, size) == ROMFS_NOERR);
        }
        CHECK(file.entry.start == old_start);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        CHECK(calls[1][TEST_FLASH_READ] == 0);
        CHECK(calls[1][TEST_FLASH_ERASE] == (size + SECTOR - 1) / SECTOR);
        CHECK(calls[1][TEST_FLASH_WRITE] == (size + SECTOR - 1) / SECTOR);
        /* The tiny image's reclaimed chain is contiguous. Check physical bytes
         * beyond EOF as well: they must not retain the old file's contents. */
        check_zero_tail(old_start + size / SECTOR, size % SECTOR);
        remount();
        readback("new", size);
        CHECK(romfs_delete("new") == ROMFS_NOERR && romfs_free() == capacity);
    }
    puts("PASS reused sectors are initialized in RAM; partial tails and gap/truncate extensions contain zeros");
}

static void shared_commit_order(bool full_sync)
{
    for (unsigned operation = TEST_FLASH_ERASE; operation <= TEST_FLASH_WRITE; operation++) {
        for (unsigned fail = 0; fail <= 2; fail++) {
            setup("two reserved sectors: data before metadata, including retries", false);
            romfs_file first = {0}, second = {0};
            create(&first, "first", io);
            create(&second, "second", second_io);
            write_data(&first, payload, 17);
            write_data(&second, payload + 17, 29);
            CHECK(first.entry.start != second.entry.start);
            CHECK(romfs_free() == IMAGE_SIZE - data_start - 2 * SECTOR);
            for (unsigned op = 0; op < TEST_FLASH_OPERATION_COUNT; op++) {
                CHECK(calls[0][op] == 0 && calls[1][op] == 0);
            }
            commit_guards[0].sector = first.entry.start;
            commit_guards[0].contents = io;
            commit_guards[1].sector = second.entry.start;
            commit_guards[1].contents = second_io;
            guard_count = 2;
            if (fail) {
                CHECK(test_flash_fail_on(operation, fail));
                CHECK((full_sync ? romfs_sync_full() : romfs_sync()) == ROMFS_ERR_IO);
                CHECK(calls[0][TEST_FLASH_ERASE] == 0 && calls[0][TEST_FLASH_WRITE] == 0);
                CHECK(first.buffer_dirty || second.buffer_dirty);
            }
            CHECK((full_sync && !fail ? romfs_sync_full() : romfs_sync()) == ROMFS_NOERR);
            CHECK(!first.buffer_dirty && !second.buffer_dirty);
            check_zero_tail(first.entry.start, 17);
            check_zero_tail(second.entry.start, 29);
            remount(); /* The shared commit must survive before either close. */
            CHECK(romfs_close_file(&first) == ROMFS_NOERR && romfs_close_file(&second) == ROMFS_NOERR);
            memcpy(expected, payload, 17);
            readback("first", 17);
            memcpy(expected, payload + 17, 29);
            readback("second", 29);
        }
    }
    puts("PASS new sectors are reserved separately and all writers' data precedes metadata on sync/retry");
}

static void extension_failures(void)
{
    for (unsigned operation = TEST_FLASH_ERASE; operation <= TEST_FLASH_WRITE; operation++) {
        for (unsigned nth = 1; nth <= 2; nth++) {
            for (unsigned truncate = 0; truncate < 2; truncate++) {
                setup("gap/truncate retry after old or new sector flush failure", false);
                /* Simulate stale contents in free sectors, without making them
                 * part of a live file. No fresh allocation may read these bytes. */
                memset(test_flash_data() + data_start, 0x6c, 3 * SECTOR);
                romfs_file file = {0};
                create(&file, "data", io);
                write_data(&file, payload, 17);
                CHECK(romfs_close_file(&file) == ROMFS_NOERR);
                CHECK(romfs_open_append("data", &file, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
                uint32_t size = 2 * SECTOR + 17;
                uint32_t position = truncate ? 7 : size - 1;
                CHECK(romfs_seek_file(&file, position, SEEK_SET) == ROMFS_NOERR);
                reset_counters();
                CHECK(test_flash_fail_on(operation, nth));
                if (truncate) {
                    CHECK(romfs_truncate_file(&file, size) == ROMFS_ERR_IO);
                } else {
                    CHECK(romfs_write_file(payload, 1, &file) == 0 && file.err == ROMFS_ERR_IO);
                }
                CHECK(file.buffer_dirty && file.entry.size == nth * SECTOR);
                CHECK(file.write_offset == position);
                CHECK(calls[0][TEST_FLASH_ERASE] == 0 && calls[0][TEST_FLASH_WRITE] == 0);
                CHECK(test_flash_get_stats()->first_failure_offset == data_start + (nth - 1) * SECTOR);
                if (truncate) {
                    CHECK(romfs_truncate_file(&file, size) == ROMFS_NOERR);
                    CHECK(file.write_offset == position);
                } else {
                    write_data(&file, payload, 1);
                }
                CHECK(romfs_close_file(&file) == ROMFS_NOERR);
                CHECK(calls[1][TEST_FLASH_READ] == 1); /* Only the original partial sector. */
                check_zero_tail(data_start / SECTOR + 2, 17);
                remount();
                memset(expected, 0, size);
                memcpy(expected, payload, 17);
                if (!truncate) {
                    expected[size - 1] = payload[0];
                }
                readback("data", size);
            }
        }
    }
    puts("PASS gap/truncate flush failures preserve offsets and dirty data; retry retains prefix and zero fill");
}

int main(int argc, char **argv)
{
    bool measure_only = argc == 2 && strcmp(argv[1], "--measure") == 0;
    if (argc != 1 && !measure_only) {
        fprintf(stderr, "Usage: %s [--measure]\n", argv[0]);
        return 2;
    }
    for (unsigned i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t) (i * 37 + i / SECTOR + 11);
    }
    measurements(measure_only);
    if (!measure_only) {
        reused_sectors();
        shared_commit_order(false);
        shared_commit_order(true);
        extension_failures();
    }
    test_flash_destroy();
    return 0;
}
