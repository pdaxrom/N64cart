#include <stdio.h>

//#include "hardware/flash.h"
#include "flashrom.h"
#include "pico/bootrom.h"

#include "hardware/structs/ssi.h"
#include "hardware/structs/ioqspi.h"

//#include "hardware/regs/io_qspi.h"
//#include "hardware/regs/pads_qspi.h"
//#include "hardware/structs/ssi.h"
//#include "hardware/structs/xip_ctrl.h"
//#include "hardware/resets.h"

#define FLASH_BLOCK_ERASE_CMD 0xd8

#if 1
#define BOOT2_SIZE_WORDS 64

static uint32_t boot2_copyout[BOOT2_SIZE_WORDS];
static bool boot2_copyout_valid = false;

static void __no_inline_not_in_flash_func(flash_init_boot2_copyout)(void) {
    if (boot2_copyout_valid)
        return;
    for (int i = 0; i < BOOT2_SIZE_WORDS; ++i)
        boot2_copyout[i] = ((uint32_t *)XIP_BASE)[i];
    __compiler_memory_barrier();
    boot2_copyout_valid = true;
}

static void __no_inline_not_in_flash_func(flash_enable_xip_via_boot2)(void) {
    ((void (*)(void))boot2_copyout+1)();
}
#else
static void __no_inline_not_in_flash_func(flash_init_boot2_copyout)(void) {}

static void __no_inline_not_in_flash_func(flash_enable_xip_via_boot2)(void) {
    // Set up XIP for 03h read on bus access (slow but generic)
    rom_flash_enter_cmd_xip_fn flash_enter_cmd_xip = (rom_flash_enter_cmd_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_ENTER_CMD_XIP);
    assert(flash_enter_cmd_xip);
    flash_enter_cmd_xip();
}
#endif

static void __no_inline_not_in_flash_func(flash_cs_force)(bool high) {
    uint32_t field_val = high ?
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_HIGH :
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_LOW;
    hw_write_masked(&ioqspi_hw->io[1].ctrl,
        field_val << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB,
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS
    );
}

static void __no_inline_not_in_flash_func(xflash_do_cmd_internal)(const uint8_t *txbuf, uint8_t *rxbuf, size_t count) 
{
    flash_cs_force(0);
    size_t tx_remaining = count;
    size_t rx_remaining = count;
    // We may be interrupted -- don't want FIFO to overflow if we're distracted.
    const size_t max_in_flight = 16 - 2;
    while (tx_remaining || rx_remaining) {
        uint32_t flags = ssi_hw->sr;
        bool can_put = !!(flags & SSI_SR_TFNF_BITS);
        bool can_get = !!(flags & SSI_SR_RFNE_BITS);
        if (can_put && tx_remaining && rx_remaining - tx_remaining < max_in_flight) {
            ssi_hw->dr0 = *txbuf++;
            --tx_remaining;
        }
        if (can_get && rx_remaining) {
            *rxbuf++ = (uint8_t)ssi_hw->dr0;
            --rx_remaining;
        }
    }
    flash_cs_force(1);
}

void __no_inline_not_in_flash_func(xflash_do_cmd)(const uint8_t *txbuf, uint8_t *rxbuf, size_t count)
{
    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_flush_cache_fn flash_flush_cache = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash && flash_exit_xip && flash_flush_cache);
    flash_init_boot2_copyout();
    __compiler_memory_barrier();
    connect_internal_flash();
    flash_exit_xip();

    xflash_do_cmd_internal(txbuf, rxbuf, count);

    flash_flush_cache();
    flash_enable_xip_via_boot2();
}

void __no_inline_not_in_flash_func(xflash_set_ea_reg)(uint8_t addr)
{
    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_flush_cache_fn flash_flush_cache = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash && flash_exit_xip && flash_flush_cache);
    flash_init_boot2_copyout();
    __compiler_memory_barrier();
    connect_internal_flash();
    flash_exit_xip();

    uint8_t txbuf[4];
    uint8_t rxbuf[4];

//    txbuf[0] = 0xc8;
//    xflash_do_cmd_internal(txbuf, rxbuf, 2);
//    printf("EA register %02X\n", rxbuf[1]);

    txbuf[0] = 0x06;
    xflash_do_cmd_internal(txbuf, rxbuf, 1);

//    txbuf[0] = 0x05;
//    xflash_do_cmd_internal(txbuf, rxbuf, 2);
//    printf("Status register %02X\n", rxbuf[1]);

    txbuf[0] = 0xc5;
    txbuf[1] = addr;
    xflash_do_cmd_internal(txbuf, rxbuf, 2);

    txbuf[0] = 0x04;
    xflash_do_cmd_internal(txbuf, rxbuf, 1);

//    txbuf[0] = 0xc8;
//    xflash_do_cmd_internal(txbuf, rxbuf, 2);
//    printf("EA register %02X\n", rxbuf[1]);

    flash_flush_cache();
    flash_enable_xip_via_boot2();
}

uint8_t __no_inline_not_in_flash_func(xflash_get_ea_reg)(void)
{
    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_flush_cache_fn flash_flush_cache = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash && flash_exit_xip && flash_flush_cache);
    flash_init_boot2_copyout();
    __compiler_memory_barrier();
    connect_internal_flash();
    flash_exit_xip();

    uint8_t txbuf[2];
    uint8_t rxbuf[2];

    txbuf[0] = 0xc8;
    xflash_do_cmd_internal(txbuf, rxbuf, 2);

    flash_flush_cache();
    flash_enable_xip_via_boot2();

    return rxbuf[1];
}

void __no_inline_not_in_flash_func(xflash_range_erase)(uint32_t flash_offs, size_t count)
{
    invalid_params_if(FLASH, flash_offs & (FLASH_SECTOR_SIZE - 1));
    invalid_params_if(FLASH, count & (FLASH_SECTOR_SIZE - 1));
    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase_fn flash_range_erase = (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_flush_cache_fn flash_flush_cache = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash && flash_exit_xip && flash_range_erase && flash_flush_cache);
    flash_init_boot2_copyout();

    // No flash accesses after this point
    __compiler_memory_barrier();

    connect_internal_flash();
    flash_exit_xip();
    flash_range_erase(flash_offs, count, FLASH_BLOCK_SIZE, FLASH_BLOCK_ERASE_CMD);
    flash_flush_cache(); // Note this is needed to remove CSn IO force as well as cache flushing
    flash_enable_xip_via_boot2();
}

void __no_inline_not_in_flash_func(xflash_range_program)(uint32_t flash_offs, const uint8_t *data, size_t count) {
    invalid_params_if(FLASH, flash_offs & (FLASH_PAGE_SIZE - 1));
    invalid_params_if(FLASH, count & (FLASH_PAGE_SIZE - 1));
    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_program_fn flash_range_program = (rom_flash_range_program_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_PROGRAM);
    rom_flash_flush_cache_fn flash_flush_cache = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash && flash_exit_xip && flash_range_program && flash_flush_cache);
    flash_init_boot2_copyout();

    __compiler_memory_barrier();

    connect_internal_flash();
    flash_exit_xip();
    flash_range_program(flash_offs, data, count);
    flash_flush_cache(); // Note this is needed to remove CSn IO force as well as cache flushing
    flash_enable_xip_via_boot2();
}
