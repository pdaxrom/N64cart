/**
 * Copyright (c) 2022-2024 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ctype.h>
#include <libdragon.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../fw/romfs/romfs.h"
#include "ext/boot.h"
#include "ext/boot_io.h"
#include "ext/shell_utils.h"
#include "n64cart.h"
#include "../build/wy700font-regular.h"
#include "main.h"
#include "usb/usbd.h"

#include "syslog.h"
#include "md5.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_IMG_STATIC
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#include "stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

enum {
    STEP_LOGO = 0,
    STEP_ROMFS_INIT,
    STEP_LOAD_BACKGROUND,
    STEP_SAVE_GAMESAVE,
    //STEP_USB_INIT,
    STEP_FINISH
};

static const struct flash_chip flash_chip[] = {
    { 0xc2, 0x201b, 128, "MX66L1G45G" },
    { 0xef, 0x4020, 64, "W25Q512" },
    { 0xef, 0x4019, 32, "W25Q256" },
    { 0xef, 0x4018, 16, "W25Q128" },
    { 0xef, 0x4017, 8, "W25Q64" },
    { 0xef, 0x4016, 4, "W25Q32" },
    { 0xef, 0x4015, 2, "W25Q16" }
};

static const struct flash_chip *used_flash_chip = NULL;

static int scr_width;
static int scr_height;
static int scr_scale;

static char *files[128];
static int num_files = 0;
static int menu_sel = 0;

static uint8_t __attribute__((aligned(16))) save_data[131072];

static sprite_t *bg_img = NULL;

static int do_step = STEP_LOGO;

bool romfs_flash_sector_erase(uint32_t offset)
{
#ifdef DEBUG_FS
    syslog(LOG_DEBUG, "%s: offset %08X", __func__, offset);
#endif
    disable_interrupts();
    flash_mode(0);
    flash_erase_sector(offset);
    flash_mode(1);
    enable_interrupts();

    return true;
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
#ifdef DEBUG_FS
    syslog(LOG_DEBUG, "%s: offset %08X", __func__, offset);
#endif
    disable_interrupts();
    flash_mode(0);
    flash_write_sector(offset, buffer);
    flash_mode(1);
    enable_interrupts();

    return true;
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)
{
#ifdef DEBUG_FS
    syslog(LOG_DEBUG, "%s: offset %08lX, need %ld", __func__, offset, need);
#endif
    disable_interrupts();
    flash_mode(0);
    flash_read(offset, buffer, need);
    flash_mode(1);
    enable_interrupts();

    return true;
}

static int valign(const char *s)
{
    return (scr_width >> 1) - strlen(s) * 4 * scr_scale;
}

static void detect_flash_chip()
{
    uint8_t rxbuf[4];

    disable_interrupts();
    flash_mode(0);
    flash_do_cmd(0x9f, NULL, rxbuf, 4);
    flash_mode(1);
    enable_interrupts();

    syslog(LOG_INFO, "Flash jedec id %02X %02X %02X", rxbuf[0], rxbuf[1], rxbuf[2]);

    uint8_t mf = rxbuf[0];
    uint16_t id = (rxbuf[1] << 8) | rxbuf[2];

    used_flash_chip = NULL;
    for (int i = 0; i < sizeof(flash_chip) / sizeof(struct flash_chip); i++) {
        if (flash_chip[i].mf == mf && flash_chip[i].id == id) {
            used_flash_chip = &flash_chip[i];
            break;
        }
    }
}

const struct flash_chip *get_flash_info()
{
    return used_flash_chip;
}

static bool get_rom_name(char *name, int size)
{
    n64cart_sram_unlock();
    disable_interrupts();
    flash_mode(0);
    flash_read(((pi_io_read(N64CART_ROM_LOOKUP) >> 16) << 12) + 0x3b, (void *)name, 5);
    flash_mode(1);
    enable_interrupts();
    n64cart_sram_lock();

    for (int i = 0; i < 4; i++) {
        if (!isalnum((int)name[i])) {
            return false;
        }
    }

    snprintf(&name[4], size - 4, "-%d", name[4]);

    return true;
}

static void print_bytes(uint8_t bytes[16]) {
    char hex[33] = { 0 };
    for (int i = 0; i < 16; i++) {
        snprintf(&hex[i * 2], 3, "%02X", bytes[i]);
    }
    syslog(LOG_INFO, "file map md5: %s", hex);

    // if (strcmp(hex, "EA9E1832EFAB0CF7303273128A79DB5E")) {
    //     syslog(LOG_ERR, "ERROR MD5! (EA9E1832EFAB0CF7303273128A79DB5E)");
    //     while (true) {}
    // }
}

static void show_md5(uint8_t *buf, size_t size)
{
    md5_context ctx;
    md5_init(&ctx);
    md5_digest(&ctx, buf, size);

    uint8_t md5_actual[16] = {0};
    md5_output(&ctx, md5_actual);

    print_bytes(md5_actual);
}

int main(void)
{
    usbd_start();

    display_init(is_memory_expanded()? RESOLUTION_640x480 : RESOLUTION_320x240, DEPTH_32_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);

    if (is_memory_expanded()) {
        graphics_set_font_sprite((sprite_t *) wy700font_regular_sprite);
        scr_scale = 2;
    } else {
        scr_scale = 1;
    }

    scr_width = display_get_width();
    scr_height = display_get_height();

    joypad_init();

    dma_wait();

    io_write(N64CART_LED_CTRL, 0);

    detect_flash_chip();

    if (used_flash_chip) {
        syslog(LOG_INFO, "Flash chip: %s (%d MB)", used_flash_chip->name, used_flash_chip->rom_size);
    }

    syslog(LOG_INFO, "N64Cart fw size: %ld", n64cart_fw_size());

    static const char *txt_title_1 = "N64CART MANAGER";
    static const char *txt_title_2 = "(c) sashz /pdaXrom.org/, 2022-2024";
    static char txt_rom_info[128];
    static const char *txt_menu_info_1 = "[UP]/[DOWN]-Select, [L]/[R]-Page";
    static const char *txt_menu_info_2 = "[A]-Run, [C-L]-Delete";

    if (used_flash_chip) {
        snprintf(txt_rom_info, sizeof(txt_rom_info) - 1, "Flash chip: %s (%d MB)", used_flash_chip->name, used_flash_chip->rom_size);
    } else {
        strncpy(txt_rom_info, "Unknown flash chip", sizeof(txt_rom_info) - 1);
    }

    static const char *tv_type_str[] = { "PAL", "NTSC", "M-PAL" };
    static char txt_tv_type_msg[64];
    snprintf(txt_tv_type_msg, sizeof(txt_tv_type_msg), "%s mode", tv_type_str[get_tv_type()]);

    static display_context_t disp = 0;

    bool hide_menu = false;

    do_step = STEP_LOGO;

    /* Main loop test */
    while (1) {
        disp = display_get();

        /* Create Place for Text */
        char tStr[256];

        if (bg_img) {
            /* Logo */
            graphics_draw_sprite(disp, 0, 0, bg_img);
        } else {
            /* Fill the screen */
            graphics_fill_screen(disp, 0);
        }

        graphics_set_color(0xeeeeee00, 0x00000000);


        if (do_step == STEP_LOGO) {
            static int i = 0;
            if (++i < 30) {
                static char *logo_text = "pdaXrom";
                char text[strlen(logo_text) + 1];
                memset(text, 0, sizeof(text));

                strncpy(text, logo_text, (i + 1) / 2);
                graphics_draw_text(disp, valign(text), 120 * scr_scale, text);
                display_show(disp);

                continue;
            }

            do_step = STEP_ROMFS_INIT;
            display_show(disp);
            continue;
        }

        if (do_step == STEP_ROMFS_INIT) {
            static const char *save_data_txt = "ROM FS starting...";
            graphics_draw_text(disp, valign(save_data_txt), 120 * scr_scale, save_data_txt);
            display_show(disp);

            uint32_t flash_map_size, flash_list_size;
            romfs_get_buffers_sizes(used_flash_chip->rom_size * 1024 * 1024, &flash_map_size, &flash_list_size);

            static uint16_t *romfs_flash_map = NULL;
            static uint8_t *romfs_flash_list = NULL;

            if (!romfs_flash_map) {
                romfs_flash_map = malloc(flash_map_size);
            }

            if (!romfs_flash_list) {
                romfs_flash_list = malloc(flash_list_size);
            }

            uint32_t fw_size = n64cart_fw_size();
            syslog(LOG_INFO, "flash_map: %d, flash_list: %d, fw_size: %d", flash_map_size, flash_list_size, fw_size);
            syslog(LOG_INFO, "flash_map  ptr %p", romfs_flash_map);
            syslog(LOG_INFO, "flash_list ptr %p", romfs_flash_list);

            if (!romfs_start(fw_size, used_flash_chip->rom_size * 1024 * 1024, romfs_flash_map, romfs_flash_list)) {
                syslog(LOG_ERR, "Cannot start romfs!");
            } else {

                romfs_file file;

                num_files = 0;
                menu_sel = 0;

                syslog(LOG_INFO, "File list:");
                if (romfs_list(&file, true) == ROMFS_NOERR) {
                    do {
                        if (file.entry.attr.names.type > ROMFS_TYPE_FLASHMAP) {
                            files[num_files++] = strdup(file.entry.name);
                        }
                        syslog(LOG_INFO, "%s\t%ld\t%0X %4X", file.entry.name, file.entry.size, file.entry.attr.names.mode, file.entry.attr.names.type);
                    } while (romfs_list(&file, false) == ROMFS_NOERR);
                }
            }

            show_md5((uint8_t *)romfs_flash_map, flash_map_size);

            do_step = STEP_LOAD_BACKGROUND;
            continue;
        }

        if (do_step == STEP_LOAD_BACKGROUND) {
            romfs_file file;
            uint8_t romfs_flash_buffer[ROMFS_FLASH_SECTOR];

            static const char *save_data_txt = "Loading background...";
            graphics_draw_text(disp, valign(save_data_txt), 120 * scr_scale, save_data_txt);
            display_show(disp);

            if (romfs_open_file("background.jpg", &file, romfs_flash_buffer) == ROMFS_NOERR) {
                int ret;
                int pos = 0;

                int picture_data_length = file.entry.size;
                uint8_t *picture_data = malloc(picture_data_length);

                while (pos < picture_data_length && (ret = romfs_read_file(&picture_data[pos], 4096, &file)) > 0) {
                    pos += ret;
                }
                romfs_close_file(&file);
                syslog(LOG_INFO, "read image file %d bytes", pos);

                if (picture_data != NULL && picture_data[0] == 0xff && picture_data[1] == 0xd8) {
                    syslog(LOG_INFO, "User picture");

                    int w, h, channels;

                    stbi_uc *stbi_img = stbi_load_from_memory(picture_data, picture_data_length, &w, &h, &channels, 4);

                    if (w != scr_width || h != scr_height) {
                        stbi_uc *stbi_img_new = stbir_resize_uint8_linear(stbi_img, w, h, w * 4, NULL, scr_width, scr_height,
                                                                          scr_width * 4, STBIR_RGBA);
                        stbi_image_free(stbi_img);
                        stbi_img = stbi_img_new;
                        w = scr_width;
                        h = scr_height;
                    }

                    syslog(LOG_INFO, "w = %d, h = %d, c = %d", w, h, channels);

                    free(picture_data);

                    bg_img = malloc(sizeof(sprite_t) + w * h * 4);
                    bg_img->width = w;
                    bg_img->height = h;
                    bg_img->flags = FMT_RGBA32;
                    bg_img->hslices = 1;
                    bg_img->vslices = 1;

                    memmove(&bg_img->data[0], stbi_img, w * h * 4);

                    stbi_image_free(stbi_img);
                }
            }

            do_step = STEP_SAVE_GAMESAVE;
            continue;
        }

        if (do_step == STEP_SAVE_GAMESAVE) {
            char save_name[64] = { 0 };
            uint32_t save_addr = 0;
            int save_size = 0;

            do_step = STEP_FINISH;

            // copy save data from cartridge on reset button and set default save data configuration
            syslog(LOG_INFO, "reset type %d", sys_reset_type());
            n64cart_sram_unlock();
            if (sys_reset_type() == RESET_WARM) {
                syslog(LOG_INFO, "get eeprom data");

                save_addr = io_read(N64CART_RMRAM);
                save_size = io_read(N64CART_RMRAM + 4);
                for (int i = 0; i < sizeof(save_name); i += 4) {
                    *((uint32_t *) & save_name[i]) = io_read(N64CART_RMRAM + 8 + i);
                }

                if (save_addr && save_size && save_name[0]) {
                    //              dma_wait();
                    //              data_cache_hit_writeback_invalidate(save_data, sizeof(save_data));
                    //              dma_read(save_data, N64CART_EEPROM, sizeof(save_data));
                    for (int i = 0; i < save_size; i += 4) {
                        *((uint32_t *) & save_data[i]) = io_read(save_addr + i);
                    }
                }
            }
            io_write(N64CART_RMRAM, 0);
            n64cart_sram_lock();
            n64cart_eeprom_16kbit(true);

            if (save_addr && save_size && save_name[0]) {
                static const char *save_data_txt = "Write game save...";
                graphics_draw_text(disp, valign(save_data_txt), 120 * scr_scale, save_data_txt);
                display_show(disp);

                syslog(LOG_INFO, "save name: %s, pi_addr %08lX, size %d", save_name, save_addr, save_size);

                for (int i = 0; i < save_size; i++) {
                    if (save_data[i] != 0) {
                        romfs_file save_file;
                        uint8_t romfs_flash_buffer[ROMFS_FLASH_SECTOR];

                        romfs_delete(save_name);
                        if (save_size <= 2048) {
                            // eeprom byte swap
                            for (int i = 0; i < save_size; i += 2) {
                                uint8_t tmp = save_data[i];
                                save_data[i] = save_data[i + 1];
                                save_data[i + 1] = tmp;
                            }
                        } else {
                            // sram word swap
                            for (int i = 0; i < save_size; i += 4) {
                                uint8_t tmp = save_data[i];
                                save_data[i] = save_data[i + 3];
                                save_data[i + 3] = tmp;
                                tmp = save_data[i + 2];
                                save_data[i + 2] = save_data[i + 1];
                                save_data[i + 1] = tmp;
                            }
                        }

                        if (romfs_create_file(save_name, &save_file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, romfs_flash_buffer) == ROMFS_NOERR) {
                            int bwrite = 0;
                            int ret = 0;
                            while (save_size > 0) {
                                ret = romfs_write_file(&save_data[bwrite], (save_size > 4096) ? 4096 : save_size, &save_file);
                                if (!ret) {
                                    break;
                                }
                                save_size -= ret;
                                bwrite += ret;
                            }
                            romfs_close_file(&save_file);
                            if (!ret) {
                                syslog(LOG_ERR, "error write save file, delete");
                                romfs_delete(save_name);
                            }
                        }
                        syslog(LOG_INFO, "save file created");
                        break;
                    }
                }

                save_addr = 0;
                save_size = 0;
                save_name[0] = 0;
                continue;
            }
        }

        // if (do_step == STEP_USB_INIT) {
        //     static int count = 0;
        //     static const char *save_data_txt = "USB initialization...";
        //     graphics_draw_text(disp, valign(save_data_txt), 120 * scr_scale, save_data_txt);
        //     display_show(disp);

        //     if (count++ == 0) {
        //         usbd_start();
        //     }

        //     if (count < 20) {
        //         continue;
        //     }
        //     do_step = STEP_FINISH;
        //     continue;
        // }

        /* Scan for User input */
        joypad_poll();
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        //        joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);
        //      joypad_inputs_t inputs = joypad_get_inputs(JOYPAD_PORT_1);

        if (pressed.z) {
            hide_menu = !hide_menu;
        }

        if (hide_menu) {
            display_show(disp);
            continue;
        }

        /* Text */
        graphics_draw_text(disp, valign(txt_title_1), 10 * scr_scale, txt_title_1);
        graphics_draw_text(disp, valign(txt_title_2), 20 * scr_scale, txt_title_2);

        graphics_draw_text(disp, valign(txt_rom_info), 30 * scr_scale, txt_rom_info);
        graphics_draw_text(disp, valign(txt_tv_type_msg), 40 * scr_scale, txt_tv_type_msg);

        graphics_draw_text(disp, valign(txt_menu_info_1), 90 * scr_scale, txt_menu_info_1);
        graphics_draw_text(disp, valign(txt_menu_info_2), 100 * scr_scale, txt_menu_info_2);

        if (pressed.a) {
            romfs_file file;
            uint8_t romfs_flash_buffer[ROMFS_FLASH_SECTOR];

            graphics_draw_box(disp, 40 * scr_scale, 110 * scr_scale, (320 - 40 * 2) * scr_scale, 50 * scr_scale, 0x00000080);
            graphics_draw_box(disp, 45 * scr_scale, 115 * scr_scale, (320 - 45 * 2) * scr_scale, 40 * scr_scale, 0x77777780);

            if (romfs_open_file(files[menu_sel], &file, romfs_flash_buffer) == ROMFS_NOERR) {
                uint16_t rom_lookup[ROMFS_FLASH_SECTOR * 4];

                memset(rom_lookup, 0, sizeof(rom_lookup));
                uint32_t map_size = romfs_read_map_table(rom_lookup, sizeof(rom_lookup) / 2, &file);

                //syslog(LOG_INFO, "map size: %d (%08X)\n", map_size, map_size);

                n64cart_sram_unlock();
                for (int i = 0; i < map_size; i += 2) {
                    uint32_t data = (rom_lookup[i] << 16) | rom_lookup[i + 1];
                    //syslog(LOG_INFO, "%08X: %08X", i, data);
                    io_write(N64CART_ROM_LOOKUP + (i << 1), data);
                }
                n64cart_sram_lock();

                char save_name[64];
                int cic_id = 2;
                int save_type = 0;
                int rom_detected = 0;

                if (get_rom_name(save_name, sizeof(save_name))) {
                    rom_detected = get_cic_save(&save_name[1], &cic_id, &save_type);
                }

                syslog(LOG_INFO, "rom detected %d, cic_id %d, save_type %d", rom_detected, cic_id, save_type);

                if (rom_detected) {
                    syslog(LOG_INFO, "eeprom save name: %s", save_name);

                    int save_file_size = 0;
                    uint32_t pi_addr = 0;
                    switch (save_type) {
                    case 1:
                        strcat(save_name, ".sra");
                        save_file_size = 32768;
                        pi_addr = N64CART_SRAM;
                        break;
                    case 2:
                        strcat(save_name, ".sra");
                        save_file_size = 131072;
                        pi_addr = N64CART_SRAM;
                        break;
                    case 3:
                        strcat(save_name, ".eep");
                        save_file_size = 512;
                        n64cart_eeprom_16kbit(false);
                        pi_addr = N64CART_EEPROM;
                        break;
                    case 4:
                        strcat(save_name, ".eep");
                        save_file_size = 2048;
                        n64cart_eeprom_16kbit(true);
                        pi_addr = N64CART_EEPROM;
                        break;
                    case 5:
                        strcat(save_name, ".fla");
                        save_file_size = 131072;
                        pi_addr = N64CART_SRAM;
                        break;
                    default:
                        strcat(save_name, ".sav");
                        n64cart_eeprom_16kbit(true);
                    }

                    syslog(LOG_INFO, "save name: %s, pi_addr %08lX, size %d", save_name, pi_addr, save_file_size);

                    romfs_file save_file;

                    n64cart_sram_unlock();
                    io_write(N64CART_RMRAM, pi_addr);
                    io_write(N64CART_RMRAM + 4, save_file_size);
                    for (int i = 0; i < sizeof(save_name); i += 4) {
                        io_write(N64CART_RMRAM + 8 + i, *((uint32_t *) & save_name[i]));
                    }
                    n64cart_sram_lock();

                    if (romfs_open_file(save_name, &save_file, romfs_flash_buffer) == ROMFS_NOERR) {
                        syslog(LOG_INFO, "Load save data");
                        int rbytes = 0;
                        while (rbytes < save_file_size) {
                            int ret = romfs_read_file(&save_data[rbytes], 4096, &save_file);
                            if (!ret) {
                                break;
                            }
                            rbytes += ret;
                        }

                        syslog(LOG_INFO, "read %d bytes", rbytes);

                        if (rbytes <= 2048) {
                            // eeprom byte swap
                            for (int i = 0; i < rbytes; i += 2) {
                                uint8_t tmp = save_data[i];
                                save_data[i] = save_data[i + 1];
                                save_data[i + 1] = tmp;
                            }
                        } else {
                            // sram word swap
                            for (int i = 0; i < rbytes; i += 4) {
                                uint8_t tmp = save_data[i];
                                save_data[i] = save_data[i + 3];
                                save_data[i + 3] = tmp;
                                tmp = save_data[i + 2];
                                save_data[i + 2] = save_data[i + 1];
                                save_data[i + 1] = tmp;
                            }
                        }

                        n64cart_sram_unlock();
                        // dma_wait();
                        // data_cache_hit_writeback(save_data, sizeof(save_data));
                        // dma_write(save_data, N64CART_EEPROM, sizeof(save_data));
                        for (int i = 0; i < save_file_size; i += 4) {
                            io_write(pi_addr + i, *((uint32_t *) & save_data[i]));
                        }
                        n64cart_sram_lock();
                    } else {
                        syslog(LOG_INFO, "No valid eeprom dump, clean eeprom data");
                        n64cart_sram_unlock();
                        for (int i = 0; i < save_file_size; i += 4) {
                            io_write(pi_addr + i, 0);
                        }
                        n64cart_sram_lock();
                    }
                } else {
                    n64cart_sram_unlock();
                    io_write(N64CART_RMRAM, 0);
                    n64cart_sram_lock();
                }

                OS_INFO->tv_type = get_tv_type();
                OS_INFO->reset_type = RESET_COLD;
                OS_INFO->mem_size = get_memory_size();

                syslog(LOG_INFO, "cic_id %d", cic_id);
                syslog(LOG_INFO, "tv_type %ld", OS_INFO->tv_type);
                syslog(LOG_INFO, "device_type %ld", OS_INFO->device_type);
                syslog(LOG_INFO, "device_base %8lX", OS_INFO->device_base);
                syslog(LOG_INFO, "reset_type %ld", OS_INFO->reset_type);
                syslog(LOG_INFO, "cic_id %ld", OS_INFO->cic_id);
                syslog(LOG_INFO, "version %ld", OS_INFO->version);
                syslog(LOG_INFO, "mem_size %ld", OS_INFO->mem_size);

                joypad_close();
                display_close();


                //                boot_params_t params;
                //                params.device_type = BOOT_DEVICE_TYPE_ROM;
                //                params.tv_type = get_tv_type(); //BOOT_TV_TYPE_NTSC;
                //                params.detect_cic_seed = true;

                usbd_finish();

                disable_interrupts();

                //                boot(&params);

                //                set_force_tv(get_tv_type());
                simulate_boot(cic_id, 2);
            } else {
                static const char *fopen_error_1 = "File open error!";
                graphics_draw_text(disp, valign(fopen_error_1), 120 * scr_scale, fopen_error_1);
                static const char *fopen_error_2 = "Press (B) to continue";
                graphics_draw_text(disp, valign(fopen_error_2), 130 * scr_scale, fopen_error_2);
            }

            display_show(disp);

            while (1) {
                joypad_poll();
                joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

                if (pressed.b) {
                    break;
                }
            }

            continue;
        } else if (pressed.c_left) {
            static const char *fopen_error_1 = "Delete file? (A) Yes (B) No";
            graphics_draw_text(disp, valign(fopen_error_1), 120 * scr_scale, fopen_error_1);

            display_show(disp);

            while (1) {
                joypad_poll();
                joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

                if (pressed.a) {
                    romfs_delete(files[menu_sel]);
                    free(files[menu_sel]);
                    num_files--;
                    for (int i = menu_sel; i < num_files; i++) {
                        files[i] = files[i + 1];
                    }
                    if (menu_sel >= num_files) {
                        menu_sel = num_files - 1;
                    }
                    break;
                } else if (pressed.b) {
                    break;
                }
            }

            continue;
        }

        int menu_page_size = 10;

        if (menu_sel > 0 && pressed.d_up) {
            menu_sel--;
        } else if (menu_sel < (num_files - 1) && pressed.d_down) {
            menu_sel++;
        } else if (menu_sel >= menu_page_size && pressed.l) {
            menu_sel -= menu_page_size;
        } else if ((menu_sel - menu_sel % menu_page_size) + menu_page_size < num_files && pressed.r) {
            menu_sel += menu_page_size;
            menu_sel = (menu_sel < num_files) ? menu_sel : (num_files - 1);
        }

        int first_file = menu_sel - menu_sel % menu_page_size;
        int total_files_to_show = first_file + menu_page_size;
        total_files_to_show = (total_files_to_show > num_files) ? num_files : total_files_to_show;

        for (int i = first_file; i < total_files_to_show; i++) {
            if (i == menu_sel) {
                sprintf(tStr, "%02d: *%.24s", i, files[i]);
            } else {
                sprintf(tStr, "%02d:  %.24s", i, files[i]);
            }
            graphics_draw_text(disp, 40 * scr_scale, (120 + (i - first_file) * 10) * scr_scale, tStr);
        }

        if (pressed.start) {
#ifdef DISABLE_RGB_LED
            static int led_on = 0;
            led_on = !led_on;
            if (led_on) {
                syslog(LOG_INFO, "led on");
            } else {
                syslog(LOG_INGO, "led off");
            }
            io_write(N64CART_LED_CTRL, led_on);
#else
            static int led_cnt = 0;
            static uint32_t led_colors[] = { 0xff0000, 0x00ff00, 0x0000ff, 0x000000 };
            syslog(LOG_INFO, "led");
            io_write(N64CART_LED_CTRL, led_colors[led_cnt++]);
            led_cnt &= 0x03;
#endif
        }

        display_show(disp);
    }
}