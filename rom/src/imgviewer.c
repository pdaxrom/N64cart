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
#include "syslog.h"
#include "imgviewer.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_IMG_STATIC
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#include "stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

static int scr_width, scr_height, scr_scale;

static int valign(const char *s)
{
    return (scr_width >> 1) - strlen(s) * 4 * scr_scale;
}

sprite_t *image_load(char *name, int screen_w, int screen_h)
{
    sprite_t *image = NULL;
    uint8_t romfs_flash_buffer[ROMFS_FLASH_SECTOR];
    romfs_file file;

    if (romfs_open_file(name, &file, romfs_flash_buffer) == ROMFS_NOERR) {
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

            syslog(LOG_INFO, "image w = %d, h = %d, c = %d", w, h, channels);

            if (w != screen_w || h != screen_h) {
                stbi_uc *stbi_img_new = stbir_resize_uint8_linear(stbi_img, w, h, w * 4, NULL, screen_w, screen_h,
                                                                  screen_w * 4, STBIR_RGBA);
                stbi_image_free(stbi_img);
                stbi_img = stbi_img_new;
                w = screen_w;
                h = screen_h;
            }

            syslog(LOG_INFO, "resized w = %d, h = %d, c = %d", w, h, channels);

            free(picture_data);

            image = malloc(sizeof(sprite_t) + w * h * 4);
            image->width = w;
            image->height = h;
            image->flags = FMT_RGBA32;
            image->hslices = 1;
            image->vslices = 1;

            memmove(&image->data[0], stbi_img, w * h * 4);

            stbi_image_free(stbi_img);
        }
    }

    return image;
}

void image_view(char *name, int screen_w, int screen_h, int screen_scale)
{
    static display_context_t disp = 0;

    scr_width = screen_w;
    scr_height = screen_h;
    scr_scale = screen_scale;

    while (true) {
        disp = display_get();

        sprite_t *image = image_load(name, screen_w, screen_h);

        if (image) {
            graphics_draw_sprite(disp, 0, 0, image);
        } else {
            static const char *fopen_error_1 = "Can't open image file!";
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

        if (image) {
            free(image);
        }

        break;
    }
}
