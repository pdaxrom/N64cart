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
#include <errno.h>

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

#define ROMFS_POSIX_PREFIX "romfs:/"
#define ROMFS_PATH_MAX 256

static int scr_width, scr_height, scr_scale;

static void build_romfs_prefixed_path(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    if (!path || path[0] == '\0') {
        snprintf(out, out_size, "%s", ROMFS_POSIX_PREFIX);
    } else if (path[0] == '/') {
        snprintf(out, out_size, "romfs:%s", path);
    } else {
        snprintf(out, out_size, ROMFS_POSIX_PREFIX "%s", path);
    }
}

static int valign(const char *s)
{
    return (scr_width >> 1) - strlen(s) * 4 * scr_scale;
}

sprite_t *image_load(char *name, int screen_w, int screen_h)
{
    sprite_t *image = NULL;
    char path[ROMFS_PATH_MAX + 8];
    build_romfs_prefixed_path(name, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        syslog(LOG_ERR, "cannot open image %s (errno %d)", path, errno);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }

    long length = ftell(fp);
    if (length < 0) {
        fclose(fp);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    uint8_t *picture_data = malloc((size_t)length);
    if (!picture_data) {
        fclose(fp);
        return NULL;
    }

    size_t read = fread(picture_data, 1, (size_t)length, fp);
    fclose(fp);
    if (read != (size_t)length) {
        free(picture_data);
        return NULL;
    }

    syslog(LOG_INFO, "read image file %ld bytes", length);

    if (picture_data[0] == 0xff && picture_data[1] == 0xd8) {
        syslog(LOG_INFO, "User picture");

        int w, h, channels;

        stbi_uc *stbi_img = stbi_load_from_memory(picture_data, (int)length, &w, &h, &channels, 4);

        free(picture_data);

        if (stbi_img) {
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

            image = malloc(sizeof(sprite_t) + w * h * 4);
            if (image) {
                image->width = w;
                image->height = h;
                image->flags = FMT_RGBA32;
                image->hslices = 1;
                image->vslices = 1;

                memmove(&image->data[0], stbi_img, w * h * 4);
            }
            stbi_image_free(stbi_img);
        }
    } else {
        free(picture_data);
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
            graphics_draw_box(disp, 40 * scr_scale, 110 * scr_scale, (320 - 40 * 2) * scr_scale, 50 * scr_scale, 0x00000080);
            graphics_draw_box(disp, 45 * scr_scale, 115 * scr_scale, (320 - 45 * 2) * scr_scale, 40 * scr_scale, 0x77777780);
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
