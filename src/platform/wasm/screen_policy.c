#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <emscripten.h>
#include <emscripten/threading.h>
#include "platform/desktop/screen_policy.h"
#include "platform/video.h"
#include "platform/wasm/fullscreen.h"
#include "preferences.h"

static int output_width, output_height, output_captured, fullscreen_pending;
static _Atomic int web_choice;
static void (*web_chosen)(int);
static char web_menu_json[16384];

Uint32 gex_screen_window_flags(int fullscreen) {
    (void)fullscreen;
    return SDL_WINDOW_RESIZABLE;
}
SDL_Renderer *gex_screen_create_renderer(SDL_Window *window) {
    return SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
}
void gex_screen_opened(int fullscreen) {
    output_captured = 0;
    fullscreen_pending = fullscreen != 0;
}
void gex_screen_close_platform(SDL_Window *window) {
    if (window) {
        int result = gex_wasm_set_fullscreen(0);
        if (result) fprintf(stderr, "[video] Browser display change failed (%d)\n", result);
    }
    fullscreen_pending = 0;
}
void gex_screen_shortcut_fullscreen(SDL_Window *window, int on) {
    (void)window; (void)on;
}
int gex_screen_display(SDL_Window *window, int mode, char *error, size_t size) {
    if (window && mode != GEX_DISPLAY_WINDOWED && !output_captured) {
        fullscreen_pending = 1;
        return 1;
    }
    fullscreen_pending = 0;
    int result = window ? gex_wasm_set_fullscreen(mode != GEX_DISPLAY_WINDOWED) : 0;
    if (result) {
        snprintf(error, size, "Browser display change failed (%d)", result);
        return -1;
    }
    return 0;
}
int gex_screen_desktop(SDL_Window *window, int *width, int *height) {
    (void)window;
    *width = 1280; *height = 720;
    return 0;
}
int gex_video_migrate_saved_resolution(int *width, int *height) {
    if (!width || !height || *width <= 0 || *height <= 0 ||
        (int64_t)*width * 9 >= (int64_t)*height * 16)
        return 0;
    int desktop_width, desktop_height;
    if (gex_video_desktop(&desktop_width, &desktop_height) ||
        gex_preferences_default_size(desktop_width, desktop_height, width, height))
        return -1;
    return 1;
}
void gex_screen_before_present(SDL_Renderer *renderer, int *fullscreen) {
    if (renderer && !output_captured) {
        if (SDL_GetRendererOutputSize(renderer, &output_width, &output_height))
            hle_fatal("SDL renderer output size: %s", SDL_GetError());
        output_captured = 1;
    }
    if (fullscreen_pending) {
        int result = gex_wasm_set_fullscreen(1);
        if (result) hle_fatal("Browser display change failed (%d)", result);
        *fullscreen = 1;
        fullscreen_pending = 0;
    }
}
void gex_screen_after_present(void) {
    static int presented;
    if (!presented) {
        presented = 1;
        MAIN_THREAD_ASYNC_EM_ASM({
            if (document.getElementById("status").textContent === "Game files loaded. Starting Gex...")
                window.gexSetStatus("Gex is running.");
        });
    }
}
static size_t json_item(char *out, size_t n, const hle_menu_item_t *it) {
    size_t k = 0;
    if (!it->label) return (size_t)snprintf(out, n, "null");
    k += (size_t)snprintf(out + k, n - k, "{\"label\":\"");
    for (const char *p = it->label; *p && k + 8 < n; p++) {
        if (*p == '"' || *p == '\\') out[k++] = '\\';
        out[k++] = *p;
    }
    k += (size_t)snprintf(out + k, n - k, "\",\"hint\":\"%s\",\"id\":%d,\"checked\":%d", it->hint ? it->hint : "", it->id, it->checked ? 1 : 0);
    if (it->sub && it->nsub > 0) {
        k += (size_t)snprintf(out + k, n - k, ",\"sub\":[");
        for (int i = 0; i < it->nsub && k + 64 < n; i++) {
            if (i) out[k++] = ',';
            k += json_item(out + k, n - k, &it->sub[i]);
        }
        k += (size_t)snprintf(out + k, n - k, "]");
    }
    k += (size_t)snprintf(out + k, n - k, "}");
    return k;
}
int gex_screen_platform_menu(const hle_menu_item_t *items, int n, void (*chosen)(int)) {
    if (!items || n <= 0) return 1;
    size_t k = 0;
    web_menu_json[k++] = '[';
    for (int i = 0; i < n && k + 64 < sizeof web_menu_json; i++) {
        if (i) web_menu_json[k++] = ',';
        k += json_item(web_menu_json + k, sizeof web_menu_json - k, &items[i]);
    }
    web_menu_json[k++] = ']';
    web_menu_json[k] = 0;
    atomic_store(&web_choice, 0);
    web_chosen = chosen;
    MAIN_THREAD_ASYNC_EM_ASM({
        if (Module['gexPopupMenu'])
            Module['gexPopupMenu'](UTF8ToString($0), $1);
        else
            Atomics.store(HEAP32, $1 >> 2, -1);
    }, web_menu_json, &web_choice);
    return 1;
}
int gex_screen_platform_menu_open(void) { return web_chosen != NULL; }
void gex_screen_poll_platform(void) {
    emscripten_current_thread_process_queued_calls();
    int choice = atomic_load(&web_choice);
    if (!web_chosen || !choice) return;
    void (*cb)(int) = web_chosen;
    atomic_store(&web_choice, 0);
    web_chosen = NULL;
    if (choice > 0) cb(choice);
}
void gex_screen_event_platform(SDL_Window *window, SDL_Renderer *renderer,
                               const SDL_Event *event, int input_trace) {
    if (getenv("RA2_INPUT_TRACE") || input_trace) {
        if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP)
            fprintf(stderr, "[input] key %s scancode %d repeat %d\n", event->type == SDL_KEYDOWN ? "down" : "up", event->key.keysym.scancode, event->key.repeat);
        else if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP)
            fprintf(stderr, "[input] button %d %s at %d,%d\n", event->button.button, event->type == SDL_MOUSEBUTTONDOWN ? "down" : "up", event->button.x, event->button.y);
    }
    if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        int width, height;
        if (SDL_GetRendererOutputSize(renderer, &width, &height))
            hle_fatal("SDL renderer output size: %s", SDL_GetError());
        if (output_width > 0 && (width != output_width || height != output_height))
            SDL_SetWindowSize(window, output_width, output_height);
    }
}
