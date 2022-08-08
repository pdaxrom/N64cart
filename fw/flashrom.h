#ifndef __FLASHROM_H__
#define __FLASHROM_H__

#ifndef PARAM_ASSERTIONS_ENABLED_FLASH
#define PARAM_ASSERTIONS_ENABLED_FLASH 0
#endif

#define FLASH_PAGE_SIZE (1u << 8)
#define FLASH_SECTOR_SIZE (1u << 12)
#define FLASH_BLOCK_SIZE (1u << 16)

void xflash_do_cmd(const uint8_t *txbuf, uint8_t *rxbuf, size_t count);

void xflash_set_ea_reg(uint8_t addr);

uint8_t xflash_get_ea_reg(void);

void xflash_range_erase(uint32_t flash_offs, size_t count);

void xflash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count);

#endif
