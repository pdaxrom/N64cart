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
#include "flashrom.h"

#include "main.h"
#include "cic.h"
#include "n64_pi.h"
#include "n64.h"

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

volatile uint32_t jpeg_start;

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

    printf("Detect ROM\n");

    flash_do_cmd(txbuf, rxbuf, 4);

//    printf("Flash jedec id %02X %02X %02X\n", rxbuf[1], rxbuf[2], rxbuf[3]);

    uint8_t mf = rxbuf[1];
    uint16_t id = (rxbuf[2] << 8) | rxbuf[3];

    jpeg_start = ((fw_binary_end - XIP_BASE) + 4095) & ~4095;

    rom_pages = 1;
    rom_start[0] = jpeg_start + 64 * 1024;
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
	printf(" Size    %d bytes\n", rom_size[i] - rom_start[i]);
    }
}

void n64_pi_restart(void)
{
    multicore_reset_core1();
    multicore_launch_core1(n64_pi);
}

#if PI_SRAM
const uint8_t __aligned(4096) __in_flash("n64_sram") n64_sram[SRAM_1MBIT_SIZE];

uint8_t sram_8[SRAM_1MBIT_SIZE];

void n64_save_sram(void)
{
    uint32_t offset = ((uint32_t) n64_sram) - XIP_BASE;
    uint32_t count = sizeof(n64_sram);

    flash_range_erase(offset, count);
    flash_range_program(offset, sram_8, count);
}
#endif

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

    memcpy(sram_8, n64_sram, SRAM_1MBIT_SIZE);

    multicore_launch_core1(n64_pi);

    cic_main();

    return 0;
}
