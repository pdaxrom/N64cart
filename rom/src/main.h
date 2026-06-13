#pragma once

#include <stdbool.h>
#include <stdint.h>

struct flash_chip {
    uint8_t mf;
    uint16_t id;
    uint8_t rom_size;
    const char *name;
};

const struct flash_chip *get_flash_info();
void n64cart_set_usb_display_mode(bool active);
void n64cart_note_usb_romfs_modified(void);
