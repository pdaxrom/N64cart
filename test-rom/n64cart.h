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

#define N64CART_FLASH_CTRL	0x1fd0100c
#define N64CART_SSI_SR		0x1fd01010
#define N64CART_SSI_DR0		0x1fd01014
#define N64CART_FLASH_CTRL_MODE_MASK	0x10
#define N64CART_FLASH_CTRL_CS_MASK	0x01
#define N64CART_FLASH_CTRL_MODE_QUAD	0x10
#define N64CART_FLASH_CTRL_MODE_SPI	0x00
#define N64CART_FLASH_CTRL_CS_HIGH	0x01
#define N64CART_FLASH_CTRL_CS_LOW	0x00
#define N64CART_SSI_SR_TFNF_BITS	0x01
#define N64CART_SSI_SR_RFNE_BITS	0x02

#define N64CART_FW_SIZE		0x1fd01018

uint8_t n64cart_uart_getc(void);
void n64cart_uart_putc(uint8_t data);
void n64cart_uart_puts(char *str);

void flash_cs_force(bool high);
void flash_do_cmd(const uint8_t *txbuf, uint8_t *rxbuf, size_t count, size_t rxskip);
void flash_mode(bool mode);
bool flash_erase_sector(uint32_t addr);
bool flash_write_sector(uint32_t addr, uint8_t *buffer);
bool flash_read_0C(uint32_t addr, uint8_t *buffer, uint32_t len);
uint8_t flash_read8_0C(uint32_t addr);
uint16_t flash_read16_0C(uint32_t addr);
uint32_t flash_read32_0C(uint32_t addr);

uint32_t n64cart_fw_size(void);

#endif
