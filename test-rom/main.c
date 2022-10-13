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

#ifndef USE_FILESYSTEM
#include "image.h"
#endif

static const int scr_width = 320;
static const int scr_height = 240;

static uint8_t __attribute__((aligned(16))) picture_data[64 * 1024];

static int valign(char *s)
{
    return (scr_width >> 1) - strlen(s) * 4;
}

int main(void)
{
    display_init(RESOLUTION_320x240, DEPTH_32_BPP, 2, GAMMA_NONE, ANTIALIAS_RESAMPLE);

    controller_init();

    dma_wait();
    data_cache_hit_writeback_invalidate(picture_data, sizeof(picture_data));
    dma_read(picture_data, 0x1fd80000, sizeof(picture_data));

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

    int rom_pages = n64cart_get_rom_pages();

    int selected_rom = -1;

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

	strcpy(tStr, "(c) sashz /pdaXrom.org/, 2022");
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

	sprintf(tStr, "Available ROMs: %d", rom_pages);
	graphics_draw_text( disp, valign(tStr), 90, tStr );

	strcpy(tStr, "Press [UP]/[DOWN] to select");
	graphics_draw_text( disp, valign(tStr), 100, tStr );

	if (selected_rom < (rom_pages - 1) && keys.c[0].up) {
	    selected_rom++;
	}

	if (keys.c[0].down) {
	    if (selected_rom > 0) {
		selected_rom--;
	    } else {
		selected_rom = 0;
	    }
	}

	if (selected_rom != -1) {
	    n64cart_set_rom_page(selected_rom);
	    sprintf(tStr, "Selected ROM: %d", selected_rom);
	    graphics_draw_text( disp, valign(tStr), 120, tStr );
	    strcpy(tStr, "Press [RESET] to start");
	    graphics_draw_text( disp, valign(tStr), 130,  tStr);
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
