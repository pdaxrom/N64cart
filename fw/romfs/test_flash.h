#ifndef ROMFS_TEST_FLASH_H
#define ROMFS_TEST_FLASH_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TEST_FLASH_READ,
    TEST_FLASH_ERASE,
    TEST_FLASH_WRITE,
    TEST_FLASH_OPERATION_COUNT
} test_flash_operation;

typedef struct {
    uint64_t calls[TEST_FLASH_OPERATION_COUNT];
    uint64_t rejected;
    uint64_t injected;
    test_flash_operation first_failure_op;
    uint32_t first_failure_offset;
    const char *first_failure_reason;
} test_flash_stats;

/* One active image, matching the ROMFS callback interface. init/destroy clear
 * the image, counters and fault settings. A rejected operation changes neither
 * the image nor the caller's read buffer. */
bool test_flash_init(uint32_t size);
void test_flash_destroy(void);

/* Direct fixture access only; filesystem I/O must use the ROMFS callbacks. */
uint8_t *test_flash_data(void);
const test_flash_stats *test_flash_get_stats(void);

/* Counters reset independently of the image; resetting also disarms faults. */
void test_flash_reset_counters(void);

/* Fail the nth subsequent callback of this kind once; zero disarms it.
 * Counts include invalid requests. Invalid requests are classified as rejected
 * before fault injection, so they cannot be mistaken for an expected failure. */
bool test_flash_fail_on(test_flash_operation operation, uint64_t nth);
const char *test_flash_operation_name(test_flash_operation operation);

#endif
