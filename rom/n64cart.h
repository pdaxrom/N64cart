/**
 * Copyright (c) 2022-2024 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __N64CART_H__
#define __N64CART_H__

#include <libdragon.h>

#define N64CART_UART_CTRL	0x1fd01000
#define N64CART_UART_RXTX	0x1fd01004
#define N64CART_UART_RX_AVAIL	0x01
#define N64CART_UART_TX_FREE	0x02

#define N64CART_LED_CTRL	0x1fd01008

#define N64CART_SYS_CTRL	0x1fd0100c
#define N64CART_SSI_SR		0x1fd01010
#define N64CART_SSI_DR0		0x1fd01014

#define N64CART_EEPROM_16KBIT	0x1000
#define N64CART_SRAM_UNLOCK	0x100
#define N64CART_FLASH_MODE_QUAD	0x10
#define N64CART_FLASH_CS_HIGH	0x01

#define N64CART_SSI_SR_TFNF_BITS	0x01
#define N64CART_SSI_SR_RFNE_BITS	0x02

#define N64CART_FW_SIZE		0x1fd01018

#define N64CART_USBCFG		0x1fd01020

#define N64CART_USB_IRQ		0x8000
#define N64CART_USB_BSWAP32 0x0080
#define N64CART_USB_IRQ_ENABLE	0x0010
#define N64CART_USB_RESET	0x0001

#define N64CART_USBCTRL_BASE	0x1fe00000
#define N64CART_USBCTRL_REGS_BASE 0x1fe10000

#define N64CART_SRAM		0x08000000
#define N64CART_ROM_LOOKUP	0x08020000
#define N64CART_EEPROM		(0x08020000 + 4096 * 4 * 2 * 2)
#define N64CART_RMRAM		(0x08020000 + 4096 * 4 * 2 * 2 + 2048)

inline void pi_io_write(uint32_t pi_address, uint32_t val)
{
    volatile uint32_t *uncached_address = (uint32_t *) (pi_address | 0xa0000000);

    dma_wait();
    MEMORY_BARRIER();
    *uncached_address = val;
    MEMORY_BARRIER();
}

inline uint32_t pi_io_read(uint32_t pi_address)
{
    volatile uint32_t *uncached_address = (uint32_t *) (pi_address | 0xa0000000);

    dma_wait();
    MEMORY_BARRIER();
    return *uncached_address;
}

uint8_t n64cart_uart_getc(void);
void n64cart_uart_putc(uint8_t data);
void n64cart_uart_puts(char *str);

//void flash_cs_force(bool high);
void flash_do_cmd(uint8_t cmd, const uint8_t * txbuf, uint8_t * rxbuf, size_t count);
void flash_mode(bool mode);
bool flash_erase_sector(uint32_t addr);
bool flash_write_sector(uint32_t addr, uint8_t * buffer);
bool flash_read_0C(uint32_t addr, uint8_t * buffer, uint32_t len);
uint8_t flash_read8_0C(uint32_t addr);
uint16_t flash_read16_0C(uint32_t addr);
uint32_t flash_read32_0C(uint32_t addr);

uint32_t n64cart_fw_size(void);

void n64cart_sram_lock(void);
void n64cart_sram_unlock(void);

void n64cart_eeprom_16kbit(bool enable);
bool n64cart_is_eeprom_16kbit(void);

#endif
