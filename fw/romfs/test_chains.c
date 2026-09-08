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

static const char *case_name = "setup";
enum { IMAGE_SIZE = 2 * ROMFS_MB, START = 65536, FIRST = 18, LIMIT = 512 };
static uint16_t map[ROMFS_FLASH_SECTOR / sizeof(uint16_t)];
static uint8_t list[ROMFS_FLASH_SECTOR];
static uint16_t saved_map[ROMFS_FLASH_SECTOR / sizeof(uint16_t)];
static uint8_t saved_list[ROMFS_FLASH_SECTOR];
static uint16_t healthy_map[ROMFS_FLASH_SECTOR / sizeof(uint16_t)];
static uint8_t healthy_list[ROMFS_FLASH_SECTOR];
static uint8_t *saved_flash;
static uint8_t io[ROMFS_FLASH_SECTOR], read_io[ROMFS_FLASH_SECTOR];
static uint8_t payload[3 * ROMFS_FLASH_SECTOR], actual[sizeof(payload)];
static romfs_file reclaim, victim, neighbor, reader, writer;

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

static romfs_entry native_entry(unsigned slot)
{
    romfs_entry entry = ((romfs_entry *) list)[slot];
    entry.attr.raw = le16(entry.attr.raw);
    entry.start = le32(entry.start);
    entry.size = le32(entry.size);
    return entry;
}

static void persist_fixture(void)
{
    memcpy(test_flash_data() + START, list, sizeof(list));
    memcpy(test_flash_data() + START + sizeof(list), map, sizeof(map));
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
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

static void create_file(const char *name, romfs_file *file, uint32_t size)
{
    CHECK(romfs_create_file(name, file, 0, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    CHECK(romfs_write_file(payload, size, file) == size);
    CHECK(romfs_close_file(file) == ROMFS_NOERR);
}

static void setup(void)
{
    CHECK(test_flash_init(IMAGE_SIZE));
    memset(test_flash_data(), 0x5a, START);
    CHECK(romfs_start(START, IMAGE_SIZE, map, list));
    CHECK(romfs_format());
    create_file("reclaim", &reclaim, ROMFS_FLASH_SECTOR);
    create_file("victim", &victim, sizeof(payload));
    create_file("neighbor", &neighbor, ROMFS_FLASH_SECTOR);
    CHECK(victim.entry.start == FIRST + 1 && neighbor.entry.start == FIRST + 4);
    memcpy(healthy_map, map, sizeof(map));
    memcpy(healthy_list, list, sizeof(list));
}

static void open_before_live_corruption(bool writable)
{
    uint16_t corrupt_map[ROMFS_FLASH_SECTOR / sizeof(uint16_t)];
    uint8_t corrupt_list[ROMFS_FLASH_SECTOR];
    memcpy(corrupt_map, map, sizeof(map));
    memcpy(corrupt_list, list, sizeof(list));
    memcpy(map, healthy_map, sizeof(map));
    memcpy(list, healthy_list, sizeof(list));
    if (writable) {
        CHECK(romfs_open_append("victim", &writer, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
        CHECK(romfs_seek_file(&writer, 0, SEEK_SET) == ROMFS_NOERR);
        uint8_t patch = 0xee;
        CHECK(romfs_write_file(&patch, 1, &writer) == 1);
        CHECK(writer.buffer_dirty);
    } else {
        CHECK(romfs_open_file("victim", &reader, read_io) == ROMFS_NOERR);
    }
    memcpy(map, corrupt_map, sizeof(map));
    memcpy(list, corrupt_list, sizeof(list));
    (writable ? &writer : &reader)->entry = native_entry(victim.nentry);
    test_flash_reset_counters();
}

static void reject_file_operations(void)
{
    /* First reject the persisted image, then corrupt each real open handle's
     * chain in RAM. Copies of handles are intentionally no longer usable. */
    romfs_file fresh = {0};
    CHECK(romfs_open_file("victim", &fresh, read_io) == ROMFS_ERR_OPERATION);
    CHECK(romfs_open_append("victim", &fresh, ROMFS_TYPE_MISC, io) == ROMFS_ERR_OPERATION);
    unchanged();
    open_before_live_corruption(false);
    memset(actual, 0xa7, sizeof(actual));
    CHECK(romfs_read_file(actual, UINT32_MAX, &reader) == 0 && reader.err == ROMFS_ERR_OPERATION);
    CHECK(reader.read_offset == 0);
    for (unsigned i = 0; i < sizeof(actual); i++) {
        CHECK(actual[i] == 0xa7);
    }
    CHECK(romfs_seek_file(&reader, 1, SEEK_SET) == ROMFS_ERR_OPERATION);
    CHECK(reader.read_offset == 0);
    uint16_t lookup[8];
    memset(lookup, 0xa7, sizeof(lookup));
    CHECK(romfs_read_map_table(lookup, 8, &reader) == 0 && reader.err == ROMFS_ERR_OPERATION);
    for (unsigned i = 0; i < 8; i++) {
        CHECK(lookup[i] == 0xa7a7);
    }
    CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    unchanged();
    open_before_live_corruption(true);
    romfs_file saved_writer = writer;
    CHECK(romfs_write_file(payload, 1, &writer) == 0 && writer.err == ROMFS_ERR_OPERATION);
    CHECK(writer.buffer_dirty && writer.write_offset == saved_writer.write_offset);
    writer = saved_writer;
    CHECK(romfs_seek_file(&writer, 0, SEEK_SET) == ROMFS_ERR_OPERATION);
    const uint32_t lengths[] = {0, 1, ROMFS_FLASH_SECTOR, ROMFS_FLASH_SECTOR + 7,
                                sizeof(payload), sizeof(payload) + 1};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        writer = saved_writer;
        CHECK(romfs_truncate_file(&writer, lengths[i]) == ROMFS_ERR_OPERATION);
        CHECK(writer.buffer_dirty && writer.entry.size == saved_writer.entry.size);
    }
    writer = saved_writer;
    CHECK(romfs_flush_file(&writer) == ROMFS_ERR_OPERATION && writer.buffer_dirty);
    writer = saved_writer;
    CHECK(romfs_close_file(&writer) == ROMFS_ERR_OPERATION && writer.buffer_dirty);
    CHECK(romfs_delete("victim") == ROMFS_ERR_OPERATION);
    unchanged();
}

static void reject_catalog_gc(void)
{
    romfs_entry *entries = (romfs_entry *) list;
    entries[reclaim.nentry].name[0] = ROMFS_DELETED_ENTRY;
    entries[victim.nentry].name[0] = ROMFS_DELETED_ENTRY;
    for (unsigned i = 0; i < sizeof(list) / sizeof(romfs_entry); i++) {
        if (entries[i].name[0] != ROMFS_EMPTY_ENTRY) {
            continue;
        }
        memset(&entries[i], 0, sizeof(entries[i]));
        snprintf(entries[i].name, sizeof(entries[i].name), "empty-%u", i);
        entries[i].attr.raw = le16(ROMFS_TYPE_MISC << ROMFS_TYPE_SHIFT);
        entries[i].start = le32(0xffff);
    }
    persist_fixture();
    snapshot();
    romfs_file attempt = {0};
    CHECK(romfs_create_file("new", &attempt, 0, ROMFS_TYPE_MISC, io) == ROMFS_ERR_OPERATION);
    CHECK(romfs_dir_create(&(romfs_dir){.id = ROMFS_ROOT_DIR_ID}, "new-dir", NULL) == ROMFS_ERR_OPERATION);
    CHECK(romfs_free() <= (LIMIT - FIRST) * ROMFS_FLASH_SECTOR);
    unchanged();
}

static void corruptions(void)
{
    struct corruption { const char *name; int field; uint32_t value; } cases[] = {
        {"start-free", -1, 0xffff}, {"start-overflow", -1, UINT32_MAX},
        {"start-physical-end", -1, LIMIT}, {"start-padding", -1, 2047},
        {"start-map-end", -1, 2048}, {"start-firmware", -1, 0},
        {"start-list", -1, 16}, {"start-map", -1, 17},
        {"link-free", 0, 0xffff}, {"link-physical-end", 0, LIMIT},
        {"link-padding", 0, 2047}, {"link-firmware", 0, 0},
        {"link-list", 0, 16}, {"link-map", 0, 17},
        {"short-first", 0, FIRST + 1}, {"short-middle", 1, FIRST + 2},
        {"middle-free", 1, 0xffff}, {"middle-outside", 1, LIMIT},
        {"two-sector-cycle", 1, FIRST + 1}, {"last-free", 2, 0xffff},
        {"last-outside", 2, LIMIT}, {"last-firmware", 2, 0},
        {"three-sector-cycle", 2, FIRST + 1}, {"tail-cycle", 2, FIRST + 2},
        {"extra-link", 2, FIRST + 4}, {"size-overflow", -2, UINT32_MAX},
        {"size-exceeds-flash", -2, IMAGE_SIZE}, {"size-short", -2, 2 * ROMFS_FLASH_SECTOR},
        {"size-long", -2, 4 * ROMFS_FLASH_SECTOR}, {"empty-with-chain", -2, 0},
        {"cycle-at-max-length", -3, (LIMIT - FIRST) * ROMFS_FLASH_SECTOR},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        case_name = cases[i].name;
        setup();
        romfs_entry *entry = &((romfs_entry *) list)[victim.nentry];
        if (cases[i].field == -1) {
            entry->start = le32(cases[i].value);
        } else if (cases[i].field == -2 || cases[i].field == -3) {
            entry->size = le32(cases[i].value);
            if (cases[i].field == -3) {
                map[victim.entry.start + 1] = le16((uint16_t) victim.entry.start);
            }
        } else {
            map[victim.entry.start + (unsigned) cases[i].field] = le16((uint16_t) cases[i].value);
        }
        persist_fixture();
        snapshot();
        reject_file_operations();
        reject_catalog_gc();
        printf("PASS: %s, file operations and GC reject without I/O or mutation\n", case_name);
    }
}

static void sector_gc(void)
{
    case_name = "sector-capacity-GC";
    setup();
    for (unsigned i = FIRST; i < LIMIT; i++) {
        if (map[i] == 0xffff) {
            map[i] = le16((uint16_t) i);
        }
    }
    ((romfs_entry *) list)[reclaim.nentry].name[0] = ROMFS_DELETED_ENTRY;
    ((romfs_entry *) list)[victim.nentry].name[0] = ROMFS_DELETED_ENTRY;
    map[victim.entry.start + 2] = le16((uint16_t) victim.entry.start);
    persist_fixture();
    CHECK(romfs_free() == 0); /* The bad tombstone blocks the entire GC batch. */
    romfs_file attempt = {0};
    CHECK(romfs_create_file("new", &attempt, 0, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    snapshot();
    CHECK(romfs_write_file(payload, ROMFS_FLASH_SECTOR, &attempt) == 0);
    CHECK(attempt.err == ROMFS_ERR_OPERATION);
    CHECK(romfs_close_file(&attempt) == ROMFS_ERR_OPERATION);
    unchanged();

    /* Repair only the fixture's bad link: the same allocation can now proceed. */
    map[victim.entry.start + 2] = le16((uint16_t) (victim.entry.start + 2));
    CHECK(romfs_free() == 4 * ROMFS_FLASH_SECTOR);
    CHECK(romfs_create_file("new", &attempt, 0, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    CHECK(romfs_write_file(payload, ROMFS_FLASH_SECTOR, &attempt) == ROMFS_FLASH_SECTOR);
    CHECK(attempt.entry.start == reclaim.entry.start);
    CHECK(romfs_close_file(&attempt) == ROMFS_NOERR);
    CHECK(romfs_free() == 3 * ROMFS_FLASH_SECTOR);
    CHECK(romfs_open_file("neighbor", &reader, read_io) == ROMFS_NOERR);
    CHECK(romfs_read_file(actual, ROMFS_FLASH_SECTOR, &reader) == ROMFS_FLASH_SECTOR);
    CHECK(memcmp(actual, payload, ROMFS_FLASH_SECTOR) == 0);
    CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    CHECK(test_flash_get_stats()->rejected == 0);
    puts("PASS: sector-capacity GC refusal and recovery after fixture repair");
}

static void valid_reads(void)
{
    case_name = "valid-reads-and-overflow";
    setup();
    CHECK(romfs_open_file("victim", &reader, read_io) == ROMFS_NOERR);
    CHECK(romfs_seek_file(&reader, 1, SEEK_SET) == ROMFS_NOERR);
    CHECK(romfs_read_file(actual, UINT32_MAX, &reader) == sizeof(payload) - 1);
    CHECK(reader.err == ROMFS_ERR_EOF);
    CHECK(memcmp(actual, payload + 1, sizeof(payload) - 1) == 0);
    uint16_t lookup[8];
    CHECK(romfs_read_map_table(lookup, 8, &reader) == 3);
    for (unsigned i = 0; i < 8; i++) {
        CHECK(lookup[i] == (i < 3 ? victim.entry.start + i : 0));
    }
    memset(lookup, 0xa7, sizeof(lookup));
    CHECK(romfs_read_map_table(lookup, 2, &reader) == 0 && reader.err == ROMFS_ERR_BUFFER_TOO_SMALL);
    CHECK(romfs_read_map_table(lookup, UINT32_MAX, &reader) == 0 && reader.err == ROMFS_ERR_OPERATION);
    for (unsigned i = 0; i < 8; i++) {
        CHECK(lookup[i] == 0xa7a7);
    }
    CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    CHECK(romfs_open_append("victim", &writer, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    CHECK(romfs_seek_file(&writer, INT32_MAX, SEEK_SET) == ROMFS_NOERR);
    CHECK(romfs_seek_file(&writer, INT32_MAX, SEEK_CUR) == ROMFS_NOERR);
    snapshot();
    CHECK(romfs_write_file(payload, 2, &writer) == 0 && writer.err == ROMFS_ERR_FILE_DATA_TOO_BIG);
    unchanged();
    CHECK(romfs_close_file(&writer) == ROMFS_ERR_FILE_DATA_TOO_BIG);

    const char *names[] = {"firmware", "flashlist", "flashmap"};
    for (unsigned n = 0; n < 3; n++) {
        CHECK(romfs_open_file(names[n], &reader, read_io) == ROMFS_NOERR);
        uint16_t service_map[16];
        uint32_t count = reader.entry.size / ROMFS_FLASH_SECTOR;
        CHECK(romfs_read_map_table(service_map, 16, &reader) == count);
        for (unsigned i = 0; i < count; i++) {
            CHECK(service_map[i] == reader.entry.start + i);
        }
        CHECK(romfs_seek_file(&reader, -1, SEEK_END) == ROMFS_NOERR);
        CHECK(romfs_read_file(actual, 1, &reader) == 1);
        CHECK(actual[0] == test_flash_data()[reader.entry.start * ROMFS_FLASH_SECTOR + reader.entry.size - 1]);
        CHECK(romfs_open_append(names[n], &writer, ROMFS_TYPE_MISC, io) == ROMFS_ERR_OPERATION);
        CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    }
    CHECK(test_flash_get_stats()->rejected == 0);
    puts("PASS: bounded reads, arithmetic overflow and read-only service chains");
}

static void service_and_buffer_corruption(void)
{
    case_name = "service-and-buffer-corruption";
    setup();
    open_before_live_corruption(true);
    writer.pos = neighbor.entry.start;
    snapshot();
    CHECK(romfs_flush_file(&writer) == ROMFS_ERR_OPERATION && writer.buffer_dirty);
    CHECK(romfs_truncate_file(&writer, 1) == ROMFS_ERR_OPERATION);
    unchanged();
    CHECK(romfs_close_file(&writer) == ROMFS_ERR_OPERATION);
    romfs_file attempt = {0};

    const char *names[] = {"firmware", "flashlist", "flashmap"};
    for (unsigned n = 0; n < 3; n++) {
        CHECK(romfs_open_file(names[n], &reader, read_io) == ROMFS_NOERR);
        unsigned slot = reader.nentry;
        romfs_entry original = ((romfs_entry *) list)[slot];
        ((romfs_entry *) list)[slot].start = le32(reader.entry.start + 1);
        snapshot();
        CHECK(romfs_open_file(names[n], &attempt, read_io) == ROMFS_ERR_OPERATION);
        unchanged();
        ((romfs_entry *) list)[slot] = original;
        ((romfs_entry *) list)[slot].size = le32(reader.entry.size - 1);
        snapshot();
        CHECK(romfs_open_file(names[n], &attempt, read_io) == ROMFS_ERR_OPERATION);
        unchanged();
        ((romfs_entry *) list)[slot] = original;

        /* A corrupt live service link is not allowed to enter user data. */
        uint16_t original_link = map[reader.entry.start];
        map[reader.entry.start] = le16(FIRST);
        snapshot();
        CHECK(romfs_read_file(actual, 1, &reader) == 0 && reader.err == ROMFS_ERR_OPERATION);
        unchanged();
        map[reader.entry.start] = original_link;
        CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    }
    puts("PASS: service metadata/ranges and dirty buffer sector consistency");

    CHECK(test_flash_init(IMAGE_SIZE));
    CHECK(romfs_start(0, IMAGE_SIZE, map, list));
    CHECK(romfs_format());
    CHECK(romfs_open_file("firmware", &reader, read_io) == ROMFS_NOERR);
    CHECK(reader.entry.size == 0);
    CHECK(romfs_read_file(actual, 1, &reader) == 0 && reader.err == ROMFS_ERR_EOF);
    CHECK(romfs_read_map_table(NULL, 0, &reader) == 0 && reader.err == ROMFS_NOERR);
    CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    create_file("empty", &attempt, 0);
    CHECK(romfs_open_file("empty", &reader, read_io) == ROMFS_NOERR);
    CHECK(romfs_read_file(actual, 1, &reader) == 0 && reader.err == ROMFS_ERR_EOF);
    CHECK(romfs_read_map_table(NULL, 0, &reader) == 0 && reader.err == ROMFS_NOERR);
    CHECK(romfs_seek_file(&reader, 0, SEEK_END) == ROMFS_NOERR);
    CHECK(romfs_close_file(&reader) == ROMFS_NOERR);
    CHECK(test_flash_get_stats()->rejected == 0);
    puts("PASS: empty user file and zero-length firmware service entry");
}

int main(void)
{
    saved_flash = malloc(IMAGE_SIZE);
    CHECK(saved_flash);
    for (unsigned i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t) (i * 37u + i / ROMFS_FLASH_SECTOR);
    }
    corruptions();
    sector_gc();
    valid_reads();
    service_and_buffer_corruption();
    test_flash_destroy();
    free(saved_flash);
    return 0;
}
