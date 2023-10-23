#ifndef __FLASHROM_H__
#define __FLASHROM_H__

#include <stdint.h>

void flash_set_ea_reg(uint8_t addr);

uint8_t flash_get_ea_reg(void);

void flash_spi_mode(void);

uint16_t flash_read16_0C(uint32_t addr);

uint32_t flash_read32_0C(uint32_t addr);

#endif
