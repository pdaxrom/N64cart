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

static inline void xflash_put_cmd_addr32(uint8_t cmd, uint32_t addr)
{
    flash_cs_force(0);
    ssi_hw->dr0 = cmd;
    for (int i = 0; i < 4; ++i) {
        ssi_hw->dr0 = addr >> 24;
        addr <<= 8;
    }
}

bool flash_erase_sector(uint32_t addr)
{
    xflash_do_cmd(0x06, NULL, NULL, 0);

    xflash_put_cmd_addr32(0x21, addr);
    xflash_put_get(NULL, NULL, 0, 5);

    xflash_wait_ready();

    return true;
}

bool flash_write_sector(uint32_t addr, uint8_t * buffer)
{
    for (int i = 0; i < 4096; i += 256) {
        xflash_do_cmd(0x06, NULL, NULL, 0);

        xflash_put_cmd_addr32(0x12, addr + i);
        xflash_put_get(&buffer[i], NULL, 256, 5);

        xflash_wait_ready();
    }

    return true;
}

bool flash_read_0C(uint32_t addr, uint8_t * buffer, uint32_t len)
{
    xflash_put_cmd_addr32(0x0c, addr);
    ssi_hw->dr0 = 0;            // dummy
    xflash_put_get(NULL, buffer, len, 6);

    return true;
}

uint8_t flash_read8_0C(uint32_t addr)
{
    uint8_t val;

    xflash_put_cmd_addr32(0x0c, addr);
    ssi_hw->dr0 = 0;            // dummy
    xflash_put_get(NULL, &val, 1, 6);

    return val;
}

#if 1
uint16_t flash_read16_0C(uint32_t addr)
{
    uint16_t val;

    uint8_t rxbuf[2];

    xflash_put_cmd_addr32(0x0c, addr);
    ssi_hw->dr0 = 0;            // dummy
    xflash_put_get(NULL, rxbuf, 2, 6);

    val = (rxbuf[0] << 8) | rxbuf[1];

    return val;
}
#else
uint16_t flash_read16_0C(uint32_t addr)
{
    static uint32_t next_addr = 0xffffffff;

    uint16_t val;

    uint8_t txbuf[8];
    uint8_t rxbuf[8];

    int tx_c = 0;
    int rx_c = 0;

    if (addr == next_addr) {
        while (rx_c < 2) {
            if ((ssi_hw->sr & SSI_SR_TFNF_BITS) && (tx_c < 2)) {
                ssi_hw->dr0 = txbuf[tx_c++];
            }
            if ((ssi_hw->sr & SSI_SR_RFNE_BITS) && (rx_c < 2)) {
                rxbuf[rx_c++] = ssi_hw->dr0;
            }
        }

        next_addr = addr + 2;

        val = (rxbuf[0] << 8) | rxbuf[1];

        return val;
    }

    flash_cs_force(1);

    txbuf[0] = 0x0c;
    txbuf[1] = addr >> 24;
    txbuf[2] = addr >> 16;
    txbuf[3] = addr >> 8;
    txbuf[4] = addr;

    flash_cs_force(0);

    while (rx_c < sizeof(rxbuf)) {
        if ((ssi_hw->sr & SSI_SR_TFNF_BITS) && (tx_c < sizeof(txbuf))) {
            ssi_hw->dr0 = txbuf[tx_c++];
        }
        if ((ssi_hw->sr & SSI_SR_RFNE_BITS) && (rx_c < sizeof(rxbuf))) {
            rxbuf[rx_c++] = ssi_hw->dr0;
        }
    }

    next_addr = addr + 2;

    val = (rxbuf[6] << 8) | rxbuf[7];

    return val;
}
#endif

uint32_t flash_read32_0C(uint32_t addr)
{
    uint32_t val;

    uint8_t rxbuf[4];

    xflash_put_cmd_addr32(0x0c, addr);
    ssi_hw->dr0 = 0;            // dummy
    xflash_put_get(NULL, rxbuf, 4, 6);

    val = *((uint32_t *) rxbuf);

    return val;
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

    ssi_hw->spi_ctrlr0 = (10u << SSI_SPI_CTRLR0_ADDR_L_LSB) |   /* (Address + mode bits) / 4 */
        (4u << SSI_SPI_CTRLR0_WAIT_CYCLES_LSB) |        /* Hi-Z dummy clocks following address + mode */
        (SSI_SPI_CTRLR0_INST_L_VALUE_8B << SSI_SPI_CTRLR0_INST_L_LSB) | /* 8-bit instruction */
        (SSI_SPI_CTRLR0_TRANS_TYPE_VALUE_1C2A   /* Send Command in serial mode then address in Quad I/O mode */
         << SSI_SPI_CTRLR0_TRANS_TYPE_LSB);

    ssi_hw->ssienr = 1;

    io_rw_32 *reg = (io_rw_32 *) (IO_QSPI_BASE + IO_QSPI_GPIO_QSPI_SS_CTRL_OFFSET);
    *reg = (*reg & ~IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS) | (0 << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB);
    // Read to flush async bridge
    (void)*reg;

    ssi_hw->dr0 = 0xec;
    ssi_hw->dr0 = 0;
    ssi_hw->dr0 = 0xa0;
    while (!(ssi_hw->sr & SSI_SR_RFNE_BITS) && (ssi_hw->sr & SSI_SR_BUSY_BITS)) {
    }
    (void)ssi_hw->dr0;

    ssi_hw->ssienr = 0;

    ssi_hw->spi_ctrlr0 = (0xa0 << SSI_SPI_CTRLR0_XIP_CMD_LSB) | /* Mode bits to keep flash in continuous read mode */
        (10u << SSI_SPI_CTRLR0_ADDR_L_LSB) |    /* (Address + mode bits) / 4 */
        (4u << SSI_SPI_CTRLR0_WAIT_CYCLES_LSB) |        /* Hi-Z dummy clocks following address + mode */
        (SSI_SPI_CTRLR0_INST_L_VALUE_NONE << SSI_SPI_CTRLR0_INST_L_LSB) |       /* Do not send a command, instead send XIP_CMD as mode bits after address */
        (SSI_SPI_CTRLR0_TRANS_TYPE_VALUE_2C2A   /* Send Address in Quad I/O mode (and Command but that is zero bits long) */
         << SSI_SPI_CTRLR0_TRANS_TYPE_LSB);

    ssi_hw->ssienr = 1;

    return;
}
