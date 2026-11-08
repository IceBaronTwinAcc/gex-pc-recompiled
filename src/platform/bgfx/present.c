#include <stdio.h>
#include "platform/bgfx/backend.h"
#include "packet_gpu.h"
#include "win32hle.h"
#include "platform/desktop/app_icon.h"

void hle_screen_device_window(void *window);
static SDL_Window *window;
static int video;
static int window_width, window_height;

static int open_renderer(int headless, char *error, size_t size) {
    if (hle_screen_open("Gex - bgfx", 0, 1)) {
        snprintf(error, size, "cannot initialise bgfx input services");
        return -1;
    }
    if (!headless) {
#ifdef __ANDROID__
        SDL_SetHint(SDL_HINT_VIDEO_EXTERNAL_CONTEXT, "1");
#endif
        if (gex_app_video_init()) {
            snprintf(error, size, "bgfx window video initialisation: %s", SDL_GetError());
            return -1;
        }
        video = 1;
        uint32_t flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#ifdef __ANDROID__
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
        window = gex_app_window("Gex - bgfx", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            960, 720, flags);
        if (!window) {
            snprintf(error, size, "bgfx window creation: %s", SDL_GetError());
            return -1;
        }
        SDL_GetWindowSizeInPixels(window, &window_width, &window_height);
        hle_screen_device_window(window);
    }
    return gex_bgfx_open(window, error, size);
}

static int configure(const gex_render_frame *frame, char *error, size_t size) {
    if (gex_bgfx_configure(frame, error, size)) return -1;
    hle_gdi_window_surface(frame->pixels, frame->width, frame->height, frame->pitch, 16);
    hle_set_screen_size(frame->width, frame->height);
    return 0;
}

static int present(const gex_render_frame *frame, char *error, size_t size) {
    int width = frame->width, height = frame->height;
    if (window) {
#ifdef __EMSCRIPTEN__
        width = window_width; height = window_height;
#else
        SDL_GetWindowSizeInPixels(window, &width, &height);
#endif
    }
    return gex_bgfx_present(frame, width, height, error, size);
}

static int pump(char *error, size_t size) {
    (void)error; (void)size;
    hle_screen_pump();
    return 0;
}

static void shutdown_renderer(void) {
#ifndef __EMSCRIPTEN__
    gex_packet_gpu_shutdown_bgfx();
#endif
    hle_gdi_window_surface(NULL, 0, 0, 0, 0);
    hle_screen_close();
    SDL_DestroyWindow(window);
    window = NULL;
    if (video) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    video = 0;
#ifdef __EMSCRIPTEN__
    /* SDL can queue canvas resizes during shutdown; its owning worker must
     * remain alive until the window and video subsystem have been destroyed. */
    gex_packet_gpu_shutdown_bgfx();
#endif
}

const gex_renderer_backend *gex_bgfx_renderer(void) {
    static const gex_renderer_backend backend = {
        .open = open_renderer, .configure = configure, .present = present,
        .pump = pump, .shutdown = shutdown_renderer
    };
    return &backend;
}
