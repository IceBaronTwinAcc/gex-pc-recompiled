#include <stdio.h>
#include "platform/desktop/screen_policy.h"
#include "preferences.h"

Uint32 gex_screen_window_flags(int fullscreen) {
    return SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
           (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
SDL_Renderer *gex_screen_create_renderer(SDL_Window *window) {
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    return renderer ? renderer : SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
}
void gex_screen_opened(int fullscreen) { (void)fullscreen; }
void gex_screen_close_platform(SDL_Window *window) { (void)window; }
void gex_screen_shortcut_fullscreen(SDL_Window *window, int on) {
    if (window) SDL_SetWindowFullscreen(window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
int gex_screen_display(SDL_Window *window, int mode, char *error, size_t size) {
    Uint32 flags = mode == GEX_DISPLAY_BORDERLESS ? SDL_WINDOW_FULLSCREEN_DESKTOP :
        mode == GEX_DISPLAY_EXCLUSIVE ? SDL_WINDOW_FULLSCREEN : 0;
    if (window && SDL_SetWindowFullscreen(window, flags)) {
        snprintf(error, size, "Display change failed: %s", SDL_GetError());
        return -1;
    }
    return 0;
}
int gex_screen_desktop(SDL_Window *window, int *width, int *height) {
    if (!window) { *width = 320; *height = 240; return 0; }
    int display = SDL_GetWindowDisplayIndex(window);
    SDL_DisplayMode mode;
    if (display < 0 || SDL_GetDesktopDisplayMode(display, &mode)) {
        fprintf(stderr, "[video] cannot query display resolution: %s\n", SDL_GetError());
        return -1;
    }
    *width = mode.w; *height = mode.h;
    return 0;
}
int gex_video_migrate_saved_resolution(int *width, int *height) {
    (void)width; (void)height;
    return 0;
}
void gex_screen_before_present(SDL_Renderer *renderer, int *fullscreen) {
    (void)renderer; (void)fullscreen;
}
void gex_screen_after_present(void) {}
void gex_screen_poll_platform(void) {}
void gex_screen_event_platform(SDL_Window *window, SDL_Renderer *renderer,
                               const SDL_Event *event, int input_trace) {
    (void)window; (void)renderer; (void)event; (void)input_trace;
}
int gex_screen_platform_menu(const hle_menu_item_t *items, int count,
                             void (*chosen)(int)) {
    (void)items; (void)count; (void)chosen;
    return 0;
}
int gex_screen_platform_menu_open(void) { return 0; }
