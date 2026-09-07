#include <stdio.h>
#include <string.h>

#include "romfs.h"
#include "test_flash.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        test_flash_destroy(); \
        return 1; \
    } \
} while (0)

int main(void)
{
    const uint32_t size = 2 * ROMFS_FLASH_SECTOR;
    uint8_t buffer[ROMFS_FLASH_SECTOR], readback[ROMFS_FLASH_SECTOR];
    CHECK(!test_flash_init(0));
    CHECK(!test_flash_init(size - 1));
    CHECK(test_flash_init(size));

    memset(buffer, 0xff, sizeof(buffer));
    CHECK(romfs_flash_sector_read(ROMFS_FLASH_SECTOR, readback, sizeof(readback)));
    CHECK(memcmp(buffer, readback, sizeof(buffer)) == 0);
    memset(buffer, 0xa5, sizeof(buffer));
    CHECK(romfs_flash_sector_write(ROMFS_FLASH_SECTOR, buffer));
    CHECK(romfs_flash_sector_read(ROMFS_FLASH_SECTOR, readback, sizeof(readback)));
    CHECK(memcmp(buffer, readback, sizeof(buffer)) == 0);
    memset(buffer, 0x05, sizeof(buffer));
    CHECK(romfs_flash_sector_write(ROMFS_FLASH_SECTOR, buffer));

    /* An invalid bit at the end must not partially program preceding bytes. */
    memset(buffer, 0, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = 0xff;
    CHECK(!romfs_flash_sector_write(ROMFS_FLASH_SECTOR, buffer));
    memset(buffer, 0x05, sizeof(buffer));
    CHECK(romfs_flash_sector_read(ROMFS_FLASH_SECTOR, readback, sizeof(readback)));
    CHECK(memcmp(buffer, readback, sizeof(buffer)) == 0);
    CHECK(romfs_flash_sector_erase(ROMFS_FLASH_SECTOR));
    memset(buffer, 0xff, sizeof(buffer));
    CHECK(romfs_flash_sector_read(ROMFS_FLASH_SECTOR, readback, sizeof(readback)));
    CHECK(memcmp(buffer, readback, sizeof(buffer)) == 0);
    CHECK(test_flash_get_stats()->calls[TEST_FLASH_READ] == 4);
    CHECK(test_flash_get_stats()->calls[TEST_FLASH_WRITE] == 3);
    CHECK(test_flash_get_stats()->calls[TEST_FLASH_ERASE] == 1);
    CHECK(test_flash_get_stats()->rejected == 1);

    test_flash_reset_counters();
    memset(readback, 0x5a, sizeof(readback));
    CHECK(!romfs_flash_sector_read(size - 1, readback, 2));
    CHECK(!romfs_flash_sector_read(UINT32_MAX, readback, 2));
    CHECK(!romfs_flash_sector_read(1, readback, UINT32_MAX));
    CHECK(!romfs_flash_sector_read(0, NULL, 1));
    CHECK(!romfs_flash_sector_erase(size));
    CHECK(!romfs_flash_sector_erase(1));
    CHECK(!romfs_flash_sector_write(size, buffer));
    CHECK(!romfs_flash_sector_write(1, buffer));
    CHECK(!romfs_flash_sector_write(0, NULL));
    CHECK(romfs_flash_sector_read(size, readback, 0));
    for (size_t i = 0; i < sizeof(readback); i++) {
        CHECK(readback[i] == 0x5a);
    }
    CHECK(test_flash_get_stats()->rejected == 9);
    CHECK(test_flash_get_stats()->injected == 0);

    for (test_flash_operation op = TEST_FLASH_READ; op < TEST_FLASH_OPERATION_COUNT; op++) {
        CHECK(test_flash_init(size));
        memset(buffer, 0, sizeof(buffer));
        CHECK(romfs_flash_sector_write(0, buffer));
        test_flash_reset_counters();
        CHECK(test_flash_fail_on(op, 2));
        for (unsigned i = 1; i <= 3; i++) {
            bool ok;
            if (op == TEST_FLASH_READ) {
                readback[0] = 0x5a;
                ok = romfs_flash_sector_read(0, readback, 1);
                CHECK(readback[0] == (i == 2 ? 0x5a : 0));
            } else if (op == TEST_FLASH_ERASE) {
                test_flash_data()[0] = 0;
                ok = romfs_flash_sector_erase(0);
                CHECK(test_flash_data()[0] == (i == 2 ? 0 : 0xff));
            } else {
                test_flash_data()[0] = 0xff;
                ok = romfs_flash_sector_write(0, buffer);
                CHECK(test_flash_data()[0] == (i == 2 ? 0xff : 0));
            }
            CHECK(ok == (i != 2));
        }
        CHECK(test_flash_get_stats()->calls[op] == 3);
        CHECK(test_flash_get_stats()->injected == 1);
        CHECK(test_flash_get_stats()->rejected == 0);
        CHECK(test_flash_fail_on(op, 1));
        test_flash_reset_counters();
        CHECK(test_flash_get_stats()->first_failure_reason == NULL);
        if (op == TEST_FLASH_READ) {
            CHECK(romfs_flash_sector_read(0, readback, 1));
        } else if (op == TEST_FLASH_ERASE) {
            CHECK(romfs_flash_sector_erase(0));
        } else {
            CHECK(romfs_flash_sector_write(0, buffer));
        }
        CHECK(test_flash_get_stats()->injected == 0);
    }

    CHECK(test_flash_fail_on(TEST_FLASH_READ, 1));
    CHECK(test_flash_init(size));
    CHECK(romfs_flash_sector_read(0, readback, sizeof(readback)));
    memset(buffer, 0xff, sizeof(buffer));
    CHECK(memcmp(buffer, readback, sizeof(buffer)) == 0);
    CHECK(test_flash_get_stats()->injected == 0);
    CHECK(!test_flash_fail_on(TEST_FLASH_OPERATION_COUNT, 1));
    CHECK(!test_flash_fail_on(TEST_FLASH_READ, UINT64_MAX));
    CHECK(test_flash_fail_on(TEST_FLASH_READ, 1));
    CHECK(test_flash_fail_on(TEST_FLASH_READ, 0));
    CHECK(romfs_flash_sector_read(0, readback, 1));
    test_flash_destroy();
    CHECK(test_flash_data() == NULL);
    CHECK(!romfs_flash_sector_read(0, readback, 1));
    test_flash_destroy();
    puts("Flash emulator tests passed.");
    return 0;
}
