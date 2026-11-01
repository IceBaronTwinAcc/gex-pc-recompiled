#include <stdio.h>
#include <SDL2/SDL.h>
#include "win32hle.h"
#include "renderer.h"
void hle_screen_device_window(void *window);

static int open_renderer(int headless, char *error, size_t size) {
    if (hle_screen_open("Gex - static recompilation", 0, headless)) {
        snprintf(error, size, "SDL screen initialization failed: %s", SDL_GetError());
        return -1;
    }
    return 0;
}

static int configure(const gex_render_frame *frame, char *error, size_t size) {
    (void)error; (void)size;
    hle_gdi_window_surface(frame->pixels, frame->width, frame->height, frame->pitch, 16);
    hle_set_screen_size(frame->width, frame->height);
    return 0;
}

static int present(const gex_render_frame *frame, char *error, size_t size) {
    (void)frame; (void)error; (void)size;
    hle_gdi_window_touched();
    return 0;
}

static void shutdown(void) {
    hle_gdi_window_surface(NULL, 0, 0, 0, 0);
    hle_screen_close();
}

static int pump(char *error, size_t size) {
    (void)error; (void)size;
    hle_screen_pump();
    return 0;
}

const gex_renderer_backend *gex_renderer_backend_get(void) {
    static const gex_renderer_backend backend = {
        .open = open_renderer, .configure = configure, .present = present,
        .pump = pump, .shutdown = shutdown, .device_window = hle_screen_device_window
    };
    return &backend;
}
