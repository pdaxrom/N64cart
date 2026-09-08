#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "romfs.h"
#include "romfs_flash.h"
#include "test_flash.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static uint16_t le16(uint16_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return (value >> 8) | (value << 8);
#endif
}

static void check_io(void)
{
    CHECK(test_flash_get_stats()->rejected == 0);
    CHECK(test_flash_get_stats()->injected == 0);
}

static void write_sector(const char *name, romfs_file *file, uint8_t *io, uint8_t *data)
{
    CHECK(romfs_create_file(name, file, 0, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    CHECK(romfs_write_file(data, ROMFS_FLASH_SECTOR, file) == ROMFS_FLASH_SECTOR);
    CHECK(romfs_close_file(file) == ROMFS_NOERR);
}

static void read_sector(const char *name, uint8_t *io, uint8_t *data)
{
    romfs_file file = {0};
    uint8_t actual[ROMFS_FLASH_SECTOR];
    CHECK(romfs_open_file(name, &file, io) == ROMFS_NOERR);
    CHECK(romfs_read_file(actual, sizeof(actual), &file) == sizeof(actual));
    CHECK(memcmp(actual, data, sizeof(actual)) == 0);
    CHECK(romfs_close_file(&file) == ROMFS_NOERR);
}

static void check_capacity(uint32_t mb, uint32_t expected_map, uint32_t expected_list, uint32_t first)
{
    uint32_t size = mb * ROMFS_MB, map_size, list_size;
    const uint32_t raw_start = 65537, aligned_start = 98304;
    uint32_t limit = size / ROMFS_FLASH_SECTOR;
    if (limit > UINT16_MAX) {
        limit = UINT16_MAX;
    }
    romfs_get_buffers_sizes(size, &map_size, &list_size);
    CHECK(map_size == expected_map && list_size == expected_list);
    uint16_t *map = malloc(map_size);
    uint8_t *list = malloc(list_size);
    uint8_t io[ROMFS_FLASH_SECTOR], data[ROMFS_FLASH_SECTOR];
    CHECK(map && list && test_flash_init(size));
    memset(test_flash_data(), 0x5a, aligned_start);
    memset(data, 0xa5, sizeof(data));

    /* A size query for another image must not determine this mount's layout. */
    romfs_get_buffers_sizes(32 * ROMFS_MB, NULL, NULL);
    CHECK(romfs_start(raw_start, size, map, list));
    CHECK(romfs_format());
    CHECK(romfs_free() == (limit - first) * ROMFS_FLASH_SECTOR);
    romfs_entry entry;
    CHECK(romfs_get_entry("firmware", &entry) == ROMFS_NOERR);
    CHECK(entry.start == 0 && entry.size == aligned_start);
    CHECK(romfs_get_entry("flashlist", &entry) == ROMFS_NOERR);
    CHECK(entry.start == 24 && entry.size == expected_list);
    CHECK(romfs_get_entry("flashmap", &entry) == ROMFS_NOERR);
    CHECK(entry.start == 24 + expected_list / ROMFS_FLASH_SECTOR && entry.size == expected_map);

    romfs_file old = {0}, bulk = {0}, last = {0}, full = {0}, replacement = {0};
    write_sector("old", &old, io, data);
    CHECK(old.entry.start == first);
    write_sector("bulk", &bulk, io, data);
    CHECK(bulk.entry.start == first + 1);

    /* Persist a valid chain up to the penultimate usable sector. Constructing
     * it directly avoids quadratic full-device writes in this boundary test. */
    CHECK(romfs_open_append("bulk", &bulk, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    for (uint32_t i = bulk.entry.start; i < limit - 1; i++) {
        map[i] = le16((uint16_t) ((i + 1 < limit - 1) ? i + 1 : i));
    }
    bulk.entry.size = (limit - 1 - bulk.entry.start) * ROMFS_FLASH_SECTOR;
    CHECK(romfs_sync_full() == ROMFS_NOERR); /* Persist the directly constructed map. */
    CHECK(romfs_close_file(&bulk) == ROMFS_NOERR);
    CHECK(romfs_start(raw_start, size, map, list));
    CHECK(romfs_free() == ROMFS_FLASH_SECTOR);
    write_sector("last", &last, io, data);
    CHECK(last.entry.start == limit - 1);
    CHECK(romfs_free() == 0);
    read_sector("last", io, data);

    CHECK(romfs_create_file("full", &full, 0, ROMFS_TYPE_MISC, io) == ROMFS_NOERR);
    CHECK(romfs_write_file(data, sizeof(data), &full) == 0);
    CHECK(full.err == ROMFS_ERR_NO_SPACE);
    CHECK(romfs_close_file(&full) == ROMFS_NOERR);
    CHECK(romfs_delete("old") == ROMFS_NOERR);
    CHECK(romfs_start(raw_start, size, map, list));
    CHECK(romfs_free() == ROMFS_FLASH_SECTOR);
    write_sector("replacement", &replacement, io, data);
    CHECK(replacement.entry.start == old.entry.start);
    CHECK(romfs_free() == 0);
    CHECK(romfs_start(raw_start, size, map, list));
    read_sector("replacement", io, data);
    read_sector("last", io, data);
    CHECK(romfs_free() == 0);

    for (uint32_t i = limit; i < map_size / sizeof(uint16_t); i++) {
        CHECK(map[i] == 0xffff);
    }
    for (uint32_t i = 0; i < aligned_start; i++) {
        CHECK(test_flash_data()[i] == 0x5a);
    }
    if (mb == 256) {
        for (uint32_t i = size - ROMFS_FLASH_SECTOR; i < size; i++) {
            CHECK(test_flash_data()[i] == 0xff);
        }
    }
    check_io();
    printf("PASS: %u MiB geometry, last usable sector, ENOSPC and GC reuse\n", mb);
    free(map);
    free(list);
    test_flash_destroy();
}

static void check_invalid_geometry(void)
{
    const uint32_t size = 2 * ROMFS_MB, start = 65536;
    uint16_t map[ROMFS_FLASH_SECTOR / sizeof(uint16_t)];
    uint8_t list[ROMFS_FLASH_SECTOR], io[ROMFS_FLASH_SECTOR], data[ROMFS_FLASH_SECTOR];
    uint16_t saved_map[ROMFS_FLASH_SECTOR / sizeof(uint16_t)];
    uint8_t saved_list[ROMFS_FLASH_SECTOR];
    CHECK(test_flash_init(size));
    CHECK(romfs_start(start, size, map, list));
    CHECK(romfs_format());
    memset(data, 0x3c, sizeof(data));
    romfs_file file = {0};
    write_sector("keep", &file, io, data);
    uint32_t free_before = romfs_free();
    memcpy(saved_map, map, sizeof(map));
    memcpy(saved_list, list, sizeof(list));
    test_flash_reset_counters();

    const uint32_t invalid_sizes[] = {0, 1, size - 1, size + 1, 256u * ROMFS_MB + ROMFS_FLASH_SECTOR, UINT32_MAX};
    for (size_t i = 0; i < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); i++) {
        uint32_t map_size = 1, list_size = 1;
        romfs_get_buffers_sizes(invalid_sizes[i], &map_size, &list_size);
        CHECK(map_size == 0 && list_size == 0);
        CHECK(!romfs_start(start, invalid_sizes[i], map, list));
    }
    const uint32_t invalid_starts[] = {size - 1, size, size + 1, UINT32_MAX, UINT32_MAX - 32766};
    for (size_t i = 0; i < sizeof(invalid_starts) / sizeof(invalid_starts[0]); i++) {
        CHECK(!romfs_start(invalid_starts[i], size, map, list));
    }
    CHECK(!romfs_start(0, ROMFS_FLASH_SECTOR, map, list));
    CHECK(!romfs_start(0, 2 * ROMFS_FLASH_SECTOR, map, list));
    CHECK(!romfs_start(start, size, NULL, list));
    CHECK(!romfs_start(start, size, map, NULL));
    CHECK(!romfs_start(start, size, (uint16_t *) ((uint8_t *) map + 1), list));
    romfs_get_buffers_sizes(256u * ROMFS_MB, NULL, NULL);
    for (unsigned op = 0; op < TEST_FLASH_OPERATION_COUNT; op++) {
        CHECK(test_flash_get_stats()->calls[op] == 0);
    }
    CHECK(memcmp(map, saved_map, sizeof(map)) == 0);
    CHECK(memcmp(list, saved_list, sizeof(list)) == 0);
    CHECK(romfs_free() == free_before);
    read_sector("keep", io, data);
    write_sector("after-rejection", &file, io, data);
    CHECK(romfs_free() == free_before - ROMFS_FLASH_SECTOR);
    CHECK(romfs_start(start, size, map, list));
    read_sector("keep", io, data);
    read_sector("after-rejection", io, data);
    check_io();
    test_flash_destroy();
    puts("PASS: invalid geometry leaves buffers, active mount and flash untouched");
}

int main(void)
{
    CHECK(romfs_align_flash_start(0) == 0);
    CHECK(romfs_align_flash_start(32768) == 32768);
    CHECK(romfs_align_flash_start(32769) == 65536);
    CHECK(romfs_align_flash_start(UINT32_MAX - 32767) == UINT32_MAX - 32767);
    CHECK(romfs_align_flash_start(UINT32_MAX - 32766) == UINT32_MAX);
    CHECK(romfs_align_flash_start(UINT32_MAX) == UINT32_MAX);
    check_invalid_geometry();
    check_capacity(2, 4096, 4096, 26);
    check_capacity(4, 4096, 4096, 26);
    check_capacity(8, 4096, 4096, 26);
    check_capacity(16, 8192, 4096, 27);
    check_capacity(32, 16384, 4096, 29);
    check_capacity(64, 32768, 4096, 33);
    check_capacity(128, 65536, 8192, 42);
    check_capacity(256, 131072, 16384, 60);
    return 0;
}
