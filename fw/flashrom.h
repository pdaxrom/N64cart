#pragma once

#include <stdint.h>

#include "hardware/regs/pads_qspi.h"
#include "hardware/structs/ssi.h"
#include "hardware/structs/ioqspi.h"

inline void xxx_hw_xor_bits(io_rw_32 *addr, uint32_t mask) {
    *(io_rw_32 *) hw_xor_alias_untyped((volatile void *) addr) = mask;
}

inline void xxx_hw_write_masked(io_rw_32 *addr, uint32_t values, uint32_t write_mask) {
    xxx_hw_xor_bits(addr, (*addr ^ values) & write_mask);
}

void inline flash_cs_force(bool high)
{
    uint32_t field_val = high ?
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_HIGH :
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_LOW;
    xxx_hw_write_masked(&ioqspi_hw->io[1].ctrl,
        field_val << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB,
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS
    );
}

void flash_spi_mode(void);

bool flash_erase_sector(uint32_t addr);

bool flash_write_sector(uint32_t addr, uint8_t *buffer);

bool flash_read_0C(uint32_t addr, uint8_t *buffer, uint32_t len);

uint8_t flash_read8_0C(uint32_t addr);

uint16_t flash_read16_0C(uint32_t addr);

uint32_t flash_read32_0C(uint32_t addr);

void flash_quad_mode(void);

uint16_t inline flash_quad_read16_EC(uint32_t addr)
{
    ssi_hw->dr0 = 0xec;
    ssi_hw->dr0 = addr;
    ssi_hw->dr0 = 0;

    while (!(ssi_hw->sr & SSI_SR_RFNE_BITS)) {}
    uint16_t val = ssi_hw->dr0;

    return val;
}
