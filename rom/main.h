#pragma once

#include <stdint.h>

struct flash_chip {
    uint8_t mf;
    uint16_t id;
    uint8_t rom_pages;
    uint8_t rom_size;
    const char *name;
};

const struct flash_chip *get_flash_info();
