/**
 * Copyright (c) 2022 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <stdint.h>
#include <libdragon.h>
#include <stdlib.h>

#include "n64cart.h"
#include "../fw/romfs/romfs.h"

#include "ext/boot.h"
#include "ext/boot_io.h"

#include "wy700font-regular.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_IMG_STATIC
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

struct flash_chip {
    uint8_t mf;
    uint16_t id;
    uint8_t rom_pages;
    uint8_t rom_size;
    const char *name;
};

static const struct flash_chip flash_chip[] = {
    { 0xef, 0x4020, 4, 16, "W25Q512" },
    { 0xef, 0x4019, 2, 16, "W25Q256" },
    { 0xef, 0x4018, 1, 16, "W25Q128" },
    { 0xef, 0x4017, 1, 8 , "W25Q64"  },
    { 0xef, 0x4016, 1, 4 , "W25Q32"  },
    { 0xef, 0x4015, 1, 2 , "W25Q16"  }
};

static const struct flash_chip *used_flash_chip = NULL;

static int scr_width;
static int scr_height;
static int scr_scale;

static char *files[128];
static int num_files = 0;
static int menu_sel = 0;

static uint8_t *picture_data = NULL;
static int picture_data_length;

bool romfs_flash_sector_erase(uint32_t offset)
{
#ifdef DEBUG_FS
    printf("%s: offset %08X\n", __func__, offset);
#endif
    flash_mode(0);
    flash_erase_sector(offset);
    flash_mode(1);

    return true;
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
#ifdef DEBUG_FS
    printf("%s: offset %08X\n", __func__, offset);
#endif
    flash_mode(0);
    flash_write_sector(offset, buffer);
    flash_mode(1);

    return true;
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)
{
#ifdef DEBUG_FS
    char tmp[256];
    snprintf(tmp, sizeof(tmp) - 1, "%s: offset %08lX, need %ld\n", __func__, offset, need);
    n64cart_uart_puts(tmp);
#endif
    flash_mode(0);
    flash_read_0C(offset, buffer, need);
    flash_mode(1);

    return true;
}

static int valign(const char *s)
{
    return (scr_width >> 1) - strlen(s) * 4 * scr_scale;
}

static void detect_flash_chip()
{
    uint8_t txbuf[4];
    uint8_t rxbuf[4];

    flash_mode(0);
    txbuf[0] = 0x9f;
    flash_do_cmd(txbuf, rxbuf, 4, 0);
    flash_mode(1);

    char tmp[256];
    snprintf(tmp, sizeof(tmp) - 1, "Flash jedec id %02X %02X %02X\n", rxbuf[1], rxbuf[2], rxbuf[3]);
    n64cart_uart_puts(tmp);

    uint8_t mf = rxbuf[1];
    uint16_t id = (rxbuf[2] << 8) | rxbuf[3];

    used_flash_chip = NULL;
    for (int i = 0; i < sizeof(flash_chip) / sizeof(struct flash_chip); i++) {
	if (flash_chip[i].mf == mf && flash_chip[i].id == id) {
	    used_flash_chip = &flash_chip[i];
	    break;
	}
    }
}

int main(void)
{
    display_init(is_memory_expanded() ? RESOLUTION_640x480 : RESOLUTION_320x240, DEPTH_32_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);

    if (is_memory_expanded()) {
	graphics_set_font_sprite((sprite_t *)wy700font_regular_sprite);
	scr_scale = 2;
    } else {
	scr_scale = 1;
    }

    scr_width = display_get_width();
    scr_height = display_get_height();

    joypad_init();

    dma_wait();

    detect_flash_chip();

    if (used_flash_chip) {
	char tmp[256];
	snprintf(tmp, sizeof(tmp) - 1, "Flash chip: %s (%d MB)\n", used_flash_chip->name, used_flash_chip->rom_pages * used_flash_chip->rom_size);
	n64cart_uart_puts(tmp);
    }

    {
	char tmp[256];
	snprintf(tmp, sizeof(tmp) - 1, "N64Cart fw size: %ld\n", n64cart_fw_size());
	n64cart_uart_puts(tmp);
    }

    if (!romfs_start(n64cart_fw_size(), used_flash_chip->rom_pages * used_flash_chip->rom_size * 1024 * 1024)) {
	n64cart_uart_puts("Cannot start romfs!\n");
    } else {
	char tmp[256];
	romfs_file file;
	n64cart_uart_puts("File list:\n");
	if (romfs_list(&file, true) == ROMFS_NOERR) {
	    do {
		if (file.entry.attr.names.type > ROMFS_TYPE_FLASHMAP) {
		    files[num_files++] = strdup(file.entry.name);
		}
		snprintf(tmp, sizeof(tmp) - 1, "%s\t%ld\t%0X %4X\n", file.entry.name, file.entry.size, file.entry.attr.names.mode, file.entry.attr.names.type);
		n64cart_uart_puts(tmp);
	    } while (romfs_list(&file, false) == ROMFS_NOERR);
	}

	if (romfs_open_file("background.jpg", &file, NULL) == ROMFS_NOERR) {
            int ret;
            int pos = 0;
            picture_data_length = file.entry.size;
            picture_data = malloc(picture_data_length);

            while (pos < picture_data_length && (ret = romfs_read_file(&picture_data[pos], 4096, &file)) > 0) {
		pos += ret;
            }
            romfs_close_file(&file);
	    snprintf(tmp, sizeof(tmp) - 1, "read image file %d bytes\n", pos);
	    n64cart_uart_puts(tmp);

/*
	    if (romfs_create_file("xxx.jpg", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, NULL) == ROMFS_NOERR) {
		int len = pos;
		pos = 0;
        	while (len > 0) {
                    if (romfs_write_file(&picture_data[pos], (len < 4096) ? len : 4096, &file) == 0) {
                        break;
                    }
                    if (len < 4096) {
                	break;
                    }
		    len -= 4096;
		    pos += 4096;
        	}
        	romfs_close_file(&file);
	    }
 */
	}
    }

    sprite_t *img = NULL;

    if (picture_data != NULL && picture_data[0] == 0xff && picture_data[1] == 0xd8) {
	n64cart_uart_puts("User picture\n");

	int w, h, channels;

	stbi_uc *stbi_img = stbi_load_from_memory(picture_data, picture_data_length, &w, &h, &channels, 4);

	if (w != scr_width || h != scr_height) {
	    stbi_uc *stbi_img_new = stbir_resize_uint8_linear(stbi_img, w, h, w * 4, NULL, scr_width, scr_height, scr_width * 4, STBIR_RGBA);
	    stbi_image_free(stbi_img);
	    stbi_img = stbi_img_new;
	    w = scr_width;
	    h = scr_height;
	}

	char tmp[256];
	snprintf(tmp, sizeof(tmp) - 1, "w = %d, h = %d, c = %d\n", w, h, channels);
	n64cart_uart_puts(tmp);

	free(picture_data);

	img = malloc(sizeof(sprite_t) + w * h * 4);
	img->width = w;
	img->height = h;
	img->flags = FMT_RGBA32;
	img->hslices = 1;
	img->vslices = 1;

	memmove(&img->data[0], stbi_img, w * h * 4);

	stbi_image_free(stbi_img);
    }

    static const char *txt_title_1 = "N64CART MANAGER";
    static const char *txt_title_2 = "(c) sashz /pdaXrom.org/, 2022-2023";
    static char txt_rom_info[128];
    static const char *txt_menu_info_1 = "[UP]/[DOWN]-Select, [L]/[R]-Page";
    static const char *txt_menu_info_2 = "[A]-Run, [X]-Delete";

    if (used_flash_chip) {
	snprintf(txt_rom_info, sizeof(txt_rom_info) - 1, "Flash chip: %s (%d MB)", used_flash_chip->name, used_flash_chip->rom_pages * used_flash_chip->rom_size);
    } else {
	strncpy(txt_rom_info, "Unknown flash chip", sizeof(txt_rom_info) - 1);
    }

    static display_context_t disp = 0;

    bool hide_menu = false;

    /* Main loop test */
    while(1) 
    {
	disp = display_get();

        /* Fill the screen */
        graphics_fill_screen( disp, 0 );
       
	/* Create Place for Text */
	char tStr[256];

	if (img) {
	    /* Logo */
	    graphics_draw_sprite(disp, 0, 0, img);
	}

	graphics_set_color(0xeeeeee00, 0x00000000);

        /* Scan for User input */
	joypad_poll();
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
//        joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);
//	joypad_inputs_t inputs = joypad_get_inputs(JOYPAD_PORT_1);

	if (pressed.z) {
	    hide_menu = !hide_menu;
	}

	if (hide_menu) {
	    display_show(disp);
	    continue;
	}

        /* Text */
        graphics_draw_text( disp, valign(txt_title_1), 10 * scr_scale, txt_title_1);
        graphics_draw_text( disp, valign(txt_title_2), 20 * scr_scale, txt_title_2);

	graphics_draw_text(disp, valign(txt_rom_info), 30 * scr_scale, txt_rom_info);

	graphics_draw_text(disp, valign(txt_menu_info_1), 90 * scr_scale, txt_menu_info_1);
	graphics_draw_text(disp, valign(txt_menu_info_2), 100 * scr_scale, txt_menu_info_2);

	if (pressed.a) {
	    romfs_file file;

	    graphics_draw_box(disp, 40 * scr_scale, 110 * scr_scale, (320 - 40 * 2) * scr_scale, 50 * scr_scale, 0x00000080);
	    graphics_draw_box(disp, 45 * scr_scale, 115 * scr_scale, (320 - 45 * 2) * scr_scale, 40 * scr_scale, 0x77777780);

	    if (romfs_open_file(files[menu_sel], &file, NULL) == ROMFS_NOERR) {
		uint16_t rom_lookup[ROMFS_FLASH_SECTOR * 4 * 2];
		memset(rom_lookup, 0, sizeof(rom_lookup));
		romfs_read_map_table(rom_lookup, sizeof(rom_lookup) / 2, &file);

		n64cart_sram_unlock();
		for (int i = 0; i < sizeof(rom_lookup) / 4; i += 2) {
		    uint32_t data = (rom_lookup[i] << 16) | rom_lookup[i + 1];
		    io_write(N64CART_ROM_LOOKUP + (i << 1), data);
		}
		n64cart_sram_lock();

		sprintf(tStr, "tv_type %ld\n", OS_INFO->tv_type);
		n64cart_uart_puts(tStr);
		sprintf(tStr, "device_type %ld\n", OS_INFO->device_type);
		n64cart_uart_puts(tStr);
		sprintf(tStr, "device_base %8lX\n", OS_INFO->device_base);
		n64cart_uart_puts(tStr);
		sprintf(tStr, "reset_type %ld\n", OS_INFO->reset_type);
		n64cart_uart_puts(tStr);
		sprintf(tStr, "cic_id %ld\n", OS_INFO->cic_id);
		n64cart_uart_puts(tStr);
		sprintf(tStr, "version %ld\n", OS_INFO->version);
		n64cart_uart_puts(tStr);
		sprintf(tStr, "mem_size %ld\n", OS_INFO->mem_size);
		n64cart_uart_puts(tStr);


		joypad_close();
		display_close();

		boot_params_t params;
		params.device_type = BOOT_DEVICE_TYPE_ROM;
		params.tv_type = BOOT_TV_TYPE_NTSC;
		params.detect_cic_seed = true;

		disable_interrupts();

		boot(&params);
	    } else {
		static const char *fopen_error_1 = "File open error!";
		graphics_draw_text(disp, valign(fopen_error_1), 120 * scr_scale, fopen_error_1);
		static const char *fopen_error_2 = "Press (B) to continue";
		graphics_draw_text( disp, valign(fopen_error_2), 130 * scr_scale,  fopen_error_2);
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
		sprintf(tStr, "%02d: *%s", i, files[i]);
	    } else {
		sprintf(tStr, "%02d:  %s", i, files[i]);
	    }
	    graphics_draw_text(disp, 40 * scr_scale, (120 + (i - first_file) * 10) * scr_scale, tStr);
	}

	if (pressed.start) {
	    static int led_on = 0;
	    led_on = !led_on;
	    if (led_on) {
		n64cart_uart_puts("led on\n");
	    } else {
		n64cart_uart_puts("led off\n");
	    }
	    io_write(N64CART_LED_CTRL, led_on);
	}

        display_show(disp);
    }
}
