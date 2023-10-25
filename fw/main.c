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
#include "romfs/romfs.h"

#include "main.h"
#include "cic.h"
#include "n64_pi.h"
#include "n64.h"

static const struct flash_chip flash_chip[] = {
    { 0xef, 0x4020, 4, 16, 220000, 0x4028, "W25Q512" },
//    { 0xef, 0x4020, 4, 16, 200000, 0x4028, "W25Q512" },
//    { 0xef, 0x4019, 2, 16, 256000, 0x4022, "W25Q256" },
    { 0xef, 0x4019, 2, 16, 230000, 0x4022, "W25Q256" },
    { 0xef, 0x4018, 1, 16, 256000, 0x4022, "W25Q128" },
    { 0xef, 0x4017, 1, 8 , 256000, 0x4022, "W25Q64"  },
    { 0xef, 0x4016, 1, 4 , 256000, 0x4022, "W25Q32"  },
    { 0xef, 0x4015, 1, 2 , 256000, 0x4022, "W25Q16"  }
};

static const struct flash_chip *used_flash_chip;

//#define DEBUG_FS 1

bool romfs_flash_sector_erase(uint32_t offset)
{
#ifdef DEBUG_FS
    printf("%s: offset %08X\n", __func__, offset);
#endif
    flash_spi_mode();
    flash_erase_sector(offset);
    flash_quad_mode(true);

    return true;
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
#ifdef DEBUG_FS
    printf("%s: offset %08X\n", __func__, offset);
#endif
    flash_spi_mode();
    flash_write_sector(offset, buffer);
    flash_quad_mode(true);

    return true;
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)
{
#ifdef DEBUG_FS
    printf("%s: offset %08X\n", __func__, offset);
#endif
    flash_spi_mode();

    for (int i = 0; i < need; i++) {
	buffer[i] = flash_read8_0C(offset + i);
    }

    flash_quad_mode(true);

    return true;
}

static void flash_ls()
{
    romfs_file file;
    printf("File list:\n");
    if (romfs_list(&file, true) == ROMFS_NOERR) {
	do {
	    printf("%s\t%d\t%0X %4X\n", file.entry.name, file.entry.size, file.entry.mode, file.entry.type);
	} while (romfs_list(&file, false) == ROMFS_NOERR);
    }
}

static void setup_sysconfig(void)
{
    uint8_t txbuf[4];
    uint8_t rxbuf[4];

    txbuf[0] = 0x9f;

//    printf("Detect ROM chip\n");

    flash_do_cmd(txbuf, rxbuf, 4);

//    printf("Flash jedec id %02X %02X %02X\n", rxbuf[1], rxbuf[2], rxbuf[3]);

    uint8_t mf = rxbuf[1];
    uint16_t id = (rxbuf[2] << 8) | rxbuf[3];

    used_flash_chip = NULL;
    for (int i = 0; i < sizeof(flash_chip) / sizeof(struct flash_chip); i++) {
	if (flash_chip[i].mf == mf && flash_chip[i].id == id) {
	    set_sys_clock_khz(flash_chip[i].sys_freq, true);
	    set_pi_bus_freq(flash_chip[i].pi_bus_freq);
	    used_flash_chip = &flash_chip[i];
	    break;
	}
    }
}

static void show_sysinfo(void)
{
    if (used_flash_chip == NULL) {
	printf("Unknown ROM chip, system stopped!\n");
	while(1) {}
    }

    printf("ROM chip           : %s\n", used_flash_chip->name);
    printf("System frequency   : %d\n", clock_get_hz(clk_sys) / 1000);
    printf("PI bus freq config : %04X\n\n", get_pi_bus_freq());
    printf("ROM size           : %d MB\n", used_flash_chip->rom_pages * used_flash_chip->rom_size);
}

const struct flash_chip *get_flash_info(void)
{
    return used_flash_chip;
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

    gpio_init(N64_INT);
    gpio_pull_up(N64_INT);
    gpio_put(N64_INT, 1);
    gpio_set_dir(N64_INT, GPIO_OUT);

    setup_sysconfig();

    stdio_init_all();
    stdio_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);

    printf("N64cart booting!\n\n");
    show_sysinfo();

    uintptr_t fw_binary_end = (uintptr_t) &__flash_binary_end;

    flash_spi_mode();

//    for (uint32_t i = 0; i < 16; i++) {
//	uint32_t addr = 0x00000000 + (i << 1);
//	printf("%08X: %04X\n", addr, flash_read16_0C(addr));
//    }
//    printf("%08X: %02X\n", 0, flash_read8_0C(0));
//    printf("%08X: %02X\n", 0, flash_read8_0C(1));
//    printf("%08X: %02X\n", 0, flash_read8_0C(2));
//    printf("%08X: %02X\n", 0, flash_read8_0C(3));
//    printf("%08X: %08X\n", 0, flash_read32_0C(0));
//printf(">>>>%08X\n", ((fw_binary_end - XIP_BASE) + 4095) & ~4095);

    if (!romfs_start(((fw_binary_end - XIP_BASE) + 4095) & ~4095, used_flash_chip->rom_pages * used_flash_chip->rom_size * 1024 * 1024)) {
	printf("Cannot start romfs!\n");
	while(true) ;
    }

//    if (strncmp((const char *)(XIP_BASE + (((fw_binary_end - XIP_BASE) + 4095) & ~4095)), "firmware", 8)) {
//	printf("no signature found, format\n");
//	romfs_format();
//    }

    flash_ls();

    romfs_file file;
    if (romfs_open_file("test-rom.z64", &file, NULL) == ROMFS_NOERR) {
	romfs_read_map_table(rom_lookup, 16384, &file);
    } else {
	printf("romfs error: %s\n", romfs_strerror(file.err));
    }

//    printf("flash quad mode\n");

    flash_quad_mode(true);

#if 0
    for (uint32_t i = 0; i < 16; i++) {
//	uint32_t addr = 0x00000000 + (i << 1);
//	printf("%08X: %04X\n", addr, flash_quad_read16_EB(addr));
//	uint32_t addr = 0x00000000 + (i << 2);
//	printf("%08X: %08X\n", addr, flash_quad_read32_EB(addr));
	uint32_t addr = 0x00000000 + (i << 1);
	printf("%08X: %04X\n", addr, flash_quad_read16_EC(addr));
//	uint32_t addr = 0x00000000 + (i << 2);
//	printf("%08X: %08X\n", addr, flash_quad_read32_EC(addr));
    }

    {
	uint32_t last_addr = 0x6578;
	uint32_t mapped_addr = (rom_lookup[(last_addr & 0x3ffffff) >> 12]) << 12 | (last_addr & 0xfff);

	printf("%08X: %08X\n", last_addr, flash_quad_read32_EC(mapped_addr));
    }
#endif

#ifdef PI_SRAM
    memcpy(sram_8, n64_sram, SRAM_1MBIT_SIZE);
#endif

    multicore_launch_core1(n64_pi);

    cic_main();

    return 0;
}
