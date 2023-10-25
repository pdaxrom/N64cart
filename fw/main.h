/**
 * Copyright (c) 2022-2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MAIN_H__
#define __MAIN_H__

#define PI_SRAM 1

#define UART_TX_PIN (28)
#define UART_RX_PIN (29)	/* not available on the pico */
#define UART_ID     uart0
#define BAUD_RATE   115200

#define SRAM_256KBIT_SIZE         0x00008000
#define SRAM_768KBIT_SIZE         0x00018000
#define SRAM_1MBIT_SIZE           0x00020000

struct flash_chip {
    uint8_t mf;
    uint16_t id;
    uint8_t rom_pages;
    uint8_t rom_size;
    uint32_t sys_freq;
    uint16_t pi_bus_freq;
    const char *name;
};

extern uint16_t rom_lookup[16386];

extern char __flash_binary_end;

const struct flash_chip *get_flash_info(void);

void n64_pi_restart(void);

#if PI_SRAM
extern uint8_t sram_8[];

void n64_save_sram(void);
#endif

#endif
