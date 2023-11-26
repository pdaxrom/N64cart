#pragma once

#include <stdint.h>

void flash_cs_force(bool high);

void flash_spi_mode(void);

bool flash_erase_sector(uint32_t addr);

bool flash_write_sector(uint32_t addr, uint8_t *buffer);

bool flash_read_0C(uint32_t addr, uint8_t *buffer, uint32_t len);

uint8_t flash_read8_0C(uint32_t addr);

uint16_t flash_read16_0C(uint32_t addr);

uint32_t flash_read32_0C(uint32_t addr);

void flash_quad_mode(void);

uint32_t flash_quad_read32_EC(uint32_t addr);

uint16_t flash_quad_read16_EC(uint32_t addr);
