/**
 * Copyright (c) 2022-2024 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <libdragon.h>
#include "n64cart.h"

#if defined(DISABLE_FLASH_ADDR_32) && (DISABLE_FLASH_ADDR_32 == 1)
#define SECTOR_ERASE (0x20)
#define SECTOR_WRITE (0x02)
#define BYTE_READ (0x0b)
#define CMD_ADDR_LEN (4)
#else
#define SECTOR_ERASE (0x21)
#define SECTOR_WRITE (0x12)
#define BYTE_READ (0x0c)
#define CMD_ADDR_LEN (5)
#endif

uint8_t n64cart_uart_getc(void)
{
#ifndef DISABLE_UART
    while (!(io_read(N64CART_UART_CTRL) & N64CART_UART_RX_AVAIL)) ;

    return io_read(N64CART_UART_RXTX) & 0xff;
#else
    return 0;
#endif
}

void n64cart_uart_putc(uint8_t data)
{
#ifndef DISABLE_UART
    while (!(io_read(N64CART_UART_CTRL) & N64CART_UART_TX_FREE)) ;

    io_write(N64CART_UART_RXTX, data);
#endif
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
    uint32_t ctrl = pi_io_read(N64CART_SYS_CTRL);
    if (high) {
        ctrl |= N64CART_FLASH_CS_HIGH;
    } else {
        ctrl &= ~N64CART_FLASH_CS_HIGH;
    }
    pi_io_write(N64CART_SYS_CTRL, ctrl);
}

static void flash_put_get(const uint8_t *txbuf, uint8_t *rxbuf, size_t count, size_t rxskip)
{
    size_t tx_remaining = count;
    size_t rx_remaining = count;
    // We may be interrupted -- don't want FIFO to overflow if we're distracted.
    const size_t max_in_flight = 16 - 2;
    while (tx_remaining || rx_remaining || rxskip) {
        uint32_t flags = pi_io_read(N64CART_SSI_SR);
        bool can_put = !!(flags & N64CART_SSI_SR_TFNF_BITS);
        bool can_get = !!(flags & N64CART_SSI_SR_RFNE_BITS);
        if (can_put && tx_remaining && rx_remaining - tx_remaining < max_in_flight) {
            if (txbuf) {
                pi_io_write(N64CART_SSI_DR0, *txbuf++);
            } else {
                pi_io_write(N64CART_SSI_DR0, 0);
            }
            --tx_remaining;
        }
        if (can_get && (rx_remaining || rxskip)) {
            uint8_t rxbyte = pi_io_read(N64CART_SSI_DR0) & 0xff;
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

void flash_do_cmd(uint8_t cmd, const uint8_t *txbuf, uint8_t *rxbuf, size_t count)
{
    flash_cs_force(0);
    pi_io_write(N64CART_SSI_DR0, cmd);
    flash_put_get(txbuf, rxbuf, count, 1);
}

void flash_mode(bool mode)
{
    uint32_t ctrl = pi_io_read(N64CART_SYS_CTRL);
    if (mode) {
        ctrl |= N64CART_FLASH_MODE_QUAD;
    } else {
        ctrl &= ~N64CART_FLASH_MODE_QUAD;
    }
    pi_io_write(N64CART_SYS_CTRL, ctrl);
}

static void flash_wait_ready(void)
{
    uint8_t stat;
    do {
        flash_do_cmd(0x05, NULL, &stat, 1);
    } while (stat & 0x1);
}

static inline void flash_put_cmd_addr(uint8_t cmd, uint32_t addr)
{
    flash_cs_force(0);
    pi_io_write(N64CART_SSI_DR0, cmd);
#if defined(DISABLE_FLASH_ADDR_32) && (DISABLE_FLASH_ADDR_32 == 1)
    addr <<= 8;
    for (int i = 0; i < 3; ++i) {
#else
    for (int i = 0; i < 4; ++i) {
#endif
        pi_io_write(N64CART_SSI_DR0, addr >> 24);
        addr <<= 8;
    }
}

bool flash_erase_sector(uint32_t addr)
{
    flash_do_cmd(0x06, NULL, NULL, 0);

    flash_put_cmd_addr(SECTOR_ERASE, addr);
    flash_put_get(NULL, NULL, 0, CMD_ADDR_LEN);

    flash_wait_ready();

    return true;
}

bool flash_write_sector(uint32_t addr, uint8_t *buffer)
{
    for (int i = 0; i < 4096; i += 256) {
        flash_do_cmd(0x06, NULL, NULL, 0);

        flash_put_cmd_addr(SECTOR_WRITE, addr + i);
        flash_put_get(&buffer[i], NULL, 256, CMD_ADDR_LEN);

        flash_wait_ready();
    }

    return true;
}

bool flash_read(uint32_t addr, uint8_t *buffer, uint32_t len)
{
    flash_put_cmd_addr(BYTE_READ, addr);
    pi_io_write(N64CART_SSI_DR0, 0);    // dummy
    flash_put_get(NULL, buffer, len, CMD_ADDR_LEN + 1);

    return true;
}

uint8_t flash_read8(uint32_t addr)
{
    uint8_t val;

    flash_put_cmd_addr(BYTE_READ, addr);
    pi_io_write(N64CART_SSI_DR0, 0);    // dummy
    flash_put_get(NULL, &val, 1, CMD_ADDR_LEN + 1);

    return val;
}

uint16_t flash_read16(uint32_t addr)
{
    uint16_t val;

    uint8_t rxbuf[2];

    flash_put_cmd_addr(BYTE_READ, addr);
    pi_io_write(N64CART_SSI_DR0, 0);    // dummy
    flash_put_get(NULL, rxbuf, 2, CMD_ADDR_LEN + 1);

    val = *((uint16_t *) rxbuf);

    return val;
}

uint32_t flash_read32(uint32_t addr)
{
    uint32_t val;

    uint8_t rxbuf[4];

    flash_put_cmd_addr(BYTE_READ, addr);
    pi_io_write(N64CART_SSI_DR0, 0);    // dummy
    flash_put_get(NULL, rxbuf, 4, CMD_ADDR_LEN + 1);

    val = *((uint32_t *) rxbuf);

    return val;
}

uint32_t n64cart_fw_size(void)
{
    return io_read(N64CART_FW_SIZE);
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
