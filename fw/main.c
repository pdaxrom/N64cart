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
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "flashrom.h"
#include "romfs/romfs.h"

#include "main.h"
#include "cic.h"
#include "usb/usbd.h"
#include "n64_pi.h"
#include "n64.h"

static const struct flash_chip flash_chip[] = {
    { 0xef, 0x4020, 4, 16, 266000, "W25Q512" },
    { 0xef, 0x4019, 2, 16, 266000, "W25Q256" },
    { 0xef, 0x4018, 1, 16, 266000, "W25Q128" },
    { 0xef, 0x4017, 1, 8 , 266000, "W25Q64"  },
    { 0xef, 0x4016, 1, 4 , 266000, "W25Q32"  },
    { 0xef, 0x4015, 1, 2 , 266000, "W25Q16"  }
};

static const struct flash_chip *used_flash_chip;

//#define DEBUG_FS 1

bool romfs_flash_sector_erase(uint32_t offset)
{
#ifdef DEBUG_FS
    printf("%s: offset %08X\n", __func__, offset);
#endif
    flash_erase_sector(offset);

    return true;
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
#ifdef DEBUG_FS
    printf("%s: offset %08X\n", __func__, offset);
#endif
    flash_write_sector(offset, buffer);

    return true;
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)
{
#ifdef DEBUG_FS
    printf("%s: offset %08X\n", __func__, offset);
#endif
    flash_read_0C(offset, buffer, need);

    return true;
}

static void flash_ls()
{
    romfs_file file;
    printf("File list:\n");
    if (romfs_list(&file, true) == ROMFS_NOERR) {
	do {
	    printf("%s\t%d\t%0X %4X\n", file.entry.name, file.entry.size, file.entry.attr.names.mode, file.entry.attr.names.type);
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
	    used_flash_chip = &flash_chip[i];
	    break;
	}
    }
}

static void show_sysinfo(void)
{
    printf("ROM chip           : %s\n", used_flash_chip->name);
    printf("System frequency   : %d\n", clock_get_hz(clk_sys) / 1000);
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

int main(void)
{
    for (int i = 0; i <= 27; i++) {
	gpio_init(i);
	gpio_set_dir(i, GPIO_IN);
	gpio_set_pulls(i, false, false);
    }

    gpio_init(N64_CIC_DCLK);
    gpio_init(N64_CIC_DIO);
    gpio_init(N64_SI_CLK);
    gpio_init(N64_SI_DATA);
    gpio_init(N64_COLD_RESET);
    gpio_init(N64_NMI);

    gpio_pull_up(N64_CIC_DIO);
    gpio_pull_up(N64_SI_DATA);

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    gpio_init(N64_INT);
    gpio_pull_up(N64_INT);
    gpio_put(N64_INT, 1);
    gpio_set_dir(N64_INT, GPIO_OUT);

    setup_sysconfig();

    stdio_init_all();
    stdio_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);

    printf("N64cart by pdaXrom!\n");

    if (used_flash_chip == NULL) {
	printf("Unknown ROM chip, system stopped!\n");
	while(true) {
	    tight_loop_contents();
	}
    }

#ifdef DEBUG_INFO
    show_sysinfo();
#endif

    uintptr_t fw_binary_end = (uintptr_t) &__flash_binary_end;

    flash_quad_exit_cont_read_mode();
    flash_spi_mode();

    uint32_t flash_map_size, flash_list_size;

    romfs_get_buffers_sizes(used_flash_chip->rom_pages * used_flash_chip->rom_size * 1024 * 1024, &flash_map_size, &flash_list_size);

    uint16_t *romfs_flash_map = (uint16_t *) pi_sram;
    uint8_t *romfs_flash_list = &pi_sram[flash_map_size];
    uint8_t *romfs_flash_buffer = &pi_sram[flash_map_size + flash_list_size];

    if (!romfs_start(((fw_binary_end - XIP_BASE) + 4095) & ~4095, used_flash_chip->rom_pages * used_flash_chip->rom_size * 1024 * 1024, romfs_flash_map, romfs_flash_list)) {
	printf("Cannot start romfs!\n");
	while(true) {
	    tight_loop_contents();
	}
    }

    romfs_file file;
    if (romfs_open_file("n64cart-manager.z64", &file, romfs_flash_buffer) == ROMFS_NOERR) {
	romfs_read_map_table(pi_rom_lookup, 16384, &file);

	backup_rom_lookup();
    } else {
	printf("romfs error: %s\n", romfs_strerror(file.err));
	usbd_main();
	while(true) {
	    tight_loop_contents();
	}
    }

    flash_quad_cont_read_mode();

    multicore_launch_core1(n64_pi);

    usbd_main();
    cic_main();

    return 0;
}
