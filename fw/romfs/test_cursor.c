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

enum { SECTOR = ROMFS_FLASH_SECTOR, IMAGE_SIZE = 64 * ROMFS_MB, START = 65536, MAX_SECTORS = 4096 };
static const char *case_name = "setup";
static uint16_t map[8 * SECTOR / sizeof(uint16_t)];
static uint8_t list[SECTOR], io[SECTOR];
static uint8_t payload[(MAX_SECTORS + 1) * SECTOR], actual[sizeof(payload)];
static uint32_t first_data;
extern uint64_t romfs_test_validation_links, romfs_test_position_links;

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

static void setup(const char *name)
{
    case_name = name;
    CHECK(test_flash_init(IMAGE_SIZE));
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    CHECK(romfs_format());
    first_data = (START + sizeof(list) + sizeof(map)) / SECTOR;
}

static void reset_counters(void)
{
    romfs_test_validation_links = romfs_test_position_links = 0;
    test_flash_reset_counters();
}

/* Build valid, persisted chains directly so fixture creation does not dominate
 * the measured operations. The blocker owns alternating physical sectors. */
static void fixture_file(const char *name, uint32_t first, uint32_t size, uint32_t stride, bool blocker)
{
    romfs_file file = {0};
    CHECK(romfs_create_file(name, &file, 0, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    uint32_t count = (size + SECTOR - 1) / SECTOR;
    for (unsigned i = 0; i < count; i++) {
        uint32_t sector = first + i * stride;
        CHECK(map[sector] == 0xffff);
        map[sector] = le16(i + 1 == count ? sector : sector + stride);
        uint8_t *data = test_flash_data() + sector * SECTOR;
        memset(data, 0, SECTOR);
        uint32_t bytes = size - i * SECTOR < SECTOR ? size - i * SECTOR : SECTOR;
        if (blocker) {
            memset(data, 0x6c, bytes);
        } else {
            memcpy(data, payload + i * SECTOR, bytes);
        }
    }
    romfs_entry *entry = &((romfs_entry *) list)[file.nentry];
    entry->start = le32(count ? first : 0xffff);
    entry->size = le32(size);
    memcpy(test_flash_data() + START, list, sizeof(list));
    memcpy(test_flash_data() + START + sizeof(list), map, sizeof(map));
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
}

static void readback(const char *name, uint32_t size)
{
    romfs_file reader = {0};
    CHECK(romfs_open_file(name, &reader, io) == ROMFS_NOERR && reader.entry.size == size);
    CHECK(romfs_read_file(actual, sizeof(actual), &reader) == size);
    CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    CHECK(test_flash_get_stats()->rejected == 0);
}

static void check_blocker(uint32_t count)
{
    readback("blocker", count * SECTOR);
    for (unsigned i = 0; i < count * SECTOR; i++) {
        CHECK(actual[i] == 0x6c);
    }
}

static void sequential(bool existing, bool fragmented, uint32_t count, uint32_t chunk, bool measure_only)
{
    char label[128];
    snprintf(label, sizeof(label), "%s-%s sectors=%u chunk=%u", existing ? "overwrite" : "create",
             fragmented ? "fragmented" : "contiguous", count, chunk);
    setup(label);
    uint32_t size = count * SECTOR;
    if (fragmented) {
        fixture_file("blocker", first_data + 1, size, 2, true);
    }
    romfs_file file = {0};
    if (existing) {
        fixture_file("data", first_data, size, fragmented ? 2 : 1, false);
        /* Make every old sector differ from the replacement, so a skipped or
         * misdirected overwrite cannot pass by reading unchanged fixture data. */
        for (unsigned i = 0; i < count; i++) {
            uint8_t *data = test_flash_data() + (first_data + i * (fragmented ? 2 : 1)) * SECTOR;
            for (unsigned j = 0; j < SECTOR; j++) {
                data[j] ^= 0xff;
            }
        }
        CHECK(romfs_open_write_path("data", &file, io) == ROMFS_NOERR);
    } else {
        CHECK(romfs_create_file("data", &file, 0, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    }
    reset_counters();
    uint64_t expected_validation = 0;
    for (uint32_t offset = 0; offset < size;) {
        uint32_t length = chunk < size - offset ? chunk : size - offset;
        expected_validation += existing ? count : (offset + SECTOR - 1) / SECTOR;
        CHECK(romfs_write_file(payload + offset, length, &file) == length && file.err == ROMFS_NOERR);
        offset += length;
    }
    uint64_t validation = romfs_test_validation_links, position = romfs_test_position_links;
    printf("%s validation=%" PRIu64 " position=%" PRIu64 "\n", label, validation, position);
    CHECK(validation == expected_validation);
    if (!measure_only) {
        CHECK(position <= count - 1);
    }
    CHECK(test_flash_get_stats()->calls[TEST_FLASH_ERASE] == count);
    CHECK(test_flash_get_stats()->calls[TEST_FLASH_WRITE] == count);
    CHECK(file.entry.size == size && file.write_offset == size);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    readback("data", size);
    CHECK(memcmp(actual, payload, size) == 0);
    if (fragmented) {
        check_blocker(count);
    }
}

static void extension(bool truncate, bool fragmented, uint32_t count, bool measure_only)
{
    char label[128];
    snprintf(label, sizeof(label), "%s-%s sectors=%u", truncate ? "truncate-extension" : "gap",
             fragmented ? "fragmented" : "contiguous", count);
    setup(label);
    if (fragmented) {
        fixture_file("blocker", first_data + 1, (count + 1) * SECTOR, 2, true);
    }
    fixture_file("data", first_data, 17, 1, false);
    romfs_file file = {0};
    CHECK(romfs_open_write_path("data", &file, io) == ROMFS_NOERR);
    uint32_t size = count * SECTOR + 17;
    CHECK(romfs_seek_file(&file, truncate ? 7 : size - 1, SEEK_SET) == ROMFS_NOERR);
    reset_counters();
    if (truncate) {
        CHECK(romfs_truncate_file(&file, size) == ROMFS_NOERR && file.write_offset == 7);
    } else {
        CHECK(romfs_write_file(payload, 1, &file) == 1 && file.err == ROMFS_NOERR);
    }
    printf("%s validation=%" PRIu64 " position=%" PRIu64 "\n", label,
           romfs_test_validation_links, romfs_test_position_links);
    if (!measure_only) {
        CHECK(romfs_test_position_links <= count);
    }
    CHECK(file.entry.size == size);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    readback("data", size);
    CHECK(memcmp(actual, payload, 17) == 0);
    for (unsigned i = 17; i < size; i++) {
        CHECK(actual[i] == ((!truncate && i == size - 1) ? payload[0] : 0));
    }
    if (fragmented) {
        check_blocker(count + 1);
    }
}

static void seeks_and_retries(void)
{
    setup("dirty buffer and cursor at different positions in a fragmented chain");
    fixture_file("blocker", first_data + 1, 12 * SECTOR, 2, true);
    fixture_file("data", first_data, 8 * SECTOR, 2, false);
    uint8_t expected[10 * SECTOR], patch[2 * SECTOR + 77];
    memcpy(expected, payload, 8 * SECTOR);
    uint32_t size = 8 * SECTOR;
    romfs_file file = {0};
    CHECK(romfs_open_write_path("data", &file, io) == ROMFS_NOERR);
    const uint32_t offsets[] = {7 * SECTOR + 100, SECTOR + 200, 5 * SECTOR - 10, 0, 9 * SECTOR + 31};
    const uint32_t lengths[] = {17, sizeof(patch), 33, 1, 17};
    for (unsigned n = 0; n < sizeof(offsets) / sizeof(offsets[0]); n++) {
        for (unsigned i = 0; i < sizeof(patch); i++) {
            patch[i] = (uint8_t) (0xe5 - i * 11 - n * 17);
        }
        CHECK(romfs_seek_file(&file, offsets[n], SEEK_SET) == ROMFS_NOERR);
        CHECK(romfs_write_file(patch, lengths[n], &file) == lengths[n] && file.err == ROMFS_NOERR);
        if (offsets[n] > size) {
            memset(expected + size, 0, offsets[n] - size);
        }
        memcpy(expected + offsets[n], patch, lengths[n]);
        if (offsets[n] + lengths[n] > size) {
            size = offsets[n] + lengths[n];
        }
        CHECK(file.write_offset == offsets[n] + lengths[n] && file.entry.size == size);
    }
    CHECK(romfs_truncate_file(&file, 3 * SECTOR + 19) == ROMFS_NOERR);
    size = 3 * SECTOR + 19;
    CHECK(romfs_seek_file(&file, 7 * SECTOR + 5, SEEK_SET) == ROMFS_NOERR);
    CHECK(romfs_write_file(patch, 17, &file) == 17 && file.err == ROMFS_NOERR);
    memset(expected + size, 0, 7 * SECTOR + 5 - size);
    memcpy(expected + 7 * SECTOR + 5, patch, 17);
    size = 7 * SECTOR + 22;
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    readback("data", size);
    CHECK(memcmp(actual, expected, size) == 0);
    check_blocker(12);

    for (unsigned operation = TEST_FLASH_READ; operation <= TEST_FLASH_WRITE; operation++) {
        setup("fragmented cursor reinitialization after read/erase/write failure");
        fixture_file("blocker", first_data + 1, 8 * SECTOR, 2, true);
        fixture_file("data", first_data, 8 * SECTOR, 2, false);
        memcpy(expected, payload, 8 * SECTOR);
        CHECK(romfs_open_write_path("data", &file, io) == ROMFS_NOERR);
        CHECK(romfs_seek_file(&file, SECTOR + 7, SEEK_SET) == ROMFS_NOERR);
        CHECK(test_flash_fail_on(operation, 2));
        uint32_t accepted = operation == TEST_FLASH_READ ? SECTOR - 7 : 2 * SECTOR - 7;
        CHECK(romfs_write_file(patch, sizeof(patch), &file) == accepted && file.err == ROMFS_ERR_IO);
        CHECK(file.write_offset == SECTOR + 7 + accepted);
        CHECK(test_flash_get_stats()->first_failure_offset == (first_data + 4) * SECTOR);
        CHECK(romfs_write_file(patch + accepted, sizeof(patch) - accepted, &file) == sizeof(patch) - accepted);
        CHECK(file.err == ROMFS_NOERR && romfs_close_file(&file) == ROMFS_NOERR);
        CHECK(romfs_start(START, IMAGE_SIZE, map, list));
        memcpy(expected + SECTOR + 7, patch, sizeof(patch));
        readback("data", 8 * SECTOR);
        CHECK(memcmp(actual, expected, 8 * SECTOR) == 0);
        check_blocker(8);
    }
    puts("PASS fragmented seeks, dirty buffers, truncate/reallocation and cursor restart after I/O failures");
}

static void externally_changed_chain(void)
{
    setup("valid external relink and invalid tail between write calls");
    fixture_file("data", first_data, 4 * SECTOR, 1, false);
    romfs_file file = {0};
    CHECK(romfs_open_write_path("data", &file, io) == ROMFS_NOERR);
    CHECK(romfs_write_file(payload, 2 * SECTOR, &file) == 2 * SECTOR && file.err == ROMFS_NOERR);
    /* Reorder the last two sectors while the I/O buffer is empty. The next
     * write starts at logical sector 2 and must follow the newly valid chain. */
    map[first_data + 1] = le16(first_data + 3);
    map[first_data + 3] = le16(first_data + 2);
    map[first_data + 2] = le16(first_data + 2);
    uint8_t patch = 0x71;
    reset_counters();
    CHECK(romfs_write_file(&patch, 1, &file) == 1 && file.err == ROMFS_NOERR);
    CHECK(romfs_test_validation_links == 4 && file.pos == first_data + 3 && file.buffer_dirty);
    /* Damage a different, later link: a dirty-buffer hit must still validate
     * the entire chain and refuse before any flash callback or file mutation. */
    map[first_data + 2] = 0xffff;
    reset_counters();
    CHECK(romfs_write_file(&patch, 1, &file) == 0 && file.err == ROMFS_ERR_OPERATION);
    CHECK(file.write_offset == 2 * SECTOR + 1 && file.buffer_dirty);
    for (unsigned op = 0; op < TEST_FLASH_OPERATION_COUNT; op++) {
        CHECK(test_flash_get_stats()->calls[op] == 0);
    }
    map[first_data + 2] = le16(first_data + 2);
    CHECK(romfs_write_file(&patch, 1, &file) == 1 && file.err == ROMFS_NOERR);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    readback("data", 4 * SECTOR);
    CHECK(memcmp(actual, payload, 2 * SECTOR) == 0);
    CHECK(actual[2 * SECTOR] == patch && actual[2 * SECTOR + 1] == patch);
    CHECK(memcmp(actual + 2 * SECTOR + 2, payload + 3 * SECTOR + 2, SECTOR - 2) == 0);
    CHECK(memcmp(actual + 3 * SECTOR, payload + 2 * SECTOR, SECTOR) == 0);
    puts("PASS each public write follows valid external relinks and rejects later corruption before I/O");
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
    const uint32_t counts[] = {64, 256, 1024, MAX_SECTORS};
    for (unsigned layout = 0; layout < 2; layout++) {
        for (unsigned existing = 0; existing < 2; existing++) {
            for (unsigned n = 0; n < sizeof(counts) / sizeof(counts[0]); n++) {
                sequential(existing, layout, counts[n], counts[n] * SECTOR, measure_only);
                sequential(existing, layout, counts[n], SECTOR, measure_only);
            }
            sequential(existing, layout, 64, 17, measure_only);
            sequential(existing, layout, 64, 16 * SECTOR, measure_only);
        }
        extension(false, layout, MAX_SECTORS, measure_only);
        extension(true, layout, MAX_SECTORS, measure_only);
    }
    seeks_and_retries();
    externally_changed_chain();
    test_flash_destroy();
    return 0;
}
