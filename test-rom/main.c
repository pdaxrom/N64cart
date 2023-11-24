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
#include "jpeg/gba-jpeg-decode.h"

#include "n64cart.h"
#include "../fw/romfs/romfs.h"

#ifndef USE_FILESYSTEM
#include "image.h"
#endif

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

static const int scr_width = 320;
static const int scr_height = 240;

static char *files[128];
static int num_files = 0;
static int menu_sel = 0;

static uint8_t __attribute__((aligned(16))) picture_data[64 * 1024];

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

static int valign(char *s)
{
    return (scr_width >> 1) - strlen(s) * 4;
}

static void detect_flash_chip()
{
    uint8_t txbuf[4];
    uint8_t rxbuf[4];

    txbuf[0] = 0x9f;

    flash_mode(0);
    flash_do_cmd(txbuf, rxbuf, 4, 0);
    flash_mode(1);

//	char tmp[256];
//	snprintf(tmp, sizeof(tmp) - 1, "Flash jedec id %02X %02X %02X\n", rxbuf[1], rxbuf[2], rxbuf[3]);
//	n64cart_uart_puts(tmp);

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
    display_init(RESOLUTION_320x240, DEPTH_32_BPP, 2, GAMMA_NONE, ANTIALIAS_RESAMPLE);

    controller_init();

    dma_wait();

    detect_flash_chip();

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
            while (pos < sizeof(picture_data) && (ret = romfs_read_file(&picture_data[pos], 4096, &file)) > 0) {
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

    uint8_t *img_jpeg;

    if (picture_data[0] == 0xff && picture_data[1] == 0xd8) {
	img_jpeg = picture_data;
	n64cart_uart_puts("User picture\n");
    } else {
	n64cart_uart_puts("Default picture\n");
#ifdef USE_FILESYSTEM   
    /* Initialize Filesystem */
	dfs_init(DFS_DEFAULT_LOCATION);
    
	int fp = dfs_open("/default.jpg");
	img_jpeg = malloc(dfs_size(fp));
	dfs_read(img_jpeg, 1, dfs_size(fp), fp);
	dfs_close(fp);
#else
	img_jpeg = (uint8_t *) bg_image_jpg;
#endif
    }

    sprite_t *img = malloc(sizeof(sprite_t) + scr_width * scr_height * 4);
    img->width = scr_width;
    img->height = scr_height;
    img->bitdepth = 4;
    img->format = 0;
    img->hslices = 1;
    img->vslices = 1;

    JPEG_DecompressImage(img_jpeg, (JPEG_OUTPUT_TYPE *)&img->data[0], scr_width, scr_height);

    /* define two variables */
    int x = 0;
    int y = 0;

    graphics_set_color(0xeeeeee00, 0x00000000);

    /* Main loop test */
    while(1) 
    {
        static display_context_t disp = 0;

        /* Grab a render buffer */
        while( !(disp = display_lock()) );
        
        /* Fill the screen */
        graphics_fill_screen( disp, 0 );
       
	/* Create Place for Text */
	char tStr[256];

	/* Logo */
	graphics_draw_sprite_trans(disp, x, y, img);

        /* Text */
	strcpy(tStr, "N64CART TEST");
        graphics_draw_text( disp, valign(tStr), 10, tStr);

	strcpy(tStr, "(c) sashz /pdaXrom.org/, 2022-2023");
        graphics_draw_text( disp, valign(tStr), 20, tStr);

        /* Scan for User input */
        controller_scan();
        struct controller_data keys = get_keys_down();

	sprintf(tStr, "A %d B %d Z %d Start %d", keys.c[0].A, keys.c[0].B, keys.c[0].Z, keys.c[0].start);
	graphics_draw_text( disp, 10, 40, tStr );
	sprintf(tStr, "D-U %d D-D %d D-L %d D-R %d", keys.c[0].up, keys.c[0].down, keys.c[0].left, keys.c[0].right);
	graphics_draw_text( disp, 10, 50, tStr );

	sprintf(tStr, "L %d R %d C-U %d C-D %d C-L %d C-R %d", keys.c[0].L, keys.c[0].R, keys.c[0].C_up, keys.c[0].C_down, keys.c[0].C_left, keys.c[0].C_right);
	graphics_draw_text( disp, 10, 60, tStr );

	sprintf(tStr, "X %d Y %d", keys.c[0].x, keys.c[0].y);
	graphics_draw_text( disp, 10, 70, tStr );

	if (used_flash_chip) {
	    sprintf(tStr, "Flash chip: %s (%d MB)", used_flash_chip->name, used_flash_chip->rom_pages * used_flash_chip->rom_size);
	} else {
	    sprintf(tStr, "Unknown flash chip");
	}
	graphics_draw_text( disp, valign(tStr), 90, tStr );

	strcpy(tStr, "Press [UP]/[DOWN] to select");
	graphics_draw_text( disp, valign(tStr), 100, tStr );

	if (menu_sel > 0 && keys.c[0].up) {
	    menu_sel--;
	} else if (menu_sel < (num_files - 1) && keys.c[0].down) {
	    menu_sel++;
	} else if (keys.c[0].A) {
	    romfs_file file;

	    graphics_draw_box(disp, 40, 110, 320 - 40 * 2, 50, 0x00000080);
	    graphics_draw_box(disp, 45, 110 + 5, 320 - 45 * 2, 40, 0x77777780);

	    if (romfs_open_file(files[menu_sel], &file, NULL) == ROMFS_NOERR) {
		uint16_t rom_lookup[ROMFS_FLASH_SECTOR * 4 * 2];
		romfs_read_map_table(rom_lookup, sizeof(rom_lookup) / 2, &file);

		n64cart_sram_unlock();
		for (int i = 0; i < sizeof(rom_lookup) / 4; i += 2) {
		    uint32_t data = (rom_lookup[i] << 16) | rom_lookup[i + 1];
		    io_write(N64CART_ROM_LOOKUP + (i << 1), data);
		}
		n64cart_sram_lock();

		sprintf(tStr, "ROM: %s", files[menu_sel]);
		graphics_draw_text( disp, valign(tStr), 120, tStr );
		strcpy(tStr, "Press [RESET] to start");
		graphics_draw_text( disp, valign(tStr), 130,  tStr);
		strcpy(tStr, "or (B) to continue");
		graphics_draw_text( disp, valign(tStr), 140,  tStr);
	    } else {
		sprintf(tStr, "File open error!");
		graphics_draw_text( disp, valign(tStr), 120, tStr );
		strcpy(tStr, "Press (B) to continue");
		graphics_draw_text( disp, valign(tStr), 130,  tStr);
	    }

	    display_show(disp);

	    while (1) {
		controller_scan();
		keys = get_keys_down();

		if (keys.c[0].B) {
		    break;
		}
	    }

	    continue;
	}

	for (int i = 0; i < num_files; i++) {
	    if (i == menu_sel) {
		sprintf(tStr, "%02d: *%s", i, files[i]);
	    } else {
		sprintf(tStr, "%02d:  %s", i, files[i]);
	    }
	    graphics_draw_text(disp, 40, 120 + i * 10, tStr);
	}

	if (keys.c[0].start) {
	    static int led_on = 0;
	    led_on = !led_on;
	    if (led_on) {
		n64cart_uart_puts("led on\n");
	    } else {
		n64cart_uart_puts("led off\n");
	    }
	    io_write(N64CART_LED_CTRL, led_on);
	}

        /* Update Display */
        display_show(disp);
    }
}
