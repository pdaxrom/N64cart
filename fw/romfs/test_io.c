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

enum { SECTOR = ROMFS_FLASH_SECTOR, PAYLOAD_SIZE = 8 * SECTOR + 77 };
static const char *case_name = "setup";
static uint32_t image_size, start;
static uint16_t map[SECTOR / 2], saved_map[SECTOR / 2];
static uint8_t list[SECTOR], saved_list[SECTOR], io[SECTOR];
static uint8_t payload[PAYLOAD_SIZE], actual[PAYLOAD_SIZE], expected[PAYLOAD_SIZE];

static void setup(const char *name, bool small)
{
    case_name = name;
    image_size = small ? 8 * SECTOR : 2 * ROMFS_MB;
    start = small ? 0 : 65536;
    CHECK(test_flash_init(image_size));
    CHECK(romfs_start(start, image_size, map, list));
    CHECK(romfs_format());
    test_flash_reset_counters();
}

static void create(romfs_file *file, const char *name, uint32_t size)
{
    CHECK(romfs_create_file(name, file, 0, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    CHECK(romfs_write_file(payload, size, file) == size && file->err == ROMFS_NOERR);
}

static void remount(void)
{
    test_flash_reset_counters();
    CHECK(romfs_start(start, image_size, map, list));
}

static void readback(const char *name, const uint8_t *data, uint32_t size)
{
    romfs_file file;
    CHECK(romfs_open_file(name, &file, io) == ROMFS_NOERR);
    CHECK(file.entry.size == size);
    memset(actual, 0xa5, sizeof(actual));
    CHECK(romfs_read_file(actual, sizeof(actual), &file) == size);
    CHECK(file.err == ROMFS_ERR_EOF);
    CHECK(memcmp(actual, data, size) == 0);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    CHECK(test_flash_get_stats()->rejected == 0);
}

static void snapshot_metadata(void)
{
    memcpy(saved_map, map, sizeof(map));
    memcpy(saved_list, list, sizeof(list));
}

static void metadata_unchanged(void)
{
    CHECK(memcmp(saved_map, map, sizeof(map)) == 0);
    CHECK(memcmp(saved_list, list, sizeof(list)) == 0);
}

static void failed_mounts(void)
{
    for (unsigned nth = 1; nth <= 2; nth++) {
        setup("failed mount disables access", false);
        romfs_file file, attempt;
        create(&file, "data", 17);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        CHECK(romfs_open_file("data", &file, io) == ROMFS_NOERR);
        test_flash_reset_counters();
        CHECK(test_flash_fail_on(TEST_FLASH_READ, nth));
        CHECK(!romfs_start(start, image_size, map, list));
        CHECK(test_flash_get_stats()->calls[TEST_FLASH_READ] == nth);
        test_flash_reset_counters();
        CHECK(romfs_list(&attempt, true) == ROMFS_ERR_OPERATION);
        CHECK(romfs_open_file("data", &attempt, io) != ROMFS_NOERR);
        CHECK(romfs_create_file("new", &attempt, 0, ROMFS_TYPE_MISC, io) != ROMFS_NOERR);
        CHECK(romfs_get_entry_path("/", &attempt.entry) != ROMFS_NOERR);
        CHECK(romfs_read_file(actual, 1, &file) == 0 && file.err == ROMFS_ERR_OPERATION);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        CHECK(romfs_sync() == ROMFS_ERR_OPERATION && !romfs_format() && romfs_free() == 0);
        for (unsigned op = 0; op < TEST_FLASH_OPERATION_COUNT; op++) {
            CHECK(test_flash_get_stats()->calls[op] == 0);
        }
        remount();
        readback("data", payload, 17);
    }
    puts("PASS mount read failures stop I/O and invalidate the incomplete mount");
}

static void metadata_failures(void)
{
    const char *names[] = {"format", "mkdir", "delete", "rmdir", "rename", "close", "flush"};
    for (unsigned action = 0; action < sizeof(names) / sizeof(names[0]); action++) {
        for (unsigned op = TEST_FLASH_ERASE; op <= TEST_FLASH_WRITE; op++) {
            for (unsigned nth = 1; nth <= 2; nth++) {
                setup(names[action], false);
                romfs_file file;
                romfs_dir root, dir;
                CHECK(romfs_dir_root(&root) == ROMFS_NOERR);
                if (action == 2 || action == 4) {
                    create(&file, "old", 17);
                    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
                } else if (action == 3) {
                    CHECK(romfs_dir_create(&root, "old", &dir) == ROMFS_NOERR);
                } else if (action >= 5) {
                    create(&file, "new", 0);
                }
                test_flash_reset_counters();
                CHECK(test_flash_fail_on((test_flash_operation) op, nth));
                uint32_t err = ROMFS_NOERR;
                switch (action) {
                case 0: CHECK(!romfs_format()); err = ROMFS_ERR_IO; break;
                case 1: err = romfs_dir_create(&root, "new", &dir); break;
                case 2: err = romfs_delete("old"); break;
                case 3: err = romfs_dir_remove(&dir); break;
                case 4: err = romfs_rename("old", "new"); break;
                case 5: err = romfs_close_file(&file); break;
                case 6: err = romfs_flush_file(&file); break;
                }
                CHECK(err == ROMFS_ERR_IO);
                CHECK(test_flash_get_stats()->injected == 1 && test_flash_get_stats()->rejected == 0);
                CHECK(test_flash_get_stats()->calls[op] == nth);
                CHECK(test_flash_get_stats()->calls[TEST_FLASH_WRITE] == (op == TEST_FLASH_ERASE ? nth - 1 : nth));
                CHECK(test_flash_fail_on((test_flash_operation) op, 1));
                CHECK(romfs_sync() == ROMFS_ERR_IO);
                if (action == 1) {
                    CHECK(romfs_dir_create(&root, "new", &dir) == ROMFS_NOERR);
                } else {
                    CHECK(romfs_sync() == ROMFS_NOERR);
                }
                if (action == 6) {
                    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
                }
                remount();
                romfs_entry entry;
                if (action == 1 || action >= 4) {
                    CHECK(romfs_get_entry("new", &entry) == ROMFS_NOERR);
                }
                CHECK(romfs_get_entry("old", &entry) == ROMFS_ERR_NO_ENTRY);
                if (action == 4) {
                    readback("new", payload, 17);
                }
                CHECK(romfs_sync() == ROMFS_NOERR);
            }
        }
    }
    puts("PASS every metadata erase/write failure reaches format/directory/flush/close callers; sync retries");
}

static void data_write_failures(void)
{
    for (unsigned op = TEST_FLASH_ERASE; op <= TEST_FLASH_WRITE; op++) {
        setup("RAM sector reservation and failed dirty buffer flush", false);
        romfs_file file;
        create(&file, "data", 0);
        uint32_t available = romfs_free();
        CHECK(test_flash_fail_on((test_flash_operation) op, 1));
        CHECK(romfs_write_file(payload, 17, &file) == 17 && file.err == ROMFS_NOERR);
        CHECK(file.entry.size == 17 && file.entry.start != 0xffff && romfs_free() == available - SECTOR);
        for (unsigned operation = 0; operation < TEST_FLASH_OPERATION_COUNT; operation++) {
            CHECK(test_flash_get_stats()->calls[operation] == 0);
        }
        snapshot_metadata();
        CHECK(romfs_flush_file(&file) == ROMFS_ERR_IO && file.err == ROMFS_ERR_IO);
        CHECK(file.buffer_dirty && file.entry_pending && memcmp(io, payload, 17) == 0);
        CHECK(test_flash_get_stats()->first_failure_offset == file.entry.start * SECTOR);
        CHECK(test_flash_get_stats()->calls[TEST_FLASH_WRITE] == (op == TEST_FLASH_ERASE ? 0 : 1));
        metadata_unchanged();
        CHECK(romfs_flush_file(&file) == ROMFS_NOERR && !file.buffer_dirty);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        remount();
        readback("data", payload, 17);

        setup("full-sector write returns accepted prefix on flush failure", false);
        create(&file, "data", 0);
        CHECK(test_flash_fail_on((test_flash_operation) op, 1));
        CHECK(romfs_write_file(payload, 2 * SECTOR, &file) == SECTOR && file.err == ROMFS_ERR_IO);
        CHECK(file.entry.size == SECTOR && file.write_offset == SECTOR && file.buffer_dirty);
        CHECK(romfs_flush_file(&file) == ROMFS_NOERR);
        CHECK(romfs_write_file(payload + SECTOR, SECTOR, &file) == SECTOR);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        remount();
        readback("data", payload, 2 * SECTOR);
    }
    puts("PASS sector reservation performs no I/O; dirty data and accepted byte counts survive flush retry");
}

static void read_failures(void)
{
    for (unsigned nth = 1; nth <= 3; nth++) {
        setup("short read and retry", false);
        romfs_file file;
        create(&file, "data", 3 * SECTOR);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        CHECK(romfs_open_file("data", &file, io) == ROMFS_NOERR);
        CHECK(test_flash_fail_on(TEST_FLASH_READ, nth));
        uint32_t accepted = (nth - 1) * SECTOR;
        CHECK(romfs_read_file(actual, 3 * SECTOR, &file) == accepted && file.err == ROMFS_ERR_IO);
        CHECK(file.read_offset == accepted && memcmp(actual, payload, accepted) == 0);
        CHECK(romfs_read_file(actual + accepted, 3 * SECTOR - accepted, &file) == 3 * SECTOR - accepted);
        CHECK(memcmp(actual, payload, 3 * SECTOR) == 0);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    }
    setup("failed read into write buffer invalidates cached sector", false);
    romfs_file file;
    create(&file, "data", 3 * SECTOR);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    CHECK(romfs_open_append("data", &file, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    CHECK(romfs_seek_file(&file, 1, SEEK_SET) == ROMFS_NOERR);
    CHECK(romfs_write_file(payload + 1, 1, &file) == 1);
    CHECK(romfs_seek_file(&file, SECTOR + 1, SEEK_SET) == ROMFS_NOERR);
    CHECK(test_flash_fail_on(TEST_FLASH_READ, 1));
    CHECK(romfs_write_file(payload, 1, &file) == 0 && file.err == ROMFS_ERR_IO);
    CHECK(file.write_offset == SECTOR + 1 && !file.buffer_from_flash && !file.buffer_dirty);
    CHECK(romfs_seek_file(&file, 1, SEEK_SET) == ROMFS_NOERR);
    CHECK(romfs_write_file(payload + 1, 1, &file) == 1);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    remount();
    readback("data", payload, 3 * SECTOR);
    puts("PASS short reads preserve offsets and invalidate failed write-buffer loads");
}

static void truncate_failures(void)
{
    for (unsigned op = TEST_FLASH_READ; op <= TEST_FLASH_WRITE; op++) {
        setup("truncate checks tail I/O before freeing links", false);
        romfs_file file;
        create(&file, "data", 3 * SECTOR);
        CHECK(romfs_flush_file(&file) == ROMFS_NOERR);
        CHECK(romfs_seek_file(&file, 123, SEEK_SET) == ROMFS_NOERR);
        snapshot_metadata();
        CHECK(test_flash_fail_on((test_flash_operation) op, 1));
        CHECK(romfs_truncate_file(&file, SECTOR + 17) == ROMFS_ERR_IO);
        CHECK(file.write_offset == 123 && file.entry.size == 3 * SECTOR);
        metadata_unchanged();
        CHECK(file.buffer_dirty == (op != TEST_FLASH_READ));
        CHECK(romfs_truncate_file(&file, SECTOR + 17) == ROMFS_NOERR);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        remount();
        readback("data", payload, SECTOR + 17);
    }
    puts("PASS truncate tail read/erase/write failures preserve chain and allow retry");
}

static void capacity_failures(void)
{
    const char *names[] = {"create ENOSPC", "append ENOSPC", "overwrite ENOSPC", "gap ENOSPC", "truncate ENOSPC"};
    for (unsigned action = 0; action < sizeof(names) / sizeof(names[0]); action++) {
        setup(names[action], true);
        uint32_t capacity = romfs_free();
        CHECK(capacity == 6 * SECTOR);
        romfs_file file;
        uint32_t initial = action == 1 || action == 2 ? SECTOR + SECTOR / 2 : 0;
        create(&file, "data", initial);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        CHECK(romfs_open_append("data", &file, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
        memcpy(expected, payload, initial);
        if (action <= 2) {
            uint32_t offset = action == 2 ? 123 : initial;
            CHECK(romfs_seek_file(&file, (int32_t) offset, SEEK_SET) == ROMFS_NOERR);
            uint32_t accepted = capacity - offset;
            CHECK(romfs_write_file(payload, sizeof(payload), &file) == accepted && file.err == ROMFS_ERR_NO_SPACE);
            memcpy(expected + offset, payload, accepted);
        } else {
            memset(expected, 0, capacity);
            CHECK(romfs_seek_file(&file, action == 3 ? (int32_t) capacity + 1 : 123, SEEK_SET) == ROMFS_NOERR);
            if (action == 3) {
                CHECK(romfs_write_file(payload, 1, &file) == 0 && file.err == ROMFS_ERR_NO_SPACE);
                CHECK(file.write_offset == capacity + 1);
            } else {
                CHECK(romfs_truncate_file(&file, capacity + 1) == ROMFS_ERR_NO_SPACE);
                CHECK(file.write_offset == 123);
            }
        }
        CHECK(file.entry.size == capacity);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        remount();
        readback("data", expected, capacity);
        CHECK(romfs_free() == 0);
        CHECK(romfs_delete("data") == ROMFS_NOERR);
        CHECK(romfs_free() == capacity);
        create(&file, "reused", capacity);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        remount();
        readback("reused", payload, capacity);
    }
    puts("PASS ENOSPC create/append/overwrite/gap/truncate preserve partial files without leaked sectors");
}

static void failed_close_cleanup(void)
{
    for (unsigned existing = 0; existing < 2; existing++) {
        setup("failed close releases unpublished allocations", false);
        uint32_t available = romfs_free();
        romfs_file file;
        create(&file, "data", existing ? SECTOR : 0);
        if (existing) {
            CHECK(romfs_flush_file(&file) == ROMFS_NOERR);
        }
        CHECK(romfs_write_file(payload, 17, &file) == 17);
        CHECK(test_flash_fail_on(TEST_FLASH_WRITE, 1));
        CHECK(romfs_flush_file(&file) == ROMFS_ERR_IO && file.buffer_dirty);
        CHECK(test_flash_fail_on(TEST_FLASH_WRITE, 1));
        CHECK(romfs_close_file(&file) == ROMFS_ERR_IO);
        CHECK(romfs_free() == available - existing * SECTOR);
        CHECK(romfs_sync() == ROMFS_NOERR);
        remount();
        if (existing) {
            readback("data", payload, SECTOR);
        } else {
            romfs_entry entry;
            CHECK(romfs_get_entry("data", &entry) == ROMFS_ERR_NO_ENTRY);
        }
        CHECK(romfs_free() == available - existing * SECTOR);
    }
    puts("PASS failed close reclaims unpublished tails and leaves published metadata retryable");
}

static void shared_metadata(void)
{
    for (unsigned fail = 0; fail < 2; fail++) {
        setup("shared metadata synchronizes other open writers", false);
        romfs_file first, second;
        uint8_t second_io[SECTOR];
        create(&first, "first", SECTOR);
        CHECK(romfs_close_file(&first) == ROMFS_NOERR);
        CHECK(romfs_open_append("first", &first, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
        CHECK(romfs_write_file(payload + SECTOR, 17, &first) == 17);
        CHECK(romfs_create_file("second", &second, 0, ROMFS_TYPE_MISC, second_io) == ROMFS_NOERR);
        if (fail) {
            CHECK(test_flash_fail_on(TEST_FLASH_WRITE, 1));
            CHECK(romfs_close_file(&second) == ROMFS_ERR_IO);
            CHECK(first.buffer_dirty);
            CHECK(romfs_sync() == ROMFS_NOERR);
        } else {
            CHECK(romfs_close_file(&second) == ROMFS_NOERR);
        }
        CHECK(!first.buffer_dirty);
        /* Deliberately remount before closing first: the shared metadata
         * commit must have persisted its matching size, chain and buffer. */
        remount();
        CHECK(romfs_close_file(&first) == ROMFS_NOERR);
        readback("first", payload, SECTOR + 17);
        readback("second", payload, 0);
    }
    puts("PASS shared metadata commits synchronize other writers and report their failures");
}

int main(void)
{
    for (unsigned i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t) (i * 37 + i / SECTOR + 11);
    }
    failed_mounts();
    metadata_failures();
    data_write_failures();
    read_failures();
    truncate_failures();
    capacity_failures();
    failed_close_cleanup();
    shared_metadata();
    test_flash_destroy();
    return 0;
}
