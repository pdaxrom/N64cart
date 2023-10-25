#ifndef __FLASHROM_H__
#define __FLASHROM_H__

#include <stdint.h>

void flash_set_ea_reg(uint8_t addr);

uint8_t flash_get_ea_reg(void);

void flash_spi_mode(void);

bool flash_erase_sector(uint32_t addr);

bool flash_write_sector(uint32_t addr, uint8_t *buffer);

uint8_t flash_read8_0C(uint32_t addr);

uint16_t flash_read16_0C(uint32_t addr);

uint32_t flash_read32_0C(uint32_t addr);

uint8_t flash_get_status(void);

void flash_quad_mode(bool use_a32);

uint32_t flash_quad_read32_EB(uint32_t addr);

uint16_t flash_quad_read16_EB(uint32_t addr);

uint32_t flash_quad_read32_EC(uint32_t addr);

uint16_t flash_quad_read16_EC(uint32_t addr);

#endif
