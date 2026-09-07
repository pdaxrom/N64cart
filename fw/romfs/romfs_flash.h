#ifndef ROMFS_FLASH_H
#define ROMFS_FLASH_H

#include "romfs.h"

/* UINT32_MAX cannot be an aligned start and represents an invalid boundary. */
static inline uint32_t romfs_align_flash_start(uint32_t start)
{
    if (start > UINT32_MAX - (ROMFS_FLASH_START_ALIGNMENT - 1)) {
        return UINT32_MAX;
    }
    return (start + ROMFS_FLASH_START_ALIGNMENT - 1) & ~(ROMFS_FLASH_START_ALIGNMENT - 1);
}

/* Physical erase/program guard; metadata sectors are writable too. */
static inline bool romfs_flash_sector_in_range(uint32_t offset, uint32_t start, uint32_t size)
{
    return size >= ROMFS_FLASH_SECTOR &&
           (size & (ROMFS_FLASH_SECTOR - 1)) == 0 &&
           (start & (ROMFS_FLASH_START_ALIGNMENT - 1)) == 0 &&
           (offset & (ROMFS_FLASH_SECTOR - 1)) == 0 &&
           offset >= start && offset <= size - ROMFS_FLASH_SECTOR;
}

#endif
