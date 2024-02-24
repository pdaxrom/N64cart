/**
 * Copyright (c) 2022-2024 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <libdragon.h>
#include "n64cart.h"

uint8_t n64cart_uart_getc(void)
{
    while (!(io_read(N64CART_UART_CTRL) & N64CART_UART_RX_AVAIL)) ;

    return io_read(N64CART_UART_RXTX) & 0xff;
}

void n64cart_uart_putc(uint8_t data)
{
    while (!(io_read(N64CART_UART_CTRL) & N64CART_UART_TX_FREE));

    io_write(N64CART_UART_RXTX, data);
}

void n64cart_uart_puts(char *str)
{
    while (*str) {
	char c = *str++;
	n64cart_uart_putc(c);
	if (c == '\n') {
	    n64cart_uart_putc('\r');
	}
    }
}

static void flash_cs_force(bool high)
{
    uint32_t ctrl = io_read(N64CART_SYS_CTRL);
    if (high) {
	ctrl |= N64CART_FLASH_CS_HIGH;
    } else {
	ctrl &= ~N64CART_FLASH_CS_HIGH;
    }
    io_write(N64CART_SYS_CTRL, ctrl);
}

static void xflash_do_cmd_internal(const uint8_t *txbuf, uint8_t *rxbuf, size_t count, size_t rxskip)
{
    flash_cs_force(0);
    size_t tx_remaining = count;
    size_t rx_remaining = count;
    // We may be interrupted -- don't want FIFO to overflow if we're distracted.
    const size_t max_in_flight = 16 - 2;
    while (tx_remaining || rx_remaining || rxskip) {
        uint32_t flags = io_read(N64CART_SSI_SR);
        bool can_put = !!(flags & N64CART_SSI_SR_TFNF_BITS);
        bool can_get = !!(flags & N64CART_SSI_SR_RFNE_BITS);
        if (can_put && tx_remaining && rx_remaining - tx_remaining < max_in_flight) {
            if (txbuf) {
		io_write(N64CART_SSI_DR0, *txbuf++);
            } else {
		io_write(N64CART_SSI_DR0, 0);
            }
            --tx_remaining;
        }
        if (can_get && (rx_remaining || rxskip)) {
            uint8_t rxbyte = io_read(N64CART_SSI_DR0) & 0xff;
            if (rxskip) {
                --rxskip;
            } else {
                if (rxbuf) {
                    *rxbuf++ = rxbyte;
                }
                --rx_remaining;
            }
        }
    }
    flash_cs_force(1);
}

void flash_do_cmd(const uint8_t *txbuf, uint8_t *rxbuf, size_t count, size_t rxskip)
{
    xflash_do_cmd_internal(txbuf, rxbuf, count, rxskip);
}

void flash_mode(bool mode)
{
    uint32_t ctrl = io_read(N64CART_SYS_CTRL);
    if (mode) {
	ctrl |= N64CART_FLASH_MODE_QUAD;
    } else {
	ctrl &= ~N64CART_FLASH_MODE_QUAD;
    }
    io_write(N64CART_SYS_CTRL, ctrl);
}

static void xflash_wait_ready(void)
{
    uint8_t txbuf[2];
    uint8_t rxbuf[2];
    do {
	txbuf[0] = 0x05;
	xflash_do_cmd_internal(txbuf, rxbuf, 2, 0);
    } while (rxbuf[1] & 0x1);
}

static inline void xflash_put_cmd_addr32(uint8_t cmd, uint32_t addr) {
    flash_cs_force(0);
    io_write(N64CART_SSI_DR0, cmd);
    for (int i = 0; i < 4; ++i) {
        io_write(N64CART_SSI_DR0, addr >> 24);
        addr <<= 8;
    }
}

bool flash_erase_sector(uint32_t addr)
{
    uint8_t txbuf[5];
    uint8_t rxbuf[5];

    txbuf[0] = 0x06;
    xflash_do_cmd_internal(txbuf, rxbuf, 1, 0);
    xflash_put_cmd_addr32(0x21, addr);
    xflash_do_cmd_internal(txbuf, rxbuf, 0, 5);
    xflash_wait_ready();

    return true;
}

bool flash_write_sector(uint32_t addr, uint8_t *buffer)
{
    uint8_t txbuf[5];
    uint8_t rxbuf[5];

    for (int i = 0; i < 4096; i += 256) {
	txbuf[0] = 0x06;
	xflash_do_cmd_internal(txbuf, rxbuf, 1, 0);
	xflash_put_cmd_addr32(0x12, addr + i);
	xflash_do_cmd_internal(&buffer[i], NULL, 256, 5);
	xflash_wait_ready();
    }

    return true;
}

bool flash_read_0C(uint32_t addr, uint8_t *buffer, uint32_t len)
{
    xflash_put_cmd_addr32(0x0c, addr);
    io_write(N64CART_SSI_DR0, 0); // dummy
    xflash_do_cmd_internal(NULL, buffer, len, 6);

    return true;
}

uint8_t flash_read8_0C(uint32_t addr)
{
    uint16_t val;

    uint8_t txbuf[7];
    uint8_t rxbuf[7];

    txbuf[0] = 0x0c;
    txbuf[1] = addr >> 24;
    txbuf[2] = addr >> 16;
    txbuf[3] = addr >> 8;
    txbuf[4] = addr;

    xflash_do_cmd_internal(txbuf, rxbuf, 7, 0);

    val = rxbuf[6];

    return val;
}

uint16_t flash_read16_0C(uint32_t addr)
{
    uint16_t val;

    uint8_t txbuf[8];
    uint8_t rxbuf[8];

    txbuf[0] = 0x0c;
    txbuf[1] = addr >> 24;
    txbuf[2] = addr >> 16;
    txbuf[3] = addr >> 8;
    txbuf[4] = addr;

    xflash_do_cmd_internal(txbuf, rxbuf, 8, 0);

    val = *((uint16_t *)&rxbuf[6]);

    return val;
}

uint32_t flash_read32_0C(uint32_t addr)
{
    uint32_t val;

    uint8_t txbuf[10];
    uint8_t rxbuf[10];

    txbuf[0] = 0x0c;
    txbuf[1] = addr >> 24;
    txbuf[2] = addr >> 16;
    txbuf[3] = addr >> 8;
    txbuf[4] = addr;

    xflash_do_cmd_internal(txbuf, rxbuf, 10, 0);

    val = *((uint32_t *)&rxbuf[6]);

    return val;
}

uint32_t n64cart_fw_size(void)
{
    return (io_read(N64CART_FW_SIZE) & 0xffff) << 12;
}

void n64cart_sram_lock(void)
{
    uint32_t ctrl = io_read(N64CART_SYS_CTRL);
    ctrl &= ~N64CART_SRAM_UNLOCK;
    io_write(N64CART_SYS_CTRL, ctrl);
}

void n64cart_sram_unlock(void)
{
    uint32_t ctrl = io_read(N64CART_SYS_CTRL);
    ctrl |= N64CART_SRAM_UNLOCK;
    io_write(N64CART_SYS_CTRL, ctrl);
}

void n64cart_eeprom_16kbit(bool enable)
{
    uint32_t ctrl = io_read(N64CART_SYS_CTRL);
    if (enable) {
	ctrl |= N64CART_EEPROM_16KBIT;
    } else {
	ctrl &= ~N64CART_EEPROM_16KBIT;
    }
    io_write(N64CART_SYS_CTRL, ctrl);
}

bool n64cart_is_eeprom_16kbit(void)
{
    return io_read(N64CART_SYS_CTRL) & N64CART_EEPROM_16KBIT;
}
