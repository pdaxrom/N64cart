/**
 * Copyright (c) 2022-2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/multicore.h"
#include "hardware/structs/ssi.h"

#include "hardware/flash.h"
#include "flashrom.h"
#include "main.h"
#include "romfs/romfs.h"
#include "n64_pi.pio.h"
#include "n64.h"

static uint16_t pi_bus_freq = 0x40ff;
static uint16_t flash_ctrl_reg = 0x11;

uint8_t pi_sram[SRAM_1MBIT_SIZE + ROMFS_FLASH_SECTOR * 4 * 2 * 2];
uint16_t *pi_rom_lookup = (uint16_t *) &pi_sram[SRAM_1MBIT_SIZE];

static uint16_t *sram_16 = (uint16_t *) pi_sram;
static const uint16_t *rom_lookup = (uint16_t *) &pi_sram[SRAM_1MBIT_SIZE];

void backup_rom_lookup(void)
{
    memmove(&pi_rom_lookup[ROMFS_FLASH_SECTOR * 4], pi_rom_lookup, ROMFS_FLASH_SECTOR * 4 * 2);
}

void restore_rom_lookup(void)
{
    memmove(pi_rom_lookup, &pi_rom_lookup[ROMFS_FLASH_SECTOR * 4], ROMFS_FLASH_SECTOR * 4 * 2);
    __dmb();
}

static inline uint32_t resolve_sram_address(uint32_t address)
{
    if (flash_ctrl_reg & 0x100) {
	return address & 0x3ffff;
    }

    uint32_t bank = (address >> 18) & 0x3;
    uint32_t resolved_address;

    if (bank) {
        resolved_address = address & (SRAM_256KBIT_SIZE - 1);
        resolved_address |= bank << 15;
    } else {
        resolved_address = address & (SRAM_1MBIT_SIZE - 1);
    }

    return resolved_address;
}

void set_pi_bus_freq(uint16_t freq)
{
    if ((freq >> 8) < 0x12) {
	freq = 0xFF40;
    }

    pi_bus_freq = freq;
}

uint16_t get_pi_bus_freq(void)
{
    return pi_bus_freq;
}

void n64_pi(void)
{
    PIO pio = pio0;
    pio_clear_instruction_memory(pio);
    uint offset = pio_add_program(pio, &pi_program);
    pi_program_init(pio, 0, offset);
    pio_sm_set_enabled(pio, 0, true);

    gpio_put(LED_PIN, 0);

    // Wait for reset to be released
    while (gpio_get(N64_COLD_RESET) == 0) {
	tight_loop_contents();
    }

    uint32_t last_addr = 0;
    uint32_t word;

    uint32_t mapped_addr;

    uint32_t addr = pio_sm_get_blocking(pio, 0);
    do {
	if (addr == 0) {
	    //READ
	    if (last_addr == 0x10000000) {
		word = 0x8037;
		pio_sm_put(pio, 0, word);
		last_addr += 2;
#if PI_SRAM
//		word = pi_bus_freq;
		word = 0xFF40;
#else
//		word = pi_bus_freq;
		word = 0xFF40;
#endif
		addr = pio_sm_get_blocking(pio, 0);
		if (addr == 0) {
		    goto hackentry;
		}

		continue;
	    } else if (last_addr >= 0x10000000 && last_addr <= 0x1FBFFFFF) {
		do {
		    mapped_addr = (rom_lookup[(last_addr & 0x3ffffff) >> 12]) << 12 | (last_addr & 0xfff);
		    word = flash_quad_read16_EC(mapped_addr);
 hackentry:
		    pio_sm_put(pio, 0, word);
		    last_addr += 2;
		    addr = pio_sm_get_blocking(pio, 0);
		} while (addr == 0);

		continue;
#if PI_SRAM
	    } else if (last_addr >= 0x08000000 && last_addr <= 0x0FFFFFFF) {
		do {
		    word = sram_16[resolve_sram_address(last_addr) >> 1];

		    pio_sm_put(pio, 0, word);
		    last_addr += 2;
		    addr = pio_sm_get_blocking(pio, 0);
		} while (addr == 0);

		continue;
#endif
	    } else if (last_addr == 0x1fd01002) {
		word =
		    ((uart_get_hw(UART_ID)->fr & UART_UARTFR_TXFF_BITS) ? 0x00 : 0x02) |
		    ((uart_get_hw(UART_ID)->fr & UART_UARTFR_RXFE_BITS) ? 0x00 : 0x01) | 0x00f0;
	    } else if (last_addr == 0x1fd01006) {
		word = uart_get_hw(UART_ID)->dr;
	    } else if (last_addr == 0x1fd0100e) {
		word = flash_ctrl_reg;
	    } else if (last_addr == 0x1fd01012) {
		uint32_t flags = ssi_hw->sr;
		word = ((flags & SSI_SR_TFNF_BITS) ? 0x01 : 0x00) | ((flags & SSI_SR_RFNE_BITS) ? 0x02 : 0x00);
	    } else if (last_addr == 0x1fd01016) {
		uint8_t byte = (uint8_t)ssi_hw->dr0;
		word = byte;
	    } else if (last_addr == 0x1fd0101a) {
		uintptr_t fw_binary_end = (uintptr_t) &__flash_binary_end;
		word = (((fw_binary_end - XIP_BASE) + 4095) & ~4095) >> 12;
	    } else {
		word = 0xdead;
	    }
	    pio_sm_put(pio, 0, word);
	    last_addr += 2;
	} else if (addr & 0x1) {
	    // WRITE
#if PI_SRAM
	    if (last_addr >= 0x08000000 && last_addr <= 0x0FFFFFFF) {
		do {
		    sram_16[resolve_sram_address(last_addr) >> 1] = addr >> 16;

		    last_addr += 2;
		    addr = pio_sm_get_blocking(pio, 0);
		} while (addr & 0x01);

		continue;
	    } else 
#endif
	    if (last_addr == 0x1fd01006) {
		uart_get_hw(UART_ID)->dr = (addr >> 16) & 0xff;
	    } else if (last_addr == 0x1fd0100a) {
		gpio_put(LED_PIN, (addr >> 16) & 0x01);
	    } else if (last_addr == 0x1fd0100e) {
		uint16_t ctrl_reg = addr >> 16;
		if (ctrl_reg & 0x10) {
		    flash_quad_mode();
		} else {
		    if (flash_ctrl_reg & 0x10) {
			flash_spi_mode();
		    }
		    flash_cs_force(ctrl_reg & 0x01);
		}
		flash_ctrl_reg = ctrl_reg;
	    } else if (last_addr == 0x1fd01016) {
		uint8_t byte = (addr >> 16) & 0xff;
		ssi_hw->dr0 = byte;
	    }

	    last_addr += 2;
	} else {
	    last_addr = addr;
	}
	addr = pio_sm_get_blocking(pio, 0);
    } while (1);
}
