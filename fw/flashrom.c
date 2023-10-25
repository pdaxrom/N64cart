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

static void __no_inline_not_in_flash_func(flash_cs_force)(bool high) {
    uint32_t field_val = high ?
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_HIGH :
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_LOW;
    hw_write_masked(&ioqspi_hw->io[1].ctrl,
        field_val << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB,
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS
    );
}

static void __no_inline_not_in_flash_func(xflash_do_cmd_internal)(const uint8_t *txbuf, uint8_t *rxbuf, size_t count, size_t rxskip)
{
    flash_cs_force(0);
    size_t tx_remaining = count;
    size_t rx_remaining = count;
    // We may be interrupted -- don't want FIFO to overflow if we're distracted.
    const size_t max_in_flight = 16 - 2;
    while (tx_remaining || rx_remaining || rxskip) {
        uint32_t flags = ssi_hw->sr;
        bool can_put = !!(flags & SSI_SR_TFNF_BITS);
        bool can_get = !!(flags & SSI_SR_RFNE_BITS);
        if (can_put && tx_remaining && rx_remaining - tx_remaining < max_in_flight) {
            ssi_hw->dr0 = *txbuf++;
            --tx_remaining;
        }
        if (can_get && (rx_remaining || rxskip)) {
            uint8_t rxbyte = (uint8_t)ssi_hw->dr0;
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

void __no_inline_not_in_flash_func(flash_spi_mode)(void)
{
    ssi_hw->ssienr = 0;

    ssi_hw->ctrlr0 =
	(7 << SSI_CTRLR0_DFS_32_LSB) | /* 8 bits per data frame */
	(SSI_CTRLR0_TMOD_VALUE_TX_AND_RX << SSI_CTRLR0_TMOD_LSB);

//    ssi_hw->ser = 1;
    ssi_hw->baudr = 4;
    ssi_hw->ssienr = 1;

    return;
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
    ssi_hw->dr0 = cmd;
    for (int i = 0; i < 4; ++i) {
        ssi_hw->dr0 = addr >> 24;
        addr <<= 8;
    }
}

bool __no_inline_not_in_flash_func(flash_erase_sector)(uint32_t addr)
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

bool __no_inline_not_in_flash_func(flash_write_sector)(uint32_t addr, uint8_t *buffer)
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

uint8_t __no_inline_not_in_flash_func(flash_read8_0C)(uint32_t addr)
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

uint16_t __no_inline_not_in_flash_func(flash_read16_0C)(uint32_t addr)
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

uint32_t __no_inline_not_in_flash_func(flash_read32_0C)(uint32_t addr)
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

void __no_inline_not_in_flash_func(flash_quad_mode)(void)
{
    ssi_hw->ssienr = 0;

    flash_cs_force(1);

//    ssi_hw->ser = 1;
    ssi_hw->baudr = 4;

    ssi_hw->ctrlr0 =
	    (SSI_CTRLR0_SPI_FRF_VALUE_QUAD << SSI_CTRLR0_SPI_FRF_LSB) |                          /* Quad I/O mode */
	    (31 << SSI_CTRLR0_DFS_32_LSB)  |       /* 32 data bits */
	    (SSI_CTRLR0_TMOD_VALUE_EEPROM_READ     /* Send INST/ADDR, Receive Data */ \
		<< SSI_CTRLR0_TMOD_LSB);

    ssi_hw->ctrlr1 = 0;

    ssi_hw->spi_ctrlr0 =
	    (10u << SSI_SPI_CTRLR0_ADDR_L_LSB) |	/* (Address + mode bits) / 4 */
	    (4u  << SSI_SPI_CTRLR0_WAIT_CYCLES_LSB) |	/* Hi-Z dummy clocks following address + mode */
	    (SSI_SPI_CTRLR0_INST_L_VALUE_8B << SSI_SPI_CTRLR0_INST_L_LSB) | /* 8-bit instruction */
	    (SSI_SPI_CTRLR0_TRANS_TYPE_VALUE_1C2A	/* Send Command in serial mode then address in Quad I/O mode */
		    << SSI_SPI_CTRLR0_TRANS_TYPE_LSB);
    ssi_hw->ssienr = 1;

    flash_quad_read32_EC(0);
    flash_quad_read32_EC(0);
    flash_quad_read32_EC(0);

    ssi_hw->ssienr = 0;

    ssi_hw->ssienr = 1;

    return;
}

uint32_t __no_inline_not_in_flash_func(flash_quad_read32_EC)(uint32_t addr)
{
    flash_cs_force(0);

    ssi_hw->dr0 = 0xec;
    ssi_hw->dr0 = addr;
    ssi_hw->dr0 = 0;

    while (!(ssi_hw->sr & SSI_SR_RFNE_BITS)) {}
    uint32_t val = ssi_hw->dr0;

    flash_cs_force(1);

    return val;
}

uint16_t __no_inline_not_in_flash_func(flash_quad_read16_EC)(uint32_t addr)
{
    flash_cs_force(0);

    ssi_hw->dr0 = 0xec;
    ssi_hw->dr0 = addr;
    ssi_hw->dr0 = 0;

    while (!(ssi_hw->sr & SSI_SR_RFNE_BITS)) {}
    uint32_t val = ssi_hw->dr0;

    flash_cs_force(1);

    uint16_t val16 = val >> 16;

    return val16;
}
