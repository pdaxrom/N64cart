#pragma once

#include <stdint.h>

#include "hardware/regs/pads_qspi.h"
#include "hardware/structs/ssi.h"
#include "hardware/structs/ioqspi.h"

inline void xxx_hw_xor_bits(io_rw_32 *addr, uint32_t mask)
{
    *(io_rw_32 *) hw_xor_alias_untyped((volatile void *)addr) = mask;
}

inline void xxx_hw_write_masked(io_rw_32 *addr, uint32_t values, uint32_t write_mask)
{
    xxx_hw_xor_bits(addr, (*addr ^ values) & write_mask);
}

void inline flash_cs_force(bool high)
{
    uint32_t field_val = high ? IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_HIGH : IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_LOW;
    xxx_hw_write_masked(&ioqspi_hw->io[1].ctrl, field_val << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB, IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS);
}

void flash_spi_mode(void);

void flash_config(void);

bool flash_erase_sector(uint32_t addr);

bool flash_write_sector(uint32_t addr, uint8_t * buffer);

bool flash_read(uint32_t addr, uint8_t * buffer, uint32_t len);

uint8_t flash_read8(uint32_t addr);

uint16_t flash_read16(uint32_t addr);

uint32_t flash_read32(uint32_t addr);

#ifdef PICO_BOOT_STAGE2_CHOOSE_MX66L
#define MODE_CONTINUOS_READ 0xf0
#endif

#ifndef MODE_CONTINUOS_READ
#define MODE_CONTINUOS_READ 0xa0
#endif

void flash_quad_gpio_init(void);

void flash_quad_cont_read_mode(void);

uint16_t inline flash_quad_read16(uint32_t addr)
{
#ifdef DISABLE_FLASH_ADDR_32
    ssi_hw->dr0 = (addr << 8) | MODE_CONTINUOS_READ;
#else
    ssi_hw->dr0 = addr;
    ssi_hw->dr0 = MODE_CONTINUOS_READ;
#endif

    while (!(ssi_hw->sr & SSI_SR_RFNE_BITS)) {
    }
    return ssi_hw->dr0;
}

void inline flash_quad_exit_cont_read_mode()
{
#ifdef DISABLE_FLASH_ADDR_32
    ssi_hw->dr0 = 0x0;
#else
    ssi_hw->dr0 = 0x0;
    ssi_hw->dr0 = 0x0;
#endif

    while (!(ssi_hw->sr & SSI_SR_RFNE_BITS)) {
    }
    (void)ssi_hw->dr0;
}
