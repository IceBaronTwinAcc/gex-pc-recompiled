#ifndef GEX_BGFX_PRESENT_POLICY_H
#define GEX_BGFX_PRESENT_POLICY_H
#include <SDL2/SDL.h>

void gex_bgfx_window_size(SDL_Window *window, int initial_width, int initial_height,
                          int *width, int *height);
void gex_bgfx_before_window_close(void);
void gex_bgfx_after_window_close(void);
#endif
