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

enum { SECTOR = ROMFS_FLASH_SECTOR, START = 65536, MAX_META = 36 };
static const char *case_name = "setup";
static uint16_t map[32 * SECTOR / sizeof(uint16_t)];
static uint8_t list[4 * SECTOR], io[SECTOR], payload[2 * SECTOR];
static uint32_t image_size, map_size, list_size, data_start;
static uint64_t calls[2][TEST_FLASH_OPERATION_COUNT];
static unsigned addresses[MAX_META][TEST_FLASH_OPERATION_COUNT];
static bool measure_only;

bool test_metadata_backend_read(uint32_t offset, uint8_t *buffer, uint32_t size);
bool test_metadata_backend_erase(uint32_t offset);
bool test_metadata_backend_write(uint32_t offset, uint8_t *buffer);

static void observe(test_flash_operation operation, uint32_t offset)
{
    CHECK(offset >= START);
    bool data = offset >= data_start;
    calls[data][operation]++;
    if (!data) {
        CHECK(offset % SECTOR == 0);
        addresses[(offset - START) / SECTOR][operation]++;
    }
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t size)
{
    observe(TEST_FLASH_READ, offset);
    return test_metadata_backend_read(offset, buffer, size);
}

bool romfs_flash_sector_erase(uint32_t offset)
{
    observe(TEST_FLASH_ERASE, offset);
    return test_metadata_backend_erase(offset);
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
    observe(TEST_FLASH_WRITE, offset);
    return test_metadata_backend_write(offset, buffer);
}

static void reset(void)
{
    memset(calls, 0, sizeof(calls));
    memset(addresses, 0, sizeof(addresses));
    test_flash_reset_counters();
}

static void remount(void)
{
    CHECK(romfs_start(START, image_size, map, list));
}

static void setup(uint32_t mb)
{
    image_size = mb * ROMFS_MB;
    romfs_get_buffers_sizes(image_size, &map_size, &list_size);
    data_start = START + map_size + list_size;
    CHECK(test_flash_init(image_size));
    remount();
    CHECK(romfs_format());
    reset();
}

static void create(romfs_file *file, const char *name, uint32_t size)
{
    CHECK(romfs_create_file(name, file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    if (size) {
        CHECK(size <= sizeof(payload));
        CHECK(romfs_write_file(payload, size, file) == size && file->err == ROMFS_NOERR);
    }
}

static void check_writes(uint32_t list_mask, uint32_t map_mask)
{
    if (measure_only) {
        return;
    }
    for (unsigned i = 0; i < (list_size + map_size) / SECTOR; i++) {
        unsigned wanted = i < list_size / SECTOR ? (list_mask >> i) & 1u :
                          (map_mask >> (i - list_size / SECTOR)) & 1u;
        CHECK(addresses[i][TEST_FLASH_ERASE] == wanted);
        CHECK(addresses[i][TEST_FLASH_WRITE] == wanted);
    }
    CHECK(test_flash_get_stats()->rejected == 0);
}

static void report(const char *name, uint32_t list_mask, uint32_t map_mask)
{
    printf("%uMiB %s data r/e/w=%" PRIu64 "/%" PRIu64 "/%" PRIu64
           " metadata r/e/w=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n",
           image_size / ROMFS_MB, name, calls[1][0], calls[1][1], calls[1][2],
           calls[0][0], calls[0][1], calls[0][2]);
    check_writes(list_mask, map_mask);
    reset();
}

static void measurements(uint32_t mb)
{
    case_name = "ordinary metadata operations";
    setup(mb);
    romfs_file file = {0};
    create(&file, "data", 17);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    report("create-17", 1, 1);
    CHECK(romfs_open_write_path("data", &file, io) == ROMFS_NOERR);
    CHECK(romfs_write_file(payload + 1, 1, &file) == 1);
    CHECK(romfs_flush_file(&file) == ROMFS_NOERR);
    report("overwrite-1", 0, 0);
    CHECK(romfs_flush_file(&file) == ROMFS_NOERR);
    report("clean-flush", 0, 0);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    report("clean-close", 0, 0);
    CHECK(romfs_open_append("data", &file, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    CHECK(romfs_write_file(payload, 1, &file) == 1);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    report("append-within-sector", 1, 0);
    CHECK(romfs_rename("data", "renamed") == ROMFS_NOERR);
    report("rename", 1, 0);
    CHECK(romfs_rename("renamed", "renamed") == ROMFS_NOERR);
    report("rename-same", 0, 0);
    CHECK(romfs_delete("renamed") == ROMFS_NOERR);
    report("delete", 1, 0);
    romfs_dir root, dir;
    CHECK(romfs_dir_root(&root) == ROMFS_NOERR);
    CHECK(romfs_dir_create(&root, "dir", &dir) == ROMFS_NOERR);
    report("mkdir", 1, 0);
    CHECK(romfs_dir_create(&root, "dir", &dir) == ROMFS_NOERR);
    report("mkdir-existing", 0, 0);
    CHECK(romfs_dir_remove(&dir) == ROMFS_NOERR);
    report("rmdir", 1, 0);
    remount();
    romfs_entry entry;
    CHECK(romfs_get_entry("renamed", &entry) == ROMFS_ERR_NO_ENTRY);
    CHECK(romfs_dir_open(&root, "dir", &dir) == ROMFS_ERR_NO_ENTRY);
}

static uint16_t le16(uint16_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return (value >> 8) | (value << 8);
#endif
}

static uint32_t le32(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap32(value);
#endif
}

/* Build a valid pre-existing image directly, then mount it. No filesystem
 * callback is bypassed while testing an API operation. */
static void fixture(uint32_t free_sector, uint32_t free_slot)
{
    romfs_entry *entries = (romfs_entry *) list;
    for (uint32_t i = 3; i < free_slot; i++) {
        romfs_entry entry = {0};
        snprintf(entry.name, sizeof(entry.name), "fixture-%u", i);
        entry.attr.names.type = ROMFS_TYPE_MISC;
        entry.attr.raw = le16(entry.attr.raw);
        entry.start = le32(0xffff);
        entries[i] = entry;
    }
    uint32_t first = data_start / SECTOR;
    if (free_sector > first) {
        entries[3].start = le32(first);
        entries[3].size = le32((free_sector - first) * SECTOR);
        for (uint32_t i = first; i < free_sector; i++) {
            map[i] = le16(i + 1 < free_sector ? i + 1 : i);
        }
    }
    memcpy(test_flash_data() + START, list, list_size);
    memcpy(test_flash_data() + START + list_size, map, map_size);
    remount();
    reset();
}

static void collect_measurement(uint32_t mb)
{
    case_name = "GC of a deleted chain and reuse of its catalog slot";
    setup(mb);
    fixture(data_start / SECTOR + 2, list_size / sizeof(romfs_entry));
    CHECK(romfs_delete("fixture-3") == ROMFS_NOERR);
    reset();
    romfs_file file = {0};
    create(&file, "replacement", 0);
    CHECK(file.nentry == 3);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    report("GC-empty-replacement", 1, 1);
    remount();
    CHECK(map[data_start / SECTOR] == 0xffff && map[data_start / SECTOR + 1] == 0xffff);
    romfs_entry entry;
    CHECK(romfs_get_entry("replacement", &entry) == ROMFS_NOERR && entry.size == 0);
}

#ifndef ROMFS_METADATA_BASELINE
static void boundaries(void)
{
    case_name = "catalog sector boundaries";
    const uint32_t slots[] = {63, 64, 127, 128, 191, 192, 255};
    for (unsigned i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        setup(256);
        fixture(data_start / SECTOR, slots[i]);
        romfs_file file = {0};
        create(&file, "boundary", 0);
        CHECK(file.nentry == slots[i]);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        check_writes(1u << (slots[i] / 64), 0);
        reset();
        CHECK(romfs_rename("boundary", "renamed") == ROMFS_NOERR);
        check_writes(1u << (slots[i] / 64), 0);
        reset();
        CHECK(romfs_delete("renamed") == ROMFS_NOERR);
        check_writes(1u << (slots[i] / 64), 0);
        remount();
        CHECK(romfs_open_file("renamed", &file, io) == ROMFS_ERR_NO_ENTRY);
    }
    case_name = "directory entries at catalog boundaries";
    for (unsigned i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        setup(256);
        fixture(data_start / SECTOR, slots[i]);
        romfs_dir root, dir;
        CHECK(romfs_dir_root(&root) == ROMFS_NOERR);
        CHECK(romfs_dir_create(&root, "directory", &dir) == ROMFS_NOERR);
        CHECK(dir.entry_index == slots[i]);
        check_writes(1u << (slots[i] / 64), 0);
        reset();
        CHECK(romfs_dir_remove(&dir) == ROMFS_NOERR);
        check_writes(1u << (slots[i] / 64), 0);
        remount();
        CHECK(romfs_dir_open(&root, "directory", &dir) == ROMFS_ERR_NO_ENTRY);
    }
    case_name = "map boundary including bit 31, allocation and shrink";
    const uint32_t starts[] = {2047, 63487};
    for (unsigned i = 0; i < 2; i++) {
        setup(256);
        fixture(starts[i], 255);
        romfs_file file = {0};
        create(&file, "boundary", sizeof(payload));
        CHECK(file.entry.start == starts[i]);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        uint32_t bits = (1u << (starts[i] / 2048)) | (1u << ((starts[i] + 1) / 2048));
        check_writes(8, bits);
        remount();
        CHECK(romfs_open_write_path("boundary", &file, io) == ROMFS_NOERR);
        uint8_t actual[sizeof(payload)];
        romfs_file reader = {0};
        CHECK(romfs_open_read_view(&file, &reader, io) == ROMFS_NOERR);
        CHECK(romfs_read_file(actual, sizeof(actual), &reader) == sizeof(actual));
        CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
        CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
        reset();
        CHECK(romfs_truncate_file(&file, SECTOR) == ROMFS_NOERR);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
        check_writes(8, bits);
        remount();
        CHECK(map[starts[i]] == le16(starts[i]) && map[starts[i] + 1] == 0xffff);
    }
    puts("PASS metadata addresses at catalog/map boundaries and map bit 31");
}

static void sparse_failures(void)
{
    case_name = "sparse metadata failure, failed close and re-dirty after partial success";
    const unsigned sectors[] = {3, 34, 35}; /* Catalog 3, map 30 and map 31. */
    for (unsigned rename = 0; rename < 2; rename++) {
        for (unsigned op = TEST_FLASH_ERASE; op <= TEST_FLASH_WRITE; op++) {
            for (unsigned nth = 1; nth <= 3; nth++) {
                setup(256);
                fixture(63487, 255);
                romfs_file file = {0};
                create(&file, "boundary", sizeof(payload));
                reset(); /* Both full data buffers are already on flash. */
                CHECK(test_flash_fail_on(op, nth));
                CHECK(romfs_close_file(&file) == ROMFS_ERR_IO);
                CHECK(test_flash_get_stats()->first_failure_offset == START + sectors[nth - 1] * SECTOR);
                CHECK(test_flash_get_stats()->injected == 1);
                for (unsigned i = 0; i < MAX_META; i++) {
                    unsigned erase = 0, write = 0;
                    for (unsigned j = 0; j < nth; j++) {
                        erase += sectors[j] == i;
                        write += sectors[j] == i && (j + 1 < nth || op == TEST_FLASH_WRITE);
                    }
                    CHECK(addresses[i][TEST_FLASH_ERASE] == erase);
                    CHECK(addresses[i][TEST_FLASH_WRITE] == write);
                }
                reset();
                if (rename) {
                    CHECK(romfs_rename("boundary", "renamed") == ROMFS_NOERR);
                } else {
                    CHECK(romfs_sync() == ROMFS_NOERR);
                }
                check_writes(rename || nth == 1 ? 8 : 0,
                             nth <= 2 ? UINT32_C(0xc0000000) : UINT32_C(0x80000000));
                remount();
                CHECK(romfs_open_file(rename ? "renamed" : "boundary", &file, io) == ROMFS_NOERR);
                uint8_t actual[sizeof(payload)];
                CHECK(romfs_read_file(actual, sizeof(actual), &file) == sizeof(actual));
                CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
                CHECK(romfs_close_file(&file) == ROMFS_NOERR);
            }
        }
    }
    puts("PASS sparse retry preserves dirty sectors; later changes re-dirty an already saved catalog sector");
}

static void full_sync_failures(void)
{
    case_name = "full sync and format: every metadata sector failure and retry";
    for (unsigned format = 0; format < 2; format++) {
        for (unsigned op = TEST_FLASH_ERASE; op <= TEST_FLASH_WRITE; op++) {
            for (unsigned nth = 1; nth <= MAX_META; nth++) {
                setup(256);
                if (!format) {
                    /* Alter unused name bytes in the last catalog sector and
                     * the last legal map link through the caller's buffers. */
                    list[list_size - sizeof(romfs_entry) + 1] = 0x5c;
                    map[65534] = le16(65534);
                }
                CHECK(test_flash_fail_on(op, nth));
                if (format) {
                    CHECK(!romfs_format());
                } else {
                    CHECK(romfs_sync_full() == ROMFS_ERR_IO);
                }
                CHECK(test_flash_get_stats()->injected == 1);
                CHECK(test_flash_get_stats()->first_failure_offset == START + (nth - 1) * SECTOR);
                for (unsigned i = 0; i < MAX_META; i++) {
                    CHECK(addresses[i][TEST_FLASH_ERASE] == (i < nth));
                    CHECK(addresses[i][TEST_FLASH_WRITE] == (i < nth - (op == TEST_FLASH_ERASE)));
                }
                reset();
                CHECK(romfs_sync() == ROMFS_NOERR);
                for (unsigned i = 0; i < MAX_META; i++) {
                    CHECK(addresses[i][TEST_FLASH_ERASE] == (i >= nth - 1));
                    CHECK(addresses[i][TEST_FLASH_WRITE] == (i >= nth - 1));
                }
                reset();
                CHECK(romfs_sync() == ROMFS_NOERR);
                check_writes(0, 0);
                remount();
                if (!format) {
                    CHECK(list[list_size - sizeof(romfs_entry) + 1] == 0x5c);
                    CHECK(map[65534] == le16(65534));
                } else {
                    CHECK(map[65534] == 0xffff);
                }
                CHECK(map[65535] == 0xffff);
                CHECK(test_flash_get_stats()->rejected == 0);
            }
        }
    }
    puts("PASS erase/program failure at each of 36 metadata sectors; retry skips successful sectors");
}

static void external_edits(void)
{
    case_name = "explicit persistence of caller-owned buffers and service repair";
    setup(256);
    list[list_size - sizeof(romfs_entry) + 1] = 0x39;
    map[65534] = le16(65534);
    CHECK(romfs_sync() == ROMFS_NOERR);
    check_writes(0, 0);
    CHECK(test_flash_data()[START + list_size - sizeof(romfs_entry) + 1] == 0xff);
    CHECK(romfs_sync_full() == ROMFS_NOERR);
    check_writes(15, UINT32_MAX);
    remount();
    CHECK(list[list_size - sizeof(romfs_entry) + 1] == 0x39 && map[65534] == le16(65534));
    reset();
    map[0] = 0xffff;
    CHECK(romfs_sync() == ROMFS_NOERR);
    check_writes(0, 1);
    remount();
    CHECK(map[0] == le16(1));
    /* A correction made while mounting must also remain pending until sync. */
    memset(test_flash_data() + START + list_size, 0xff, sizeof(uint16_t));
    remount();
    reset();
    CHECK(romfs_sync() == ROMFS_NOERR);
    check_writes(0, 1);
    reset();
    CHECK(test_flash_fail_on(TEST_FLASH_ERASE, 1));
    CHECK(romfs_sync_full() == ROMFS_ERR_IO);
    remount(); /* A fresh mount must discard the old mount's dirty masks. */
    reset();
    CHECK(romfs_sync() == ROMFS_NOERR);
    check_writes(0, 0);
    puts("PASS explicit full sync persists external buffers; service repairs mark only their map sector");
}
#endif

int main(int argc, char **argv)
{
    CHECK(argc == 1 || (argc == 2 && strcmp(argv[1], "--measure") == 0));
    measure_only = argc == 2;
    for (unsigned i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t) (i * 29 + i / 251);
    }
    const uint32_t sizes[] = {2, 4, 8, 16, 32, 64, 128, 256};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        measurements(sizes[i]);
        collect_measurement(sizes[i]);
    }
#ifndef ROMFS_METADATA_BASELINE
    if (!measure_only) {
        boundaries();
        sparse_failures();
        full_sync_failures();
        external_edits();
    }
#endif
    test_flash_destroy();
    puts("PASS metadata tests");
    return 0;
}
