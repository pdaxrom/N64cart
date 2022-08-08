/**
 * Copyright (c) 2022 Konrad Beckmann
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/multicore.h"
#include "hardware/flash.h"

#include "n64_pi.pio.h"

#include "cic.h"
#include "n64.h"

// The rom to load in normal .z64, big endian, format
#include "rom.h"
static uint16_t *rom_file_16 = (uint16_t *) rom_file;

#define UART_TX_PIN (28)
#define UART_RX_PIN (29)	/* not available on the pico */
#define UART_ID     uart0
#define BAUD_RATE   115200

static inline uint32_t swap16(uint32_t value)
{
    // 0x11223344 => 0x33441122
    return (value << 16) | (value >> 16);
}

static inline uint32_t swap8(uint16_t value)
{
    // 0x1122 => 0x2211
    return (value << 8) | (value >> 8);
}

/*

Profiling results:

Time between ~N64_READ and bit output on AD0

With constant data fetched from C-code (no memory access)
--------------------------------------
133 MHz: 240 ns
150 MHz: 230 ns
200 MHz: 230 ns
250 MHz: 190 ns

With uncached data from external flash
--------------------------------------
133 MHz: 780 ns
150 MHz: 640 ns
200 MHz: 480 ns
250 MHz: 390 ns


*/

volatile uint32_t rom_pages;
volatile uint32_t rom_start[4];
volatile uint32_t rom_size[4];

static const struct flash_chip {
    uint8_t mf;
    uint16_t id;
    uint8_t rom_pages;
    uint8_t rom_size;
} flash_chip[] = {
    { 0xef, 0x4019, 2, 16 },
    { 0xef, 0x4018, 1, 16 },
    { 0xef, 0x4017, 1, 8  },
    { 0xef, 0x4016, 1, 4  },
    { 0xef, 0x4015, 1, 2  }
};

extern char __flash_binary_end;

static void setup_rom_storage(void)
{
    uint8_t txbuf[4];
    uint8_t rxbuf[4];
    uintptr_t fw_binary_end = (uintptr_t) &__flash_binary_end;

    txbuf[0] = 0x9f;

    flash_do_cmd(txbuf, rxbuf, 4);

    printf("Flash jedec id %02X %02X %02X\n", rxbuf[1], rxbuf[2], rxbuf[3]);

    uint8_t mf = rxbuf[1];
    uint16_t id = (rxbuf[2] << 8) | rxbuf[3];

    rom_pages = 1;
    rom_start[0] = ((fw_binary_end - 0x10000000) + 4096) & ~4095;
    rom_size[0] = 2 * 1024 * 1024;

    for (int i = 0; i < sizeof(flash_chip) / sizeof(struct flash_chip); i++) {
	if (flash_chip[i].mf == mf && flash_chip[i].id == id) {
	    rom_pages = flash_chip[i].rom_pages;
	    rom_size[0] = flash_chip[i].rom_size * 1024 * 1024;

	    for (int p = 1; p < flash_chip[i].rom_pages; p++) {
		rom_start[p] = 0;
		rom_size[p] = flash_chip[i].rom_size * 1024 * 1024;
	    }

	    break;
	}
    }

    printf("Available ROM pages:\n");

    for (int i = 0; i < rom_pages; i++) {
	printf("Page %d\n", i);
	printf(" Address %08X\n", rom_start[i]);
	printf(" Size    %d bytes\n", rom_size[i]);
    }
}

int main(void)
{
    // Overclock!
    // Note that the Pico's external flash is rated to 133MHz,
    // not sure if the flash speed is based on this clock.

    // set_sys_clock_khz(PLL_SYS_KHZ, true);
    // set_sys_clock_khz(150000, true); // Does not work
    // set_sys_clock_khz(200000, true); // Does not work
    // set_sys_clock_khz(250000, true); // Does not work
    // set_sys_clock_khz(300000, true); // Doesn't even boot
    // set_sys_clock_khz(400000, true); // Doesn't even boot

//    set_sys_clock_khz(200000, true);
//    set_sys_clock_khz(250000, true);
    set_sys_clock_khz(256000, true);
//    set_sys_clock_khz(280000, true);

    stdio_init_all();

    for (int i = 0; i <= 27; i++) {
	gpio_init(i);
	gpio_set_dir(i, GPIO_IN);
	gpio_set_pulls(i, false, false);
    }

    gpio_init(N64_CIC_DCLK);
    gpio_init(N64_CIC_DIO);
    gpio_init(N64_COLD_RESET);
    gpio_init(N64_NMI);

    gpio_pull_up(N64_CIC_DIO);

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // Init UART on pin 28/29
    stdio_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);
    printf("N64 cartridge booting!\r\n");

    setup_rom_storage();

    // Init PIO before starting the second core
    PIO pio = pio0;
    uint offset = pio_add_program(pio, &pi_program);
    pi_program_init(pio, 0, offset);
    pio_sm_set_enabled(pio, 0, true);

    // Launch the CIC emulator in the second core
    // Note! You have to power reset the pico after flashing it with a jlink,
    //       otherwise multicore doesn't work properly.
    //       Alternatively, attach gdb to openocd, run `mon reset halt`, `c`.
    //       It seems this works around the issue as well.
    multicore_launch_core1(cic_main);

    // Wait for reset to be released
    while (gpio_get(N64_COLD_RESET) == 0) {
	tight_loop_contents();
    }

    uint32_t last_addr = 0;
    uint32_t word;

    uint32_t addr = pio_sm_get_blocking(pio, 0);
    do {
	if (addr == 0) {
	    //READ
#if 1
	    if (last_addr == 0x10000000) {
		// Configure bus to run slowly.
		// This is better patched in the rom, so we won't need a branch here.
		// But let's keep it here so it's easy to import roms easily.
		// 0x8037FF40 in big-endian
		//word = 0x40FF3780;
		//word = 0x40203780;
		////word = 0x40123780;
		//word = 0x401c3780;
		word = 0x3780;
		pio_sm_put(pio, 0, swap8(word));
		last_addr += 2;

		//word = 0x40FF;
		word = 0x401c;
		//word = 0x4012;
		addr = pio_sm_get_blocking(pio, 0);
		if (addr == 0) {
		    goto hackentry;
		}

		continue;
	    } else
#endif
	    if (last_addr >= 0x10000000 && last_addr <= 0x1FBFFFFF) {
		do {
		    word = rom_file_16[(last_addr & 0xFFFFFF) >> 1];
 hackentry:
		    pio_sm_put(pio, 0, swap8(word));
		    last_addr += 2;
		    addr = pio_sm_get_blocking(pio, 0);
		} while (addr == 0);

		continue;
	    } else if (last_addr == 0x1fd01002) {
		word =
		    ((uart_get_hw(UART_ID)->fr & UART_UARTFR_TXFF_BITS) ? 0x00 : 0x0200) |
		    ((uart_get_hw(UART_ID)->fr & UART_UARTFR_RXFE_BITS) ? 0x00 : 0x0100) | 0xf000;
	    } else if (last_addr == 0x1fd01006) {
		word = uart_get_hw(UART_ID)->dr << 8;
	    } else if (last_addr == 0x1fd0100c) {
		word = rom_pages << 8;
	    } else {
		word = 0xdead;
	    }
	    pio_sm_put(pio, 0, swap8(word));
	    last_addr += 2;
	} else if (addr & 0x1) {
	    // WRITE
	    if (last_addr == 0x1fd01006) {
		uart_get_hw(UART_ID)->dr = (addr >> 16) & 0xff;
	    } else if (last_addr == 0x1fd0100a) {
		gpio_put(LED_PIN, (addr >> 16) & 0x01);
	    } else if (last_addr == 0x1fd0100e) {
		int page = (addr >> 16);
		if (page < rom_pages) {
		    rom_file_16 = (uint16_t *) (0x10000000 + rom_start[page]);
		}
	    }

	    last_addr += 2;
	} else {
	    last_addr = addr;
	}
	addr = pio_sm_get_blocking(pio, 0);
    } while (1);

    return 0;
}
