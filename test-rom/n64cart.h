/**
 * Copyright (c) 2022 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __N64CART_H__
#define __N64CART_H__

#define N64CART_UART_CTRL	0x1fd01000
#define N64CART_UART_RXTX	0x1fd01004
#define N64CART_UART_RX_AVAIL	0x01
#define N64CART_UART_TX_FREE	0x02

#define N64CART_LED_CTRL	0x1fd01008
#define N64CART_ROM_PAGE_CTRL	0x1fd0100c
#define N64CART_PICTURE_ROM	0x1fd80000

uint8_t n64cart_uart_getc(void);
void n64cart_uart_putc(uint8_t data);
void n64cart_uart_puts(char *str);
uint16_t n64cart_get_rom_pages(void);
void n64cart_set_rom_page(uint16_t page);

#endif
