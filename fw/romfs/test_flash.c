#include <stdlib.h>
#include <string.h>

#include "romfs.h"
#include "test_flash.h"

static uint8_t *image;
static uint32_t image_size;
static test_flash_stats stats;
static uint64_t fail_at[TEST_FLASH_OPERATION_COUNT];

void test_flash_reset_counters(void)
{
    memset(&stats, 0, sizeof(stats));
    memset(fail_at, 0, sizeof(fail_at));
}

void test_flash_destroy(void)
{
    free(image);
    image = NULL;
    image_size = 0;
    test_flash_reset_counters();
}

bool test_flash_init(uint32_t size)
{
    test_flash_destroy();
    if (size == 0 || size % ROMFS_FLASH_SECTOR != 0) {
        return false;
    }
    image = malloc(size);
    if (!image) {
        return false;
    }
    image_size = size;
    memset(image, 0xff, image_size);
    return true;
}

uint8_t *test_flash_data(void)
{
    return image;
}

const test_flash_stats *test_flash_get_stats(void)
{
    return &stats;
}

bool test_flash_fail_on(test_flash_operation operation, uint64_t nth)
{
    if ((unsigned)operation >= TEST_FLASH_OPERATION_COUNT ||
            nth > UINT64_MAX - stats.calls[operation]) {
        return false;
    }
    fail_at[operation] = nth ? stats.calls[operation] + nth : 0;
    return true;
}

const char *test_flash_operation_name(test_flash_operation operation)
{
    switch (operation) {
    case TEST_FLASH_READ: return "read";
    case TEST_FLASH_ERASE: return "erase";
    case TEST_FLASH_WRITE: return "write";
    default: return "unknown";
    }
}

static bool fail_operation(test_flash_operation operation, uint32_t offset,
                           const char *reason, bool injected)
{
    if (!stats.first_failure_reason) {
        stats.first_failure_op = operation;
        stats.first_failure_offset = offset;
        stats.first_failure_reason = reason;
    }
    if (injected) {
        stats.injected++;
    } else {
        stats.rejected++;
    }
    return false;
}

static bool validate_operation(test_flash_operation operation, uint32_t offset,
                               uint32_t length, const void *buffer)
{
    stats.calls[operation]++;
    if (!image || offset > image_size || length > image_size - offset) {
        return fail_operation(operation, offset, "outside physical flash", false);
    }
    if (operation != TEST_FLASH_READ && offset % ROMFS_FLASH_SECTOR != 0) {
        return fail_operation(operation, offset, "unaligned sector", false);
    }
    if (operation != TEST_FLASH_ERASE && !buffer) {
        return fail_operation(operation, offset, "null buffer", false);
    }
    return true;
}

static bool inject_failure(test_flash_operation operation, uint32_t offset)
{
    if (fail_at[operation] && stats.calls[operation] == fail_at[operation]) {
        fail_at[operation] = 0;
        fail_operation(operation, offset, "injected I/O failure", true);
        return true;
    }
    return false;
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)
{
    if (!validate_operation(TEST_FLASH_READ, offset, need, buffer) ||
            inject_failure(TEST_FLASH_READ, offset)) {
        return false;
    }
    memcpy(buffer, image + offset, need);
    return true;
}

bool romfs_flash_sector_erase(uint32_t offset)
{
    if (!validate_operation(TEST_FLASH_ERASE, offset, ROMFS_FLASH_SECTOR, NULL) ||
            inject_failure(TEST_FLASH_ERASE, offset)) {
        return false;
    }
    memset(image + offset, 0xff, ROMFS_FLASH_SECTOR);
    return true;
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
    if (!validate_operation(TEST_FLASH_WRITE, offset, ROMFS_FLASH_SECTOR, buffer)) {
        return false;
    }
    /* Validate the whole request before changing any byte. */
    for (uint32_t i = 0; i < ROMFS_FLASH_SECTOR; i++) {
        if ((image[offset + i] & buffer[i]) != buffer[i]) {
            return fail_operation(TEST_FLASH_WRITE, offset, "programming 0 to 1 without erase", false);
        }
    }
    if (inject_failure(TEST_FLASH_WRITE, offset)) {
        return false;
    }
    memcpy(image + offset, buffer, ROMFS_FLASH_SECTOR);
    return true;
}
