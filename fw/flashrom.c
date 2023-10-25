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

#define FLASH_BLOCK_ERASE_CMD 0xd8

static ssi_hw_t *const ssi = (ssi_hw_t *) XIP_SSI_BASE;

#if 1
#define BOOT2_SIZE_WORDS 64

static uint32_t boot2_copyout[BOOT2_SIZE_WORDS];
static bool boot2_copyout_valid = false;

static void __no_inline_not_in_flash_func(flash_init_boot2_copyout)(void) {
    if (boot2_copyout_valid)
        return;
    for (int i = 0; i < BOOT2_SIZE_WORDS; ++i)
        boot2_copyout[i] = ((uint32_t *)XIP_BASE)[i];
    __compiler_memory_barrier();
    boot2_copyout_valid = true;
}

static void __no_inline_not_in_flash_func(flash_enable_xip_via_boot2)(void) {
    ((void (*)(void))boot2_copyout+1)();
}
#else
static void __no_inline_not_in_flash_func(flash_init_boot2_copyout)(void) {}

static void __no_inline_not_in_flash_func(flash_enable_xip_via_boot2)(void) {
    // Set up XIP for 03h read on bus access (slow but generic)
    rom_flash_enter_cmd_xip_fn flash_enter_cmd_xip = (rom_flash_enter_cmd_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_ENTER_CMD_XIP);
    assert(flash_enter_cmd_xip);
    flash_enter_cmd_xip();
}
#endif

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

void __no_inline_not_in_flash_func(flash_set_ea_reg)(uint8_t addr)
{
    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_flush_cache_fn flash_flush_cache = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash && flash_exit_xip && flash_flush_cache);
    flash_init_boot2_copyout();
    __compiler_memory_barrier();
    connect_internal_flash();
    flash_exit_xip();

    uint8_t txbuf[4];
    uint8_t rxbuf[4];

    txbuf[0] = 0x06;
    xflash_do_cmd_internal(txbuf, rxbuf, 1, 0);

    txbuf[0] = 0xc5;
    txbuf[1] = addr;
    xflash_do_cmd_internal(txbuf, rxbuf, 2, 0);

    txbuf[0] = 0x04;
    xflash_do_cmd_internal(txbuf, rxbuf, 1, 0);

    flash_flush_cache();
    flash_enable_xip_via_boot2();
}

uint8_t __no_inline_not_in_flash_func(flash_get_ea_reg)(void)
{
    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_flush_cache_fn flash_flush_cache = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash && flash_exit_xip && flash_flush_cache);
    flash_init_boot2_copyout();
    __compiler_memory_barrier();
    connect_internal_flash();
    flash_exit_xip();

    uint8_t txbuf[2];
    uint8_t rxbuf[2];

    txbuf[0] = 0xc8;
    xflash_do_cmd_internal(txbuf, rxbuf, 2, 0);

    flash_flush_cache();
    flash_enable_xip_via_boot2();

    return rxbuf[1];
}

void __no_inline_not_in_flash_func(flash_spi_mode)(void)
{
#if 0
    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_flush_cache_fn flash_flush_cache = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash && flash_exit_xip && flash_flush_cache);
    flash_init_boot2_copyout();
    __compiler_memory_barrier();
    connect_internal_flash();
    flash_exit_xip();
#endif

//printf("ssi->ser %d\n", ssi->ser);

#if 1
    ssi->ssienr = 0;

//printf("1) ctrlr0 %08X ctrlr1 %08X spi_ctrlr0 %08X\n", ssi->ctrlr0, ssi->ctrlr1, ssi->spi_ctrlr0);
//printf("ioqspi_hw->io[1].ctrl = %08X\n", ioqspi_hw->io[1].ctrl);
//    for (int i = 0; i < 6; i++) {
//	printf("spi pads_qspi_hw->io[%d] : %08X\n", i, pads_qspi_hw->io[i]);
//    }

    ssi->ctrlr0 =
	(7 << SSI_CTRLR0_DFS_32_LSB) | /* 8 bits per data frame */
	(SSI_CTRLR0_TMOD_VALUE_TX_AND_RX << SSI_CTRLR0_TMOD_LSB);

//    ssi->ser = 1;
    ssi->baudr = 4;
    ssi->ssienr = 1;
#endif

//printf("2) ctrlr0 %08X ctrlr1 %08X spi_ctrlr0 %08X\n", ssi->ctrlr0, ssi->ctrlr1, ssi->spi_ctrlr0);
//printf("ioqspi_hw->io[1].ctrl = %08X\n", ioqspi_hw->io[1].ctrl);
//    for (int i = 0; i < 6; i++) {
//	printf("spi pads_qspi_hw->io[%d] : %08X\n", i, pads_qspi_hw->io[i]);
//    }

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
    ssi->dr0 = cmd;
    for (int i = 0; i < 4; ++i) {
        ssi->dr0 = addr >> 24;
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

//    txbuf[0] = 0x21;
//    txbuf[1] = addr >> 24;
//    txbuf[2] = addr >> 16;
//    txbuf[3] = addr >> 8;
//    txbuf[4] = addr;
    xflash_do_cmd_internal(txbuf, rxbuf, 0, 5);

    xflash_wait_ready();

//    txbuf[0] = 0x04;
//    xflash_do_cmd_internal(txbuf, rxbuf, 1, 0);

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

uint8_t __no_inline_not_in_flash_func(flash_get_status)(void)
{
//    printf("CTRLR0_ENTER_XIP: %08X\n", CTRLR0_ENTER_XIP);
//    printf("SPI_CTRLR0_ENTER_XIP: %08X\n", SPI_CTRLR0_ENTER_XIP);
//    printf("SPI_CTRLR0_XIP: %08X\n", SPI_CTRLR0_XIP);

    printf("xip ctrlr0 : %08X\n", ssi_hw->ctrlr0);
    printf("xip spi_ctrlr0 : %08X\n", ssi_hw->spi_ctrlr0);

    for (int i = 0; i < 6; i++) {
	printf("xip pads_qspi_hw->io[%d] : %08X\n", i, pads_qspi_hw->io[i]);
    }

    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_flush_cache_fn flash_flush_cache = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash && flash_exit_xip && flash_flush_cache);
    flash_init_boot2_copyout();
    __compiler_memory_barrier();
    connect_internal_flash();
    flash_exit_xip();

    uint8_t txbuf[32];
    uint8_t rxbuf[32];

    txbuf[0] = 0x05;
    xflash_do_cmd_internal(txbuf, rxbuf, 2, 0);
    printf("Status register 1 %02X\n", rxbuf[1]);

    txbuf[0] = 0x35;
    xflash_do_cmd_internal(txbuf, rxbuf, 2, 0);
    printf("Status register 2 %02X\n", rxbuf[1]);

    txbuf[0] = 0x15;
    xflash_do_cmd_internal(txbuf, rxbuf, 2, 0);
    printf("Status register 3 %02X\n", rxbuf[1]);

//    txbuf[0] = 0xec;
//    txbuf[1] = 0;
//    txbuf[2] = 0;
//    txbuf[3] = 0
//    txbuf[4] = 0;
//    txbuf[5] = 0x00;
//    xflash_do_cmd_internal(txbuf, rxbuf, 32, 0);

    printf("spi ctrlr0 : %08X\n", ssi_hw->ctrlr0);
    printf("spi spi_ctrlr0 : %08X\n", ssi_hw->spi_ctrlr0);

    for (int i = 0; i < 6; i++) {
	printf("spi pads_qspi_hw->io[%d] : %08X\n", i, pads_qspi_hw->io[i]);
    }

    flash_flush_cache();
    flash_enable_xip_via_boot2();

    return rxbuf[1];
}

void __no_inline_not_in_flash_func(flash_quad_mode)(bool use_a32)
{
    if (use_a32) {
	ssi->ssienr = 0;

//printf("3) ctrlr0 %08X ctrlr1 %08X spi_ctrlr0 %08X\n", ssi->ctrlr0, ssi->ctrlr1, ssi->spi_ctrlr0);
//printf("ioqspi_hw->io[1].ctrl = %08X\n", ioqspi_hw->io[1].ctrl);
//    for (int i = 0; i < 6; i++) {
//	printf("spi pads_qspi_hw->io[%d] : %08X\n", i, pads_qspi_hw->io[i]);
//    }

flash_cs_force(1);
#if 1
//	ioqspi_hw->io[1].ctrl = 0;
//	ssi->ser = 1;
	ssi->baudr = 4;

	ssi->ctrlr0 =
	    (SSI_CTRLR0_SPI_FRF_VALUE_QUAD << SSI_CTRLR0_SPI_FRF_LSB) |                          /* Quad I/O mode */
	    (31 << SSI_CTRLR0_DFS_32_LSB)  |       /* 32 data bits */
	    (SSI_CTRLR0_TMOD_VALUE_EEPROM_READ     /* Send INST/ADDR, Receive Data */ \
		<< SSI_CTRLR0_TMOD_LSB);

	ssi->ctrlr1 = 0;
#endif

#if 1
	ssi->spi_ctrlr0 =
	    (10u << SSI_SPI_CTRLR0_ADDR_L_LSB) |	/* (Address + mode bits) / 4 */
	    (4u  << SSI_SPI_CTRLR0_WAIT_CYCLES_LSB) |	/* Hi-Z dummy clocks following address + mode */
	    (SSI_SPI_CTRLR0_INST_L_VALUE_8B << SSI_SPI_CTRLR0_INST_L_LSB) | /* 8-bit instruction */
	    (SSI_SPI_CTRLR0_TRANS_TYPE_VALUE_1C2A	/* Send Command in serial mode then address in Quad I/O mode */
		    << SSI_SPI_CTRLR0_TRANS_TYPE_LSB);
#endif
	ssi->ssienr = 1;

//printf("111\n");
	flash_quad_read32_EC(0);
//printf("222\n");
	flash_quad_read32_EC(0);
//printf("333\n");
	flash_quad_read32_EC(0);
//printf("444\n");

	ssi->ssienr = 0;

#if 0
	ssi->spi_ctrlr0 =
	    (0xa0 << SSI_SPI_CTRLR0_XIP_CMD_LSB) |	/* Mode bits to keep flash in continuous read mode */
	    (10u  << SSI_SPI_CTRLR0_ADDR_L_LSB) |	/* Total number of address + mode bits */
	    (4u << SSI_SPI_CTRLR0_WAIT_CYCLES_LSB) |	/* Hi-Z dummy clocks following address + mode */
	    (SSI_SPI_CTRLR0_INST_L_VALUE_NONE << SSI_SPI_CTRLR0_INST_L_LSB) | /* Do not send a command, instead send XIP_CMD as mode bits after address */
	    (SSI_SPI_CTRLR0_TRANS_TYPE_VALUE_2C2A	/* Send Address in Quad I/O mode (and Command but that is zero bits long) */
		    << SSI_SPI_CTRLR0_TRANS_TYPE_LSB);

#endif

//printf("4) ctrlr0 %08X ctrlr1 %08X spi_ctrlr0 %08X\n", ssi->ctrlr0, ssi->ctrlr1, ssi->spi_ctrlr0);
//printf("ioqspi_hw->io[1].ctrl = %08X\n", ioqspi_hw->io[1].ctrl);
//    for (int i = 0; i < 6; i++) {
//	printf("spi pads_qspi_hw->io[%d] : %08X\n", i, pads_qspi_hw->io[i]);
//    }

	ssi->ssienr = 1;
    }

    return;
}

uint32_t __no_inline_not_in_flash_func(flash_quad_read32_EB)(uint32_t addr)
{
//    while(ssi_hw->sr & SSI_SR_BUSY_BITS) {}

    ssi_hw->dr0 = (addr << 8) | 0xa0;

    while (!(ssi_hw->sr & SSI_SR_RFNE_BITS)) {}

    uint32_t val = ssi_hw->dr0;

    return val;
}

uint16_t __no_inline_not_in_flash_func(flash_quad_read16_EB)(uint32_t addr)
{
//    while(ssi_hw->sr & SSI_SR_BUSY_BITS) {}

    ssi_hw->dr0 = (addr << 8) | 0xa0;

    while (!(ssi_hw->sr & SSI_SR_RFNE_BITS)) {}

    uint32_t val = ssi_hw->dr0;

    uint16_t val16 = (val >> 24) | ((val >> 8) & 0xff00);

    return val16;
}

uint32_t __no_inline_not_in_flash_func(flash_quad_read32_EC)(uint32_t addr)
{
flash_cs_force(0);
    ssi_hw->dr0 = 0xec;
    ssi_hw->dr0 = addr;
    ssi_hw->dr0 = 0; //0xa0 << 24;

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
    ssi_hw->dr0 = 0; //0xa0 << 24;

    while (!(ssi_hw->sr & SSI_SR_RFNE_BITS)) {}

    uint32_t val = ssi_hw->dr0;
flash_cs_force(1);

    uint16_t val16 = (val >> 24) | ((val >> 8) & 0xff00);

    return val16;
}
