/* Host hardware substitutes. platform_functions.inc is extracted verbatim
 * from the platform callbacks and USB handler by test_platform.py. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "romfs_flash.h"
#include "../../utils/utils2.h"
#include "../../rom/src/main.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static const struct flash_chip chip = {.rom_size = 2};
static const struct flash_chip *used_flash_chip = &chip;
static uint32_t firmware_size = 65537;
static unsigned erase_calls, write_calls, mode_calls, interrupt_calls, modified_calls;
static uint32_t last_offset;
static bool flash_result = true, quad_mode = true, cart_interrupt = true;

static uint8_t __flash_binary_end;
#define XIP_BASE ((uintptr_t) &__flash_binary_end - firmware_size)
#define C0_INTERRUPT_CART 0x800
#define C0_STATUS() (cart_interrupt ? C0_INTERRUPT_CART : 0)
#define REVERSER16(x) __builtin_bswap16(x)
#define REVERSER32(x) __builtin_bswap32(x)
#define FIRMWARE_VERSION 0x1234
#define EP1_OUT_ADDR 1
#define EP2_IN_ADDR 0x82

#if TEST_N64
static int flash_access_lock_depth;
static bool flash_access_restore_cart;
#endif

static void set_CART_interrupt(bool enabled)
{
    interrupt_calls++;
    cart_interrupt = enabled;
}

static uint32_t n64cart_fw_size(void)
{
    return firmware_size;
}

const struct flash_chip *get_flash_info(void)
{
    return used_flash_chip;
}

static void flash_mode(bool quad)
{
    mode_calls++;
    quad_mode = quad;
}

static void check_hardware_access(uint32_t offset)
{
    CHECK(offset >= 98304 && offset <= 2 * ROMFS_MB - ROMFS_FLASH_SECTOR);
    CHECK(offset % ROMFS_FLASH_SECTOR == 0);
#if TEST_N64
    CHECK(!quad_mode && !cart_interrupt && flash_access_lock_depth == 1);
#endif
    last_offset = offset;
}

static bool flash_erase_sector(uint32_t offset)
{
    check_hardware_access(offset);
    erase_calls++;
    return flash_result;
}

static bool flash_write_sector(uint32_t offset, uint8_t *buffer)
{
    CHECK(buffer);
    check_hardware_access(offset);
    write_calls++;
    return flash_result;
}

static void flash_read(uint32_t offset, uint8_t *buffer, uint32_t count)
{
    (void) offset;
    memset(buffer, 0xff, count);
}

static void flash_quad_exit_cont_read_mode(void) {}
static void flash_spi_mode(void) {}
static void flash_quad_cont_read_mode(void) {}
static void reset_usb_boot(int mask, int disable) { (void) mask; (void) disable; }
static void watchdog_reboot(int pc, int sp, int ms) { (void) pc; (void) sp; (void) ms; }
void n64cart_note_usb_activity(void) {}
void n64cart_set_usb_display_mode(bool enabled) { (void) enabled; }
void n64cart_note_usb_romfs_modified(void) { modified_calls++; }

struct usb_endpoint_configuration { int address; };
static struct usb_endpoint_configuration endpoint;
static struct ack_header reply;
static unsigned replies;
static struct usb_endpoint_configuration *usb_get_endpoint_configuration(int address)
{
    endpoint.address = address;
    return &endpoint;
}

static void usb_start_transfer(struct usb_endpoint_configuration *ep, uint8_t *buffer, uint16_t len)
{
    if (ep->address == EP2_IN_ADDR) {
        CHECK(buffer && len == sizeof(reply));
        memcpy(&reply, buffer, sizeof(reply));
        replies++;
    }
}

static uint8_t sector_buffer[ROMFS_FLASH_SECTOR];
static int sector_buffer_pos, current_req, flash_stage;
static uint32_t rw_sector_offset;
static struct ack_header ackn;
static bool spi_mode_ack_pending;

/* Log calls do not participate in these tests. */
#define syslog(...) ((void) 0)
#define printf(...) ((void) 0)
#include "platform_functions.inc"
#undef printf

static uint16_t wire16(uint16_t value)
{
    return TEST_N64 ? __builtin_bswap16(value) : value;
}

static uint32_t wire32(uint32_t value)
{
    return TEST_N64 ? __builtin_bswap32(value) : value;
}

static void reset_observations(void)
{
    erase_calls = write_calls = mode_calls = interrupt_calls = modified_calls = replies = 0;
    flash_result = true;
}

static void check_no_hardware(void)
{
    CHECK(erase_calls == 0 && write_calls == 0 && mode_calls == 0 && interrupt_calls == 0);
    CHECK(modified_calls == 0);
}

static void command(uint16_t type, uint32_t offset, uint16_t expected)
{
    struct req_header req = {.type = wire16(type), .offset = wire32(offset)};
    unsigned before = replies;
    ep1_out_handler((uint8_t *) &req, sizeof(req));
    CHECK(replies == before + 1 && reply.type == wire16(expected));
}

static void check_idle(void)
{
    CHECK(current_req == 0 && flash_stage == 0 && sector_buffer_pos == 0);
}

static void send_data(uint16_t expected_final)
{
    uint8_t packet[64];
    memset(packet, 0xa5, sizeof(packet));
    for (unsigned i = 0; i < ROMFS_FLASH_SECTOR / sizeof(packet); i++) {
        unsigned before = replies;
        ep1_out_handler(packet, sizeof(packet));
        uint16_t expected = (i + 1 == ROMFS_FLASH_SECTOR / sizeof(packet)) ? expected_final : ACK_NOERROR;
        CHECK(replies == before + 1 && reply.type == wire16(expected));
    }
    check_idle();
}

int main(void)
{
    uint8_t data[ROMFS_FLASH_SECTOR] = {0};
    const uint32_t start = 98304, size = 2 * ROMFS_MB;
    const uint32_t bad[] = {0, start - ROMFS_FLASH_SECTOR, start - 1, start + 1,
                            size, size + ROMFS_FLASH_SECTOR, UINT32_MAX, UINT32_MAX - 4095};
    CHECK(get_romfs_start_offset() == start);
    CHECK(!romfs_flash_sector_in_range(0, 0, 0));
    CHECK(!romfs_flash_sector_in_range(0, 0, ROMFS_FLASH_SECTOR - 1));
    CHECK(!romfs_flash_sector_in_range(start, start, size + 1));
    CHECK(!romfs_flash_sector_in_range(start, start + 1, size));
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset_observations();
        CHECK(!romfs_flash_sector_erase(bad[i]));
        CHECK(!romfs_flash_sector_write(bad[i], data));
        check_no_hardware();
        command(CART_ERASE_SEC, bad[i], ACK_ERROR);
        check_idle();
        command(CART_WRITE_SEC, bad[i], ACK_ERROR);
        check_idle();
        check_no_hardware();
        command(CART_INFO, 0, ACK_NOERROR);
        CHECK(reply.info.start == wire32(start) && reply.info.size == wire32(size));
        command(CART_ERASE_SEC, start, ACK_NOERROR);
        CHECK(erase_calls == 1 && last_offset == start);
        check_idle();
    }
    reset_observations();
    CHECK(!romfs_flash_sector_write(start, NULL));
    check_no_hardware();
    used_flash_chip = NULL;
    CHECK(!romfs_flash_sector_erase(start));
    CHECK(!romfs_flash_sector_write(start, data));
    command(CART_WRITE_SEC, start, ACK_ERROR);
    command(CART_ERASE_SEC, start, ACK_ERROR);
    check_idle();
    check_no_hardware();
    used_flash_chip = &chip;
    firmware_size = UINT32_MAX;
    CHECK(!romfs_flash_sector_erase(start));
    CHECK(!romfs_flash_sector_write(start, data));
    check_no_hardware();
    firmware_size = 65537;

    const uint32_t good[] = {start, start + ROMFS_FLASH_SECTOR, size - ROMFS_FLASH_SECTOR};
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        for (unsigned result = 0; result <= 1; result++) {
            reset_observations();
            flash_result = result;
            cart_interrupt = result;
            CHECK(romfs_flash_sector_erase(good[i]) == (bool) result);
            CHECK(romfs_flash_sector_write(good[i], data) == (bool) result);
            CHECK(erase_calls == 1 && write_calls == 1 && last_offset == good[i]);
            CHECK(quad_mode && cart_interrupt == (bool) result);
            CHECK(mode_calls == (TEST_N64 ? 4u : 0u));
            CHECK(interrupt_calls == (TEST_N64 ? 4u : 0u));
        }
        reset_observations();
        command(CART_WRITE_SEC, good[i], ACK_NOERROR);
        CHECK(flash_stage == 1 && current_req == CART_WRITE_SEC);
        check_no_hardware();
        send_data(ACK_NOERROR);
        CHECK(write_calls == 1 && last_offset == good[i]);
        CHECK(modified_calls == (TEST_N64 ? 1u : 0u));
    }

    reset_observations();
    command(CART_WRITE_SEC, start, ACK_NOERROR);
    flash_result = false;
    send_data(ACK_ERROR);
    CHECK(write_calls == 1 && modified_calls == 0);
    command(CART_ERASE_SEC, start, ACK_ERROR);
    CHECK(erase_calls == 1 && modified_calls == 0);
    check_idle();
    flash_result = true;
    command(CART_WRITE_SEC, start, ACK_NOERROR);
    send_data(ACK_NOERROR);

    /* A boundary change during reception must be caught again at commit. */
    reset_observations();
    command(CART_WRITE_SEC, start, ACK_NOERROR);
    firmware_size = start + 1;
    send_data(ACK_ERROR);
    check_no_hardware();
    firmware_size = 65537;
    command(CART_WRITE_SEC, start, ACK_NOERROR);
    uint8_t short_packet[63] = {0};
    unsigned before = replies;
    ep1_out_handler(short_packet, sizeof(short_packet));
    CHECK(replies == before + 1 && reply.type == wire16(ACK_ERROR));
    check_idle();
    check_no_hardware();
    command(CART_WRITE_SEC, start, ACK_NOERROR);
    send_data(ACK_NOERROR);
    CHECK(write_calls == 1);
    printf("PASS: %s callbacks and USB guards, ACK encoding and recovery\n", TEST_N64 ? "N64" : "ARM");
    return 0;
}
