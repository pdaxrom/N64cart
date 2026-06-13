/**
 * Copyright (c) 2022-2026 sashz /pdaXrom.org/
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

static void *image_stbi_malloc(size_t size);
static void *image_stbi_realloc(void *ptr, size_t size);
static void image_stbi_free(void *ptr);

#define STBI_MALLOC(sz) image_stbi_malloc(sz)
#define STBI_REALLOC(p, sz) image_stbi_realloc(p, sz)
#define STBI_FREE(p) image_stbi_free(p)
#define STBIR_MALLOC(size, user_data) ((void)(user_data), image_stbi_malloc(size))
#define STBIR_FREE(ptr, user_data) ((void)(user_data), image_stbi_free(ptr))

#define STB_IMAGE_IMPLEMENTATION
#define STBI_IMG_STATIC
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#include "stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

#define ROMFS_PATH_MAX 256

static int scr_width, scr_height, scr_scale;
static uint8_t image_romfs_io_buffer[ROMFS_FLASH_SECTOR];
static uint8_t *image_decode_arena = NULL;
static size_t image_decode_arena_size = 0;
static size_t image_decode_arena_used = 0;

typedef struct {
    size_t size;
} image_arena_header_t;

static size_t image_align16(size_t value)
{
    return (value + 15) & ~(size_t)15;
}

static bool image_ptr_in_arena(void *ptr)
{
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)image_decode_arena;
    uintptr_t end = start + image_decode_arena_size;

    return image_decode_arena && addr >= start && addr < end;
}

static void image_decode_arena_reset(void)
{
    image_decode_arena_used = 0;
}

void image_set_decode_arena(void *buffer, size_t size)
{
    image_decode_arena = buffer;
    image_decode_arena_size = size;
    image_decode_arena_reset();
    if (image_decode_arena) {
        syslog(LOG_INFO, "image decode arena: ptr=%p size=%lu", image_decode_arena,
               (unsigned long)image_decode_arena_size);
    }
}

static void *image_stbi_malloc(size_t size)
{
    if (!image_decode_arena) {
        return malloc(size);
    }

    size_t header_size = image_align16(sizeof(image_arena_header_t));
    size_t total = header_size + image_align16(size);
    if (image_decode_arena_used + total > image_decode_arena_size) {
        syslog(LOG_ERR, "image decode arena OOM: request=%lu used=%lu size=%lu", (unsigned long)size,
               (unsigned long)image_decode_arena_used, (unsigned long)image_decode_arena_size);
        return NULL;
    }

    image_arena_header_t *header = (image_arena_header_t *)(image_decode_arena + image_decode_arena_used);
    header->size = size;
    image_decode_arena_used += total;
    return (uint8_t *)header + header_size;
}

static void *image_stbi_realloc(void *ptr, size_t size)
{
    if (!ptr) {
        return image_stbi_malloc(size);
    }

    if (!image_ptr_in_arena(ptr)) {
        return realloc(ptr, size);
    }

    size_t header_size = image_align16(sizeof(image_arena_header_t));
    image_arena_header_t *old_header = (image_arena_header_t *)((uint8_t *)ptr - header_size);
    void *new_ptr = image_stbi_malloc(size);
    if (new_ptr) {
        size_t copy_size = old_header->size < size ? old_header->size : size;
        memcpy(new_ptr, ptr, copy_size);
    }
    return new_ptr;
}

static void image_stbi_free(void *ptr)
{
    if (ptr && !image_ptr_in_arena(ptr)) {
        free(ptr);
    }
}

static void build_romfs_api_path(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    if (!path || path[0] == '\0') {
        snprintf(out, out_size, "/");
    } else if (path[0] == '/') {
        snprintf(out, out_size, "%s", path);
    } else {
        snprintf(out, out_size, "/%s", path);
    }
}

static int valign(const char *s)
{
    return (scr_width >> 1) - strlen(s) * 4 * scr_scale;
}

static void copy_rgb_to_rgba(uint8_t *dst, int dst_stride, const uint8_t *src, int width, int height)
{
    for (int y = 0; y < height; y++) {
        uint8_t *out = dst + (size_t)y * dst_stride;
        const uint8_t *in = src + (size_t)y * width * 3;

        for (int x = 0; x < width; x++) {
            out[x * 4 + 0] = in[x * 3 + 0];
            out[x * 4 + 1] = in[x * 3 + 1];
            out[x * 4 + 2] = in[x * 3 + 2];
            out[x * 4 + 3] = 0xff;
        }
    }
}

static bool image_decode_to_rgba(char *name, int screen_w, int screen_h, uint8_t *output, size_t output_size,
                                 int output_stride, const char *target)
{
    char path[ROMFS_PATH_MAX];
    build_romfs_api_path(name, path, sizeof(path));

    syslog(LOG_INFO, "image_decode: name='%s' path='%s' screen=%dx%d target=%s buffer=%lu stride=%d",
           name ? name : "(null)", path, screen_w, screen_h, target ? target : "(null)",
           (unsigned long)output_size, output_stride);

    if (!output || output_stride < screen_w * 4 ||
            output_size < (size_t)(screen_h - 1) * output_stride + (size_t)screen_w * 4) {
        syslog(LOG_ERR, "image_decode: output buffer too small need=%lu have=%lu stride=%d",
               (unsigned long)((size_t)(screen_h - 1) * output_stride + (size_t)screen_w * 4),
               (unsigned long)output_size, output_stride);
        return false;
    }

    romfs_file file;
    uint32_t err = romfs_open_path(path, &file, image_romfs_io_buffer);
    if (err != ROMFS_NOERR) {
        syslog(LOG_ERR, "image_decode: open failed path='%s' err=%lu (%s)", path, (unsigned long)err,
               romfs_strerror(err));
        return false;
    }

    uint32_t length = file.entry.size;
    syslog(LOG_INFO,
           "image_decode: opened path='%s' size=%lu start=%lu nentry=%lu type=%u mode=%u parent=%u current=%u",
           path, (unsigned long)file.entry.size, (unsigned long)file.entry.start, (unsigned long)file.nentry,
           file.entry.attr.names.type, file.entry.attr.names.mode, file.entry.attr.names.parent,
           file.entry.attr.names.current);

    if (length < 2) {
        syslog(LOG_ERR, "image_decode: file too small path='%s' size=%lu", path, (unsigned long)length);
        romfs_close_file(&file);
        return false;
    }

    uint8_t *picture_data = malloc(length);
    if (!picture_data) {
        syslog(LOG_ERR, "image_decode: cannot allocate file buffer size=%lu", (unsigned long)length);
        romfs_close_file(&file);
        return false;
    }

    uint32_t read = romfs_read_file(picture_data, length, &file);
    err = file.err;
    romfs_close_file(&file);
    if (read != length) {
        syslog(LOG_ERR, "image_decode: read failed path='%s' read=%lu need=%lu err=%lu (%s)", path,
               (unsigned long)read, (unsigned long)length, (unsigned long)err, romfs_strerror(err));
        free(picture_data);
        return false;
    }

    syslog(LOG_INFO, "image_decode: read ok size=%lu first=%02X %02X %02X %02X err=%lu (%s)",
           (unsigned long)length, picture_data[0], picture_data[1], length > 2 ? picture_data[2] : 0,
           length > 3 ? picture_data[3] : 0, (unsigned long)err, romfs_strerror(err));

    if (picture_data[0] == 0xff && picture_data[1] == 0xd8) {
        syslog(LOG_INFO, "image_decode: JPEG signature ok");

        int w, h, channels;

        image_decode_arena_reset();
        stbi_uc *stbi_img = stbi_load_from_memory(picture_data, (int)length, &w, &h, &channels, 3);

        free(picture_data);

        if (stbi_img) {
            syslog(LOG_INFO, "image w = %d, h = %d, c = %d", w, h, channels);

            bool ok = true;
            if (w != screen_w || h != screen_h) {
                size_t resized_size = (size_t)screen_w * screen_h * 3;
                uint8_t *resized = malloc(resized_size);
                if (resized) {
                    ok = stbir_resize_uint8_linear(stbi_img, w, h, w * 3, resized, screen_w, screen_h, screen_w * 3,
                                                   STBIR_RGB) != NULL;
                    if (ok) {
                        copy_rgb_to_rgba(output, output_stride, resized, screen_w, screen_h);
                    }
                    free(resized);
                } else {
                    ok = false;
                    syslog(LOG_ERR, "image_decode: cannot allocate resize buffer size=%lu",
                           (unsigned long)resized_size);
                }
                if (!ok) {
                    syslog(LOG_ERR, "image_decode: resize failed src=%dx%d dst=%dx%d", w, h, screen_w, screen_h);
                }
            } else {
                copy_rgb_to_rgba(output, output_stride, stbi_img, screen_w, screen_h);
            }

            syslog(LOG_INFO, "resized w = %d, h = %d, c = %d", screen_w, screen_h, channels);
            stbi_image_free(stbi_img);
            syslog(LOG_INFO, "image decode arena used=%lu", (unsigned long)image_decode_arena_used);
            image_decode_arena_reset();
            return ok;
        } else {
            syslog(LOG_ERR, "image_decode: stbi decode failed: %s", stbi_failure_reason());
            image_decode_arena_reset();
        }
    } else {
        syslog(LOG_ERR, "image_decode: not a JPEG path='%s' first=%02X %02X", path, picture_data[0],
               picture_data[1]);
        free(picture_data);
    }

    return false;
}

bool image_load_into(char *name, int screen_w, int screen_h, sprite_t *image, size_t image_size)
{
    size_t required_size = sizeof(sprite_t) + (size_t)screen_w * screen_h * 4;
    if (!image || image_size < required_size) {
        syslog(LOG_ERR, "image_load_into: buffer too small need=%lu have=%lu", (unsigned long)required_size,
               (unsigned long)image_size);
        return false;
    }

    image->width = screen_w;
    image->height = screen_h;
    image->flags = FMT_RGBA32;
    image->hslices = 1;
    image->vslices = 1;

    return image_decode_to_rgba(name, screen_w, screen_h, (uint8_t *)&image->data[0],
                                image_size - sizeof(sprite_t), screen_w * 4, "sprite");
}

bool image_load_to_surface(char *name, display_context_t disp)
{
    if (!disp) {
        syslog(LOG_ERR, "image_load_to_surface: no display surface");
        return false;
    }

    if (surface_get_format(disp) != FMT_RGBA32) {
        syslog(LOG_ERR, "image_load_to_surface: unsupported surface format %u", surface_get_format(disp));
        return false;
    }

    return image_decode_to_rgba(name, disp->width, disp->height, disp->buffer, (size_t)disp->stride * disp->height,
                                disp->stride, "display");
}

sprite_t *image_load(char *name, int screen_w, int screen_h)
{
    size_t image_size = sizeof(sprite_t) + (size_t)screen_w * screen_h * 4;
    sprite_t *image = malloc(image_size);
    if (!image) {
        syslog(LOG_ERR, "cannot allocate image sprite %d x %d", screen_w, screen_h);
        return NULL;
    }

    if (!image_load_into(name, screen_w, screen_h, image, image_size)) {
        free(image);
        return NULL;
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
        syslog(LOG_INFO, "image_view: open '%s'", name ? name : "(null)");
        disp = display_get();

        bool loaded = image_load_to_surface(name, disp);

        if (loaded) {
            syslog(LOG_INFO, "image_view: loaded '%s'", name ? name : "(null)");
        } else {
            syslog(LOG_ERR, "image_view: load failed '%s'", name ? name : "(null)");
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

        break;
    }
}
