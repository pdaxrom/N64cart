/**
 * Copyright (c) 2022 Konrad Beckmann
 *
 * Copyright (c) 2022 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "flashrom.h"

#include "main.h"
#include "cic.h"
#include "n64_pi.h"
#include "n64.h"

volatile uint32_t jpeg_start;

volatile uint32_t rom_pages;
volatile uint32_t rom_start[4];
volatile uint32_t rom_size[4];

static const char *rom_chip_name = NULL;

static const struct flash_chip {
    uint8_t mf;
    uint16_t id;
    uint8_t rom_pages;
    uint8_t rom_size;
    uint32_t sys_freq;
    uint16_t pi_bus_freq;
    const char *name;
} flash_chip[] = {
    { 0xef, 0x4020, 4, 16, 220000, 0x4028, "W25Q512" },
    { 0xef, 0x4019, 2, 16, 256000, 0x4022, "W25Q256" },
    { 0xef, 0x4018, 1, 16, 256000, 0x4022, "W25Q128" },
    { 0xef, 0x4017, 1, 8 , 256000, 0x4022, "W25Q64"  },
    { 0xef, 0x4016, 1, 4 , 256000, 0x4022, "W25Q32"  },
    { 0xef, 0x4015, 1, 2 , 256000, 0x4022, "W25Q16"  }
};

extern char __flash_binary_end;

static void setup_sysconfig(void)
{
    uint8_t txbuf[4];
    uint8_t rxbuf[4];
    uintptr_t fw_binary_end = (uintptr_t) &__flash_binary_end;

    txbuf[0] = 0x9f;

//    printf("Detect ROM chip\n");

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

	    set_sys_clock_khz(flash_chip[i].sys_freq, true);
	    set_pi_bus_freq(flash_chip[i].pi_bus_freq);
	    rom_chip_name = flash_chip[i].name;
	    break;
	}
    }
}

static void show_sysinfo(void)
{
    if (rom_chip_name == NULL) {
	printf("Unknown ROM chip, system stopped!\n");
	while(1) {}
    }

    printf("ROM chip           : %s\n", rom_chip_name);
    printf("System frequency   : %d\n", clock_get_hz(clk_sys) / 1000);
    printf("PI bus freq config : %04X\n\n", get_pi_bus_freq());
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

    setup_sysconfig();

    stdio_init_all();
    stdio_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);

    printf("N64cart booting!\n\n");
    show_sysinfo();

    memcpy(sram_8, n64_sram, SRAM_1MBIT_SIZE);

    multicore_launch_core1(n64_pi);

    cic_main();

    return 0;
}
