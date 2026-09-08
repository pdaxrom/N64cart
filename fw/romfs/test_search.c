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

enum { SECTOR = ROMFS_FLASH_SECTOR, IMAGE_SIZE = 256 * ROMFS_MB, START = 65536 };
static const char *case_name = "setup";
static uint16_t map[32 * SECTOR / sizeof(uint16_t)];
static uint8_t list[4 * SECTOR], io[SECTOR], second_io[SECTOR], payload[SECTOR];
static uint32_t first;
static bool measure_only;
extern uint64_t romfs_test_allocation_checks, romfs_test_catalog_copies;

static uint16_t le16(uint16_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap16(value);
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

static void reset(void)
{
    romfs_test_allocation_checks = romfs_test_catalog_copies = 0;
    test_flash_reset_counters();
}

static void remount(void)
{
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    reset();
}

static void setup(const char *name)
{
    case_name = name;
    first = (START + sizeof(map) + sizeof(list)) / SECTOR;
    CHECK(test_flash_init(IMAGE_SIZE));
    remount();
    CHECK(romfs_format());
    reset();
}

static void raw_entry(unsigned slot, const char *name, unsigned parent, unsigned start, unsigned count)
{
    romfs_entry entry = {0};
    snprintf(entry.name, sizeof(entry.name), "%s", name);
    entry.attr.names.type = ROMFS_TYPE_MISC;
    entry.attr.names.parent = parent;
    entry.attr.raw = le16(entry.attr.raw);
    entry.start = le32(count ? start : 0xffff);
    entry.size = le32(count * SECTOR);
    ((romfs_entry *) list)[slot] = entry;
}

static void fixture_chain(unsigned slot, const char *name, unsigned start, unsigned count, unsigned stride)
{
    raw_entry(slot, name, 0, start, count);
    for (unsigned i = 0; i < count; i++) {
        unsigned sector = start + i * stride;
        CHECK(sector < 65535 && map[sector] == 0xffff);
        map[sector] = le16(i + 1 < count ? sector + stride : sector);
    }
}

static void persist_fixture(void)
{
    memcpy(test_flash_data() + START, list, sizeof(list));
    memcpy(test_flash_data() + START + sizeof(list), map, sizeof(map));
    remount();
}

static void create(romfs_file *file, const char *name, uint8_t *buffer)
{
    CHECK(romfs_create_file(name, file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, buffer) == ROMFS_NOERR);
}

static void write_bytes(romfs_file *file, unsigned size)
{
    CHECK(romfs_write_file(payload, size, file) == size && file->err == ROMFS_NOERR);
}

static void readback(const char *name, unsigned size)
{
    romfs_file reader = {0};
    CHECK(romfs_open_file(name, &reader, io) == ROMFS_NOERR && reader.entry.size == size);
    uint8_t actual[SECTOR];
    unsigned left = size;
    while (left) {
        unsigned chunk = left < SECTOR ? left : SECTOR;
        CHECK(romfs_read_file(actual, chunk, &reader) == chunk);
        CHECK(memcmp(actual, payload, chunk) == 0);
        left -= chunk;
    }
    CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    CHECK(test_flash_get_stats()->rejected == 0);
}

static void lookup_measurements(bool nested)
{
    setup(nested ? "nested name lookup" : "root name lookup");
    romfs_dir root, dir;
    CHECK(romfs_dir_root(&root) == ROMFS_NOERR);
    CHECK(romfs_dir_create(&root, "dir", &dir) == ROMFS_NOERR);
    for (unsigned i = 4; i < 256; i++) {
        char name[24];
        snprintf(name, sizeof(name), "entry-%u", i);
        raw_entry(i, name, nested && i % 2 == 0 ? dir.id : 0, 0, 0);
    }
    persist_fixture();
    const char *names[] = {"entry-4", nested ? "entry-254" : "entry-255", "absent"};
    for (unsigned i = 0; i < 3; i++) {
        char path[64];
        snprintf(path, sizeof(path), "%s%s", nested ? "/dir/" : "/", names[i]);
        romfs_entry entry;
        reset();
        CHECK(romfs_get_entry_path(path, &entry) == (i == 2 ? ROMFS_ERR_NO_ENTRY : ROMFS_NOERR));
        if (i != 2) {
            CHECK(strcmp(entry.name, names[i]) == 0 && entry.size == 0 && entry.start == 0xffff);
        }
        printf("lookup %s copies=%" PRIu64 "\n", path, romfs_test_catalog_copies);
        if (!measure_only) {
            CHECK(romfs_test_catalog_copies == (nested ? 1u : 0u) + (i == 2 ? 0u : 1u));
        }
        CHECK(test_flash_get_stats()->calls[TEST_FLASH_READ] == 0);
        CHECK(test_flash_get_stats()->calls[TEST_FLASH_ERASE] == 0);
        CHECK(test_flash_get_stats()->calls[TEST_FLASH_WRITE] == 0);
    }
}

static void many_files(unsigned count, unsigned prefix)
{
    setup("first allocation in many small files");
    fixture_chain(3, "blocker", first, prefix, 1);
    persist_fixture();
    for (unsigned i = 0; i < count; i++) {
        char name[24];
        snprintf(name, sizeof(name), "small-%u", i);
        romfs_file file = {0};
        create(&file, name, io);
        write_bytes(&file, 17);
        CHECK(file.entry.start == first + prefix + i);
        CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    }
    printf("allocate files=%u prefix=%u checks=%" PRIu64 "\n", count, prefix, romfs_test_allocation_checks);
    if (!measure_only) {
        CHECK(romfs_test_allocation_checks == count + prefix);
    }
    remount();
    for (unsigned i = 0; i < count; i++) {
        char name[24];
        snprintf(name, sizeof(name), "small-%u", i);
        readback(name, 17);
    }
}

static void sequential(bool fragmented)
{
    setup(fragmented ? "fragmented allocation" : "contiguous allocation");
    enum { COUNT = 240 };
    if (fragmented) {
        fixture_chain(3, "blocker", first + 1, COUNT, 2);
        persist_fixture();
    }
    romfs_file file = {0};
    create(&file, "data", io);
    for (unsigned i = 0; i < COUNT; i++) {
        write_bytes(&file, SECTOR);
    }
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    printf("allocate sequential fragmented=%u checks=%" PRIu64 "\n", fragmented, romfs_test_allocation_checks);
    CHECK(romfs_test_allocation_checks == 1 + (COUNT - 1) * (fragmented ? 3u : 2u));
    remount();
    readback("data", COUNT * SECTOR);
}

static void lookup_edges(void)
{
    setup("bounded names, direct external edits, deleted entries and public listing");
    char name[ROMFS_MAX_NAME_LEN];
    memset(name, 'x', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    raw_entry(3, name, 0, 0, 0);
    ((romfs_entry *) list)[3].name[ROMFS_MAX_NAME_LEN - 1] = 'z';
    raw_entry(4, "gone", 0, 0, 0);
    ((romfs_entry *) list)[4].name[0] = ROMFS_DELETED_ENTRY;
    raw_entry(5, "visible", 0, 0, 0);
    persist_fixture();
    romfs_entry entry;
    CHECK(romfs_get_entry(name, &entry) == ROMFS_NOERR && strcmp(name, entry.name) == 0);
    CHECK(romfs_get_entry("gone", &entry) == ROMFS_ERR_NO_ENTRY);
    name[0] = 'y';
    ((romfs_entry *) list)[3].name[0] = 'y';
    CHECK(romfs_get_entry(name, &entry) == ROMFS_NOERR);
    name[0] = 'x';
    CHECK(romfs_get_entry(name, &entry) == ROMFS_ERR_NO_ENTRY);
    romfs_file iter = {0};
    unsigned seen = 0;
    for (uint32_t err = romfs_list(&iter, true); err == ROMFS_NOERR; err = romfs_list(&iter, false)) {
        CHECK(iter.nentry == (seen < 4 ? seen + 1 : 6));
        seen++;
    }
    CHECK(seen == 5);
    puts("PASS bounded lookup preserves listing and follows live catalog edits");
}

static void reuse_and_wrap(void)
{
    setup("two writers, truncate and failed-close allocation reuse");
    romfs_file a = {0}, b = {0};
    create(&a, "a", io);
    create(&b, "b", second_io);
    write_bytes(&a, 17);
    write_bytes(&b, 17);
    CHECK(a.entry.start != b.entry.start);
    unsigned freed = a.entry.start;
    CHECK(romfs_truncate_file(&a, 0) == ROMFS_NOERR);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR && romfs_close_file(&b) == ROMFS_NOERR);
    create(&a, "reuse", io);
    write_bytes(&a, 17);
    CHECK(a.entry.start == freed);
    CHECK(test_flash_fail_on(TEST_FLASH_WRITE, 1));
    CHECK(romfs_close_file(&a) == ROMFS_ERR_IO);
    create(&a, "retry", io);
    write_bytes(&a, 17);
    CHECK(a.entry.start == freed);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    remount();
    readback("retry", 17);
    readback("b", 17);

    setup("wrap to a newly freed earlier sector and exclude 65535");
    fixture_chain(3, "blocker", first, 65534 - first, 1);
    persist_fixture();
    create(&a, "last", io);
    write_bytes(&a, 17);
    CHECK(a.entry.start == 65534);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    CHECK(map[65535] == 0xffff && romfs_free() == 0);
    /* Directly free the first sector of the blocker and shorten that valid
     * chain. The allocator must inspect RAM, regardless of its saved hint. */
    map[first] = 0xffff;
    ((romfs_entry *) list)[3].start = le32(first + 1);
    ((romfs_entry *) list)[3].size = le32((65533 - first) * SECTOR);
    create(&a, "wrapped", io);
    write_bytes(&a, 17);
    CHECK(a.entry.start == first);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    CHECK(romfs_sync_full() == ROMFS_NOERR);
    create(&a, "full", io);
    CHECK(romfs_write_file(payload, 1, &a) == 0 && a.err == ROMFS_ERR_NO_SPACE);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    CHECK(romfs_delete("last") == ROMFS_NOERR);
    create(&a, "collected", io);
    write_bytes(&a, 17);
    CHECK(a.entry.start == 65534);
    CHECK(romfs_close_file(&a) == ROMFS_NOERR);
    remount();
    readback("wrapped", 17);
    readback("collected", 17);
    CHECK(romfs_free() == 0 && map[65535] == 0xffff);
    puts("PASS allocator ownership, truncate/failed-close/GC reuse, wraparound and ENOSPC");
}

static void stale_hint(void)
{
    setup("stale hint must wrap past occupied RAM entries");
    unsigned gap = first + 3;
    fixture_chain(3, "prefix", first, gap - first, 1);
    fixture_chain(4, "suffix", gap + 1, 65535 - gap - 1, 1);
    persist_fixture();
    romfs_file file = {0};
    create(&file, "gap", io);
    write_bytes(&file, 17);
    CHECK(file.entry.start == gap);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    map[first] = 0xffff;
    ((romfs_entry *) list)[3].start = le32(first + 1);
    ((romfs_entry *) list)[3].size = le32((gap - first - 1) * SECTOR);
    reset();
    create(&file, "wrapped", io);
    write_bytes(&file, 17);
    CHECK(file.entry.start == first);
    if (!measure_only) {
        CHECK(romfs_test_allocation_checks == 65535 - gap);
    }
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    CHECK(romfs_sync_full() == ROMFS_NOERR);
    remount();
    readback("wrapped", 17);
    CHECK(romfs_format());
    create(&file, "after-format", io);
    write_bytes(&file, 17);
    CHECK(file.entry.start == first);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    puts("PASS stale hint scans both ranges; format resets its starting point");
}

int main(int argc, char **argv)
{
    CHECK(argc == 1 || (argc == 2 && strcmp(argv[1], "--measure") == 0));
    measure_only = argc == 2;
    for (unsigned i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t) (i * 37 + 17);
    }
    lookup_measurements(false);
    lookup_measurements(true);
    const unsigned counts[] = {16, 64, 240};
    const unsigned prefixes[] = {0, 2048, 60000};
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned j = 0; j < 3; j++) {
            many_files(counts[i], prefixes[j]);
        }
    }
    sequential(false);
    sequential(true);
    lookup_edges();
    reuse_and_wrap();
    stale_hint();
    test_flash_destroy();
    puts("PASS search tests");
    return 0;
}
