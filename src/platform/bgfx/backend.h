#ifndef GEX_BGFX_BACKEND_H
#define GEX_BGFX_BACKEND_H

#include <SDL2/SDL.h>
#include "renderer.h"

int gex_bgfx_open(SDL_Window *window, char *error, size_t size);
int gex_bgfx_configure(const gex_render_frame *frame, char *error, size_t size);
int gex_bgfx_present(const gex_render_frame *frame, int width, int height,
                     char *error, size_t size);
int gex_bgfx_read_present(uint8_t *rgba, char *error, size_t size);

#endif
