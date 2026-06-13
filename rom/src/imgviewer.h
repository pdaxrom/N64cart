#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <libdragon.h>

sprite_t *image_load(char *name, int screen_w, int screen_h);
bool image_load_into(char *name, int screen_w, int screen_h, sprite_t *image, size_t image_size);
bool image_load_to_surface(char *name, display_context_t disp);
void image_set_decode_arena(void *buffer, size_t size);

void image_view(char *name, int screen_w, int screen_h, int screen_scale);
