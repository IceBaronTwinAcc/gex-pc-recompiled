#include <kos.h>
#include <dc/sq.h>
#include <stdio.h>
#include <string.h>
#include "renderer.h"
#include "win32hle.h"

static int configured;
static int shown;
static int video_width, video_height;
static gex_render_rect output;
static uint16_t source_x[640];
static size_t source_row[480];
static uint16_t scanline[640] __attribute__((aligned(32)));
static uint16_t upload_x[640];
static int upload_width, upload_target_width;
static uint16_t color_low[256], color_high[256];

static int open_renderer(int headless, char *error, size_t size) {
    (void)headless; (void)error; (void)size;
    shown = 0;
    for (unsigned i = 0; i < 256; ++i) {
        color_low[i] = (uint16_t)((i & 31) << 11 | (i & 0xE0) << 1);
        color_high[i] = (uint16_t)((i & 3) << 9 | (i >> 2 & 31));
    }
    video_width = 320;
    video_height = 240;
    vid_set_mode(DM_320x240, PM_RGB565);
    vid_clear(0, 0, 0);
    return 0;
}
static int configure(const gex_render_frame *frame, char *error, size_t size) {
    if (frame->width > 640 || frame->height > 480) {
        snprintf(error, size, "Dreamcast framebuffer supports at most 640x480");
        return -1;
    }
    hle_gdi_window_surface(frame->pixels, frame->width, frame->height, frame->pitch, 16);
    hle_set_screen_size(frame->width, frame->height);
    int width = frame->width <= 320 && frame->height <= 240 ? 320 : 640;
    int height = width == 320 ? 240 : 480;
    if (width != video_width) {
        vid_set_mode(width == 320 ? DM_320x240 : DM_640x480, PM_RGB565);
        video_width = width;
        video_height = height;
    }
    output = gex_renderer_fit(video_width, video_height, frame->width, frame->height);
    for (int x = 0; x < output.width; ++x)
        source_x[x] = (uint16_t)(x * frame->width / output.width);
    for (int y = 0; y < output.height; ++y)
        source_row[y] = (size_t)(y * frame->height / output.height) * frame->pitch;
    vid_clear(0, 0, 0);
    memset(scanline, 0, sizeof scanline);
    configured = 1;
    return 0;
}
static void shown_frame(const gex_render_frame *frame) {
    if (!shown) {
        fprintf(stderr, "[dreamcast] presented first framebuffer: %dx%d\n",
                frame->width, frame->height);
        shown = 1;
    }
}
static int present(const gex_render_frame *frame, char *error, size_t size) {
    (void)error; (void)size;
    /* Direct VRAM writes need no thread-affine graphics context. The shared
     * guest machine lock serializes commits from the game's worker thread. */
    if (frame->width == video_width && frame->height == video_height &&
        frame->pitch == video_width * 2 && !((uintptr_t)frame->pixels & 7)) {
        uint32_t *destination = sq_lock((void *)vram_s);
        sq_fast_cpy(destination, frame->pixels, (size_t)frame->pitch * frame->height / 32);
        sq_wait();
        sq_unlock();
    } else {
        size_t previous_row = SIZE_MAX;
        uint32_t *destination = sq_lock((void *)vram_s);
        for (int y = 0; y < output.height; ++y) {
            if (source_row[y] != previous_row) {
                const uint16_t *src = (const uint16_t *)(frame->pixels + source_row[y]);
                for (int x = 0; x < output.width; ++x)
                    scanline[output.x + x] = src[source_x[x]];
                previous_row = source_row[y];
            }
            /* Transfer whole aligned rows, including fitted black side borders.
             * Repeated output rows reuse the already-scaled source scanline. */
            sq_fast_cpy(destination + (y + output.y) * (video_width / 2),
                        scanline, (size_t)video_width * 2 / 32);
        }
        sq_wait();
        sq_unlock();
    }
    shown_frame(frame);
    return 0;
}
static int upload_bgr555(const gex_render_frame *frame, const uint8_t *pixels,
                         int width, int height, int pitch, char *error, size_t size) {
    (void)error; (void)size;
    if (frame->width != video_width || frame->height != video_height ||
        frame->pitch != video_width * 2 || ((uintptr_t)frame->pixels & 7u)) return 1;
    if (upload_width != width || upload_target_width != frame->width) {
        for (int x = 0; x < frame->width; ++x)
            upload_x[x] = (uint16_t)(x * width / frame->width);
        upload_width = width; upload_target_width = frame->width;
    }
    int previous_y = -1;
    uint32_t *destination = sq_lock((void *)vram_s);
    for (int y = 0; y < frame->height; ++y) {
        int sy = y * height / frame->height;
        uint16_t *row = (uint16_t *)(frame->pixels + (size_t)y * frame->pitch);
        if (sy != previous_y) {
            const uint8_t *source = pixels + (size_t)sy * pitch;
            int x = 0;
            if (width == frame->width && !((uintptr_t)source & 3u)) {
                const uint8_t *aligned_source = __builtin_assume_aligned(source, 4);
                uint8_t *aligned_target = __builtin_assume_aligned(row, 4);
                int bytes = frame->width * 2;
#pragma GCC unroll 4
                for (; x + 3 < bytes; x += 4) {
                    uint32_t value;
                    memcpy(&value, aligned_source + x, sizeof value);
                    uint32_t rgb = (value & UINT32_C(0x001F001F)) << 11 |
                        (value & UINT32_C(0x03E003E0)) << 1 |
                        (value >> 10 & UINT32_C(0x001F001F));
                    memcpy(aligned_target + x, &rgb, sizeof rgb);
                }
                x /= 2;
            }
            if (width == frame->width) {
                for (; x < frame->width; ++x)
                    row[x] = color_low[source[x * 2]] | color_high[source[x * 2 + 1]];
            }
            for (; x < frame->width; ++x) {
                const uint8_t *p = source + upload_x[x] * 2;
                row[x] = color_low[p[0]] | color_high[p[1]];
            }
            previous_y = sy;
        } else memcpy(row, (uint8_t *)row - frame->pitch, (size_t)video_width * 2);
        sq_fast_cpy(destination + y * (video_width / 2), row,
                    (size_t)video_width * 2 / 32);
    }
    sq_wait();
    sq_unlock();
    shown_frame(frame);
    return 0;
}
static int pump(char *error, size_t size) { (void)error; (void)size; return 0; }
static void shutdown_renderer(void) {
    if (configured) hle_gdi_window_surface(NULL, 0, 0, 0, 0);
    configured = 0;
}
const gex_renderer_backend *gex_renderer_backend_get(void) {
    static const gex_renderer_backend backend = {
        open_renderer, configure, present, pump, shutdown_renderer, upload_bgr555
    };
    return &backend;
}
