#ifndef GEX_SCREEN_POLICY_H
#define GEX_SCREEN_POLICY_H

#include <stddef.h>
#include <SDL2/SDL.h>
#include "win32hle.h"

Uint32 gex_screen_window_flags(int fullscreen);
SDL_Renderer *gex_screen_create_renderer(SDL_Window *window);
void gex_screen_opened(int fullscreen);
void gex_screen_close_platform(SDL_Window *window);
void gex_screen_shortcut_fullscreen(SDL_Window *window, int on);
int gex_screen_display(SDL_Window *window, int mode, char *error, size_t size);
int gex_screen_desktop(SDL_Window *window, int *width, int *height);
int gex_video_migrate_saved_resolution(int *width, int *height);
void gex_screen_before_present(SDL_Renderer *renderer, int *fullscreen);
void gex_screen_after_present(void);
void gex_screen_poll_platform(void);
void gex_screen_event_platform(SDL_Window *window, SDL_Renderer *renderer,
                               const SDL_Event *event, int input_trace);
int gex_screen_platform_menu(const hle_menu_item_t *items, int count,
                             void (*chosen)(int));
int gex_screen_platform_menu_open(void);

#endif
