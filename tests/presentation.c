#include "../src/platform/desktop/screen.c"
#include "host.h"
#include "renderer.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;
static unsigned recorded;
static unsigned consumed_keys;
static int failures;

#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

static int consume_key(int scancode, int down) {
    (void)scancode;
    if (down) ++consumed_keys;
    return 1;
}

static void record(const uint8_t *px, int w, int h, int pitch, int bpp) {
    CHECK(pthread_equal(pthread_self(), g_video_thread));
    CHECK(w == 2 && h == 2 && pitch == 4 && bpp == 16);
    CHECK(px[0] == 0 && px[1] == 0xF8);
    ++recorded;
}

static void *worker(void *unused) {
    (void)unused;
    mach_enter();
    CHECK(!gex_renderer_pump());
    CHECK(!gex_renderer_commit());
    mach_leave();
    return NULL;
}

int main(void) {
#ifdef __ANDROID__
    SDL_SetMainReady();
    CHECK(vk_of(SDL_SCANCODE_AC_BACK) == vk_of(SDL_SCANCODE_ESCAPE));
    CHECK(scan_of(SDL_SCANCODE_AC_BACK) == scan_of(SDL_SCANCODE_ESCAPE));
#else
    CHECK(vk_of(SDL_SCANCODE_AC_BACK) == 0);
    CHECK(scan_of(SDL_SCANCODE_AC_BACK) == 0);
#endif
    recomp_host_init();
    if (gex_renderer_init(gex_renderer_backend_get(), 1) ||
        gex_renderer_resize(2, 2)) return 1;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL video: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Surface *output = SDL_CreateRGBSurfaceWithFormat(0, 2, 2, 32, SDL_PIXELFORMAT_ARGB8888);
    g_ren = output ? SDL_CreateSoftwareRenderer(output) : NULL;
    if (!g_ren) {
        fprintf(stderr, "SDL offscreen renderer: %s\n", SDL_GetError());
        return 1;
    }
    g_video_thread = pthread_self();
    g_mode = 3;
    g_bars = 0;
    hle_screen_frame_hook = record;
    gex_render_frame frame;
    CHECK(!gex_renderer_frame(&frame));
    for (unsigned i = 0; i < 4; ++i) {
        frame.pixels[i * 2] = 0;
        frame.pixels[i * 2 + 1] = 0xF8;
    }
    g_last_present = hle_ticks_ms() - 300;
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL)) return 1;
    pthread_join(thread, NULL);
    CHECK(recorded == 0 && g_shown_seq == ~0u);
    mach_enter();
    CHECK(!gex_renderer_pump());
    CHECK(recorded == 1);
    uint32_t pixels[4] = {0};
    CHECK(!SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_ARGB8888, pixels, 8));
    for (unsigned i = 0; i < 4; ++i) CHECK((pixels[i] & 0xFFFFFFu) == 0xFF0000u);
    mach_leave();
    hle_screen_frame_hook = NULL;
    const int sizes[][2] = {{1237, 701}, {3840, 2160}};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        int w = sizes[i][0], h = sizes[i][1];
        SDL_DestroyTexture(g_tex); g_tex = NULL;
        SDL_DestroyRenderer(g_ren); g_ren = NULL;
        SDL_FreeSurface(output);
        output = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        g_ren = output ? SDL_CreateSoftwareRenderer(output) : NULL;
        if (!g_ren) {
            fprintf(stderr, "SDL custom-resolution offscreen renderer: %s\n", SDL_GetError());
            return 1;
        }
        CHECK(!gex_renderer_resize(w, h));
        CHECK(!gex_renderer_frame(&frame));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                unsigned value = y < h / 2 ? 0xF800 : 0x001F;
                uint8_t *pixel = frame.pixels + y * frame.pitch + x * 2;
                pixel[0] = (uint8_t)value; pixel[1] = (uint8_t)(value >> 8);
            }
        g_last_present = hle_ticks_ms() - 300;
        mach_enter();
        CHECK(!gex_renderer_commit() && !gex_renderer_pump());
        CHECK(g_tex_w == w && g_tex_h == h && g_dst.w == w && g_dst.h == h);
        SDL_Rect sample = {w - 1, 0, 1, 1};
        uint32_t color = 0;
        CHECK(!SDL_RenderReadPixels(g_ren, &sample, SDL_PIXELFORMAT_ARGB8888, &color, 4));
        CHECK((color & 0xFFFFFF) == 0xFF0000);
        sample.y = h - 1;
        CHECK(!SDL_RenderReadPixels(g_ren, &sample, SDL_PIXELFORMAT_ARGB8888, &color, 4));
        CHECK((color & 0xFFFFFF) == 0x0000FF);
        mach_leave();
    }
    gex_renderer_shutdown();
    CHECK(!g_tex && !g_ren);
    SDL_FreeSurface(output);
    /* A hidden dummy-driver window exercises the real startup sizing path. */
    CHECK(!gex_renderer_init(gex_renderer_backend_get(), 1));
    g_win = SDL_CreateWindow("window sizing regression", 0, 0, 1280, 960, SDL_WINDOW_HIDDEN);
    CHECK(g_win != NULL);
    if (g_win) {
        CHECK(!gex_renderer_resize(427, 240));
        CHECK(!gex_renderer_commit());
        int width, height;
        SDL_GetWindowSize(g_win, &width, &height);
        CHECK(width == 1280 && height == 719);
        CHECK(abs(width * 240 - height * 427) < 427);
        SDL_SetWindowSize(g_win, 900, 600);
        CHECK(!gex_renderer_commit() && !gex_renderer_pump());
        SDL_GetWindowSize(g_win, &width, &height);
        CHECK(width == 900 && height == 600);
    }
    gex_renderer_shutdown();
    CHECK(!gex_renderer_init(gex_renderer_backend_get(), 1));
    CHECK(!gex_renderer_resize(320, 240));
    SDL_Window *native = SDL_CreateWindow("native input handoff", 0, 0, 600, 600,
                                          SDL_WINDOW_HIDDEN);
    CHECK(native != NULL);
    if (native) {
        gex_renderer_device_window(native);
        CHECK(g_win == native && !g_ren && g_external_window);
        hle_screen_key_hook = consume_key;
        SDL_Event event = {.type = SDL_KEYDOWN};
        event.key.keysym.scancode = SDL_SCANCODE_RETURN;
        event.key.keysym.mod = KMOD_ALT;
        CHECK(SDL_PushEvent(&event) == 1);
        hle_screen_pump();
        CHECK(g_fullscreen && consumed_keys == 0);
        event.key.keysym.mod = KMOD_NONE;
        CHECK(SDL_PushEvent(&event) == 1);
        hle_screen_pump();
        CHECK(g_fullscreen && consumed_keys == 1);
        event.key.keysym.mod = KMOD_ALT;
        CHECK(SDL_PushEvent(&event) == 1);
        hle_screen_pump();
        CHECK(!g_fullscreen && consumed_keys == 1);
        hle_screen_key_hook = NULL;
        SDL_SetWindowSize(native, 600, 600);
        int x, y;
        to_game(300, 300, &x, &y);
        CHECK(x == 160 && y == 120 && g_dst.y == 75);
        SDL_SetWindowSize(native, 800, 600);
        to_game(400, 300, &x, &y);
        CHECK(x == 160 && y == 120 && g_dst.y == 0);
        CHECK(!gex_renderer_resize(427, 240));
        to_game(400, 300, &x, &y);
        CHECK(x == 213 && y == 120 && g_dst.y == 75);
        CHECK(!gex_renderer_commit());
        int width, height;
        SDL_GetWindowSize(native, &width, &height);
        CHECK(width == 800 && height == 600);
        gex_renderer_shutdown();
        CHECK(SDL_GetWindowID(native) != 0);
        SDL_DestroyWindow(native);
    } else gex_renderer_shutdown();
    SDL_Quit();
    return failures ? 1 : 0;
}
