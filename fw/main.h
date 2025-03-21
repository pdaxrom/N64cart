/**
 * Copyright (c) 2022-2024 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <stdint.h>

#define PI_SRAM 1
#define PI_USBCTRL  1

#define UART_ID     uart0

#define SRAM_256KBIT_SIZE         0x00008000
#define SRAM_768KBIT_SIZE         0x00018000
#define SRAM_1MBIT_SIZE           0x00020000

struct flash_chip {
    uint8_t mf;
    uint16_t id;
    uint8_t rom_size;
    uint32_t sys_freq;
    uint8_t voltage;
    const char *name;
};

extern uint16_t sys64_ctrl_reg;
extern uint16_t usb64_ctrl_reg;

extern uint8_t pi_sram[];
extern uint16_t *pi_rom_lookup;
extern uint8_t *si_eeprom;

extern char __flash_binary_end;

const struct flash_chip *get_flash_info(void);

void n64_pi_restart(void);
