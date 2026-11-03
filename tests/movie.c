#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "host.h"
#include "win32hle.h"
#include "movie.h"
#include "renderer.h"
#include "platform/audio.h"
#if GEX_ENABLE_GLDC_RENDERER || GEX_ENABLE_PVR_RENDERER
int gex_graphics_gpu_owned(void) { return 0; }
void gex_graphics_movie(const uint8_t *pixels, int width, int height, int pitch) {
    (void)pixels; (void)width; (void)height; (void)pitch;
    hle_fatal("software movie fixture reached native movie output");
}
#endif

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;
static uint32_t mci;
static unsigned frames;
static int resize, failures;

static unsigned sequence(void) {
    gex_render_frame frame;
    return gex_renderer_frame(&frame) ? 0 : frame.sequence;
}

static void record(const gex_render_frame *frame) {
    if (resize) {
        gex_render_rect rect = gex_renderer_fit(frame->width, frame->height, 320, 240);
        for (int y = 0; y < frame->height; ++y)
            for (int x = 0; x < frame->width; ++x)
                if (x < rect.x || x >= rect.x + rect.width ||
                    y < rect.y || y >= rect.y + rect.height) {
                    const uint8_t *pixel = frame->pixels + y * frame->pitch + x * 2;
                    if (pixel[0] || pixel[1]) {
                        if (!failures) fprintf(stderr, "movie padding was not cleared at %d,%d\n", x, y);
                        ++failures;
                    }
                }
    }
    ++frames;
}

static int command(uint32_t code, uint32_t flags, uint32_t params) {
    uint32_t args[] = {1, code, flags, params};
    uint32_t result = hle_call_guest(mci, 4, args);
    if (result) fprintf(stderr, "MCI 0x%X returned %u\n", code, result);
    return result != 0;
}

static void pump(unsigned milliseconds) {
    uint32_t start = SDL_GetTicks();
    do {
        gex_movie_pump();
        if (gex_renderer_pump()) hle_fatal("movie test renderer: %s", gex_renderer_error());
        SDL_Delay(2);
    } while (SDL_GetTicks() - start < milliseconds);
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) return 1;
    resize = argc == 3 && !strcmp(argv[2], "--resize");
    if (argc == 3 && !resize) return 1;
    recomp_host_init();
    if (!gex_audio_open()) {
        fprintf(stderr, "SKIP: movie lifecycle needs an available audio device\n");
        return 77;
    }
    gex_movie_register(0);
    mci = hle_resolve("mciSendCommandA");
    if (gex_renderer_init(gex_renderer_backend_get(), 1) || gex_renderer_resize(320, 240)) return 1;
    gex_renderer_observe(record);
    mach_enter();
    uint32_t params = hle_va(hle_alloc(28));
    uint32_t path = hle_guest_strdup(argv[1]);
    const int sizes[][2] = {{320, 224}, {427, 240}, {854, 480}, {1920, 1080},
                            {2560, 1080}, {240, 427}, {3440, 1440}};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        if (resize && gex_renderer_resize(sizes[i][0], sizes[i][1])) return 1;
        memset(hle_ptr(params), 0, 28);
        MEM32(params + 12) = path;
        if (command(0x803, 0x200, params) || command(0x806, 0, 0)) return 1;
        unsigned initial_seq = sequence();
        pump(200);
        if (sequence() == initial_seq) {
            fprintf(stderr, "Renderer sequence did not advance during playback\n");
            return 1;
        }
        if (command(0x809, 0, 0)) return 1;
        unsigned before = frames;
        unsigned paused_seq = sequence();
        pump(150);
        if (frames != before || sequence() != paused_seq) {
            fprintf(stderr, "video frames advanced during pause\n");
            return 1;
        }
        if (command(0x855, 0, 0)) return 1;
        pump(200);
        if (frames <= before || sequence() == paused_seq) {
            fprintf(stderr, "video did not resume\n");
            return 1;
        }
        if (command(0x807, 0x100, 0) || command(0x806, 0, 0)) return 1;
        pump(100);
        if (command(0x808, 0, 0) || command(0x804, 0, 0)) return 1;
    }
    mach_leave();
    gex_renderer_shutdown();
    SDL_Quit();
    return failures ? 1 : 0;
}
