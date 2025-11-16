#include <stdio.h>
#include <stdbool.h>

#include "flashrom.h"
#include "pico/bootrom.h"

#include "hardware/regs/io_qspi.h"
#include "hardware/regs/pads_qspi.h"
#include "hardware/structs/ssi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/resets.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/pads_qspi.h"

#if defined(DISABLE_FLASH_ADDR_32) && (DISABLE_FLASH_ADDR_32 == 1)
#define ADDR_L (8u)
#define SECTOR_ERASE (0x20)
#define SECTOR_WRITE (0x02)
#define BYTE_READ (0x0b)
#define CMD_ADDR_LEN (4)
#else
#define ADDR_L (10u)
#define SECTOR_ERASE (0x21)
#define SECTOR_WRITE (0x12)
#define BYTE_READ (0x0c)
#define CMD_ADDR_LEN (5)
#endif

static inline void xflash_put_get(const uint8_t * tx, uint8_t * rx, size_t count, size_t rx_skip)
{
    const uint max_in_flight = 16 - 2; // account for data internal to SSI
    size_t tx_count = count;
    size_t rx_count = count;
    while (tx_count || rx_skip || rx_count) {
        // NB order of reads, for pessimism rather than optimism
        if (ssi_hw->sr & SSI_SR_BUSY_BITS) {
            continue;
        }
        uint32_t tx_level = ssi_hw->txflr;
        uint32_t rx_level = ssi_hw->rxflr;
        if (tx_count && tx_level + rx_level < max_in_flight) {
            ssi_hw->dr0 = (uint32_t) (tx ? *tx++ : 0);
            --tx_count;
        }
        if (rx_level) {
            uint8_t rxbyte = ssi_hw->dr0;
            if (rx_skip) {
                --rx_skip;
            } else {
                if (rx)
                    *rx++ = rxbyte;
                --rx_count;
            }
        }
    }

    flash_cs_force(1);
}

static void xflash_do_cmd(uint8_t cmd, const uint8_t * txbuf, uint8_t * rxbuf, size_t count)
{
    flash_cs_force(0);
    ssi_hw->dr0 = cmd;
    xflash_put_get(txbuf, rxbuf, count, 1);
}

void flash_spi_mode(void)
{
    ssi_hw->ssienr = 0;

    ssi_hw->ctrlr0 = (7 << SSI_CTRLR0_DFS_32_LSB) |     /* 8 bits per data frame */
            (SSI_CTRLR0_TMOD_VALUE_TX_AND_RX << SSI_CTRLR0_TMOD_LSB);

    ssi_hw->baudr = 4;
    ssi_hw->ssienr = 1;

    return;
}

void flash_config(void)
{
    uint8_t sr3;

    xflash_do_cmd(0x15, NULL, &sr3, 1);

    sr3 &= ~0x60;
    sr3 |= 0x40; // driver strength 50%

    xflash_do_cmd(0x50, NULL, NULL, 0);
    xflash_do_cmd(0x11, &sr3, NULL, 1);
}

static void xflash_wait_ready(void)
{
    uint8_t stat;
    do {
        xflash_do_cmd(0x05, NULL, &stat, 1);
    } while (stat & 0x1);
}

static inline void xflash_put_cmd_addr(uint8_t cmd, uint32_t addr)
{
    flash_cs_force(0);
    ssi_hw->dr0 = cmd;
#if defined(DISABLE_FLASH_ADDR_32) && (DISABLE_FLASH_ADDR_32 == 1)
    addr <<= 8;
    for (int i = 0; i < 3; ++i) {
#else
    for (int i = 0; i < 4; ++i) {
#endif
        ssi_hw->dr0 = addr >> 24;
        addr <<= 8;
    }
}

bool flash_erase_sector(uint32_t addr)
{
    xflash_do_cmd(0x06, NULL, NULL, 0);

    xflash_put_cmd_addr(SECTOR_ERASE, addr);
    xflash_put_get(NULL, NULL, 0, CMD_ADDR_LEN);

    xflash_wait_ready();

    return true;
}

bool flash_write_sector(uint32_t addr, uint8_t * buffer)
{
    for (int i = 0; i < 4096; i += 256) {
        xflash_do_cmd(0x06, NULL, NULL, 0);

        xflash_put_cmd_addr(SECTOR_WRITE, addr + i);
        xflash_put_get(&buffer[i], NULL, 256, CMD_ADDR_LEN);

        xflash_wait_ready();
    }

    return true;
}

bool flash_read(uint32_t addr, uint8_t * buffer, uint32_t len)
{
    xflash_put_cmd_addr(BYTE_READ, addr);
    ssi_hw->dr0 = 0;            // dummy
    xflash_put_get(NULL, buffer, len, CMD_ADDR_LEN + 1);

    return true;
}

uint8_t flash_read8(uint32_t addr)
{
    uint8_t val;

    xflash_put_cmd_addr(BYTE_READ, addr);
    ssi_hw->dr0 = 0;            // dummy
    xflash_put_get(NULL, &val, 1, CMD_ADDR_LEN + 1);

    return val;
}

uint16_t flash_read16(uint32_t addr)
{
    uint16_t val;

    uint8_t rxbuf[2];

    xflash_put_cmd_addr(BYTE_READ, addr);
    ssi_hw->dr0 = 0;            // dummy
    xflash_put_get(NULL, rxbuf, 2, CMD_ADDR_LEN + 1);

    val = (rxbuf[0] << 8) | rxbuf[1];

    return val;
}

uint32_t flash_read32(uint32_t addr)
{
    uint32_t val;

    uint8_t rxbuf[4];

    xflash_put_cmd_addr(BYTE_READ, addr);
    ssi_hw->dr0 = 0;            // dummy
    xflash_put_get(NULL, rxbuf, 4, CMD_ADDR_LEN + 1);

    val = *((uint32_t *) rxbuf);

    return val;
}

void flash_quad_gpio_init(void)
{
#ifdef PICO_BOOT_STAGE2_CHOOSE_GENERIC_03H
    uint8_t buf_in[4];
    uint8_t buf_out[4];

    xflash_do_cmd(0x06, NULL, NULL, 0);

    buf_in[0] = 0x40;
    xflash_do_cmd(0x01, buf_in, NULL, 1);

    xflash_do_cmd(0x05, NULL, buf_out, 2);

    // Set pad configuration:
    // - SCLK 8mA drive, no slew limiting
    // - SDx disable input Schmitt to reduce delay

    io_rw_32 *reg = (io_rw_32 *) (PADS_QSPI_BASE + PADS_QSPI_GPIO_QSPI_SCLK_OFFSET);
    *reg = (2 << PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_LSB | PADS_QSPI_GPIO_QSPI_SCLK_SLEWFAST_BITS);
    (void)*reg;

    reg = (io_rw_32 *) (PADS_QSPI_BASE + PADS_QSPI_GPIO_QSPI_SD0_OFFSET);
    *reg = (*reg & ~PADS_QSPI_GPIO_QSPI_SD0_SCHMITT_BITS);
    (void)*reg;
    reg = (io_rw_32 *) (PADS_QSPI_BASE + PADS_QSPI_GPIO_QSPI_SD1_OFFSET);
    *reg = (*reg & ~PADS_QSPI_GPIO_QSPI_SD1_SCHMITT_BITS);
    (void)*reg;
    reg = (io_rw_32 *) (PADS_QSPI_BASE + PADS_QSPI_GPIO_QSPI_SD2_OFFSET);
    *reg = (*reg & ~PADS_QSPI_GPIO_QSPI_SD2_SCHMITT_BITS);
    (void)*reg;
    reg = (io_rw_32 *) (PADS_QSPI_BASE + PADS_QSPI_GPIO_QSPI_SD3_OFFSET);
    *reg = (*reg & ~PADS_QSPI_GPIO_QSPI_SD3_SCHMITT_BITS);
    (void)*reg;

    reg = (io_rw_32 *) (IO_QSPI_BASE + IO_QSPI_GPIO_QSPI_SD0_CTRL_OFFSET);
    *reg = (*reg & ~IO_QSPI_GPIO_QSPI_SD0_CTRL_OUTOVER_BITS) | (0 << IO_QSPI_GPIO_QSPI_SD0_CTRL_OUTOVER_LSB);
    (void)*reg;
    reg = (io_rw_32 *) (IO_QSPI_BASE + IO_QSPI_GPIO_QSPI_SD1_CTRL_OFFSET);
    *reg = (*reg & ~IO_QSPI_GPIO_QSPI_SD1_CTRL_OUTOVER_BITS) | (0 << IO_QSPI_GPIO_QSPI_SD1_CTRL_OUTOVER_LSB);
    (void)*reg;
    reg = (io_rw_32 *) (IO_QSPI_BASE + IO_QSPI_GPIO_QSPI_SD2_CTRL_OFFSET);
    *reg = (*reg & ~IO_QSPI_GPIO_QSPI_SD2_CTRL_OUTOVER_BITS) | (0 << IO_QSPI_GPIO_QSPI_SD2_CTRL_OUTOVER_LSB);
    (void)*reg;
    reg = (io_rw_32 *) (IO_QSPI_BASE + IO_QSPI_GPIO_QSPI_SD3_CTRL_OFFSET);
    *reg = (*reg & ~IO_QSPI_GPIO_QSPI_SD3_CTRL_OUTOVER_BITS) | (0 << IO_QSPI_GPIO_QSPI_SD3_CTRL_OUTOVER_LSB);
    (void)*reg;
#endif
}

void flash_quad_cont_read_mode(void)
{
    ssi_hw->ssienr = 0;

    flash_cs_force(1);

    ssi_hw->baudr = 4;

    ssi_hw->ctrlr0 = (SSI_CTRLR0_SPI_FRF_VALUE_QUAD << SSI_CTRLR0_SPI_FRF_LSB) |        /* Quad I/O mode */
            (15 << SSI_CTRLR0_DFS_32_LSB) | /* 16 data bits */
            (SSI_CTRLR0_TMOD_VALUE_EEPROM_READ      /* Send INST/ADDR, Receive Data */
             << SSI_CTRLR0_TMOD_LSB);

    ssi_hw->ctrlr1 = 0;

    ssi_hw->spi_ctrlr0 = (ADDR_L << SSI_SPI_CTRLR0_ADDR_L_LSB) |   /* (Address + mode bits) / 4 */
            (4u << SSI_SPI_CTRLR0_WAIT_CYCLES_LSB) |        /* Hi-Z dummy clocks following address + mode */
            (SSI_SPI_CTRLR0_INST_L_VALUE_8B << SSI_SPI_CTRLR0_INST_L_LSB) | /* 8-bit instruction */
            (SSI_SPI_CTRLR0_TRANS_TYPE_VALUE_1C2A   /* Send Command in serial mode then address in Quad I/O mode */
             << SSI_SPI_CTRLR0_TRANS_TYPE_LSB);

    ssi_hw->ssienr = 1;

    io_rw_32 *reg = (io_rw_32 *) (IO_QSPI_BASE + IO_QSPI_GPIO_QSPI_SS_CTRL_OFFSET);
    *reg = (*reg & ~IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS) | (0 << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB);
    (void)*reg;

#if defined(DISABLE_FLASH_ADDR_32) && (DISABLE_FLASH_ADDR_32 == 1)
    ssi_hw->dr0 = 0xeb;
#else
    ssi_hw->dr0 = 0xec;
    ssi_hw->dr0 = 0;
#endif
    ssi_hw->dr0 = MODE_CONTINUOS_READ;
    while (!(ssi_hw->sr & SSI_SR_RFNE_BITS) && (ssi_hw->sr & SSI_SR_BUSY_BITS)) {
    }
    (void)ssi_hw->dr0;

    ssi_hw->ssienr = 0;

    ssi_hw->spi_ctrlr0 = (MODE_CONTINUOS_READ << SSI_SPI_CTRLR0_XIP_CMD_LSB) | /* Mode bits to keep flash in continuous read mode */
            (ADDR_L << SSI_SPI_CTRLR0_ADDR_L_LSB) |    /* (Address + mode bits) / 4 */
            (4u << SSI_SPI_CTRLR0_WAIT_CYCLES_LSB) |        /* Hi-Z dummy clocks following address + mode */
            (SSI_SPI_CTRLR0_INST_L_VALUE_NONE << SSI_SPI_CTRLR0_INST_L_LSB) |       /* Do not send a command, instead send XIP_CMD as mode bits after address */
            (SSI_SPI_CTRLR0_TRANS_TYPE_VALUE_2C2A   /* Send Address in Quad I/O mode (and Command but that is zero bits long) */
             << SSI_SPI_CTRLR0_TRANS_TYPE_LSB);

    ssi_hw->ssienr = 1;

    return;
}
