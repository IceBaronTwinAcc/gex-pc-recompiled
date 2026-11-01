#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "renderer.h"

KOS_INIT_FLAGS(INIT_DEFAULT);
static int failures;
#define EXPECT(expr) do { if (!(expr)) { printf("VIDEO FAIL line %d: %s\n", __LINE__, #expr); ++failures; } } while (0)
void hle_gdi_window_surface(uint8_t *pixels, int w, int h, int pitch, int bpp) {
    (void)pixels; (void)w; (void)h; (void)pitch; (void)bpp;
}
void hle_set_screen_size(int w, int h) { (void)w; (void)h; }
static unsigned observed;
static void observe_frame(const gex_render_frame *frame) {
    EXPECT(frame->pixels != NULL);
    ++observed;
}
static void check_upload(int w, int h, int sw, int sh, int padding, int offset) {
    int pitch = sw * 2 + padding;
    uint8_t *storage = malloc((size_t)pitch * sh + offset);
    if (!storage) abort();
    uint8_t *pixels = storage + offset;
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            uint16_t color = (uint16_t)((y * sw + x) * 73 + 19);
            pixels[y * pitch + x * 2] = (uint8_t)color;
            pixels[y * pitch + x * 2 + 1] = (uint8_t)(color >> 8);
        }
    EXPECT(!gex_renderer_resize(w, h));
    uint64_t at = thd_get_cpu_time(thd_get_current());
    for (int i = 0; i < 30; ++i)
        EXPECT(!gex_renderer_upload_bgr555(pixels, sw, sh, pitch));
    uint64_t ns = thd_get_cpu_time(thd_get_current()) - at;
    printf("VIDEO upload benchmark: target=%dx%d source=%dx%d pitch=%d offset=%d thirty-frames-cpu-ns=%llu\n",
           w,h,sw,sh,pitch,offset,ns);
    gex_render_frame frame;
    EXPECT(!gex_renderer_frame(&frame));
    int video_width = vid_mode->width, video_height = vid_mode->height;
    gex_render_rect fit = gex_renderer_fit(video_width, video_height, w, h);
    for (int y = 0; y < video_height; ++y)
        for (int x = 0; x < video_width; ++x) {
            unsigned expected = 0;
            if (x >= fit.x && x < fit.x + fit.width && y >= fit.y && y < fit.y + fit.height) {
                int fx = (x - fit.x) * w / fit.width;
                int fy = (y - fit.y) * h / fit.height;
                int sx = fx * sw / w, sy = fy * sh / h;
                const uint8_t *p = pixels + sy * pitch + sx * 2;
                unsigned value = p[0] | (unsigned)p[1] << 8;
                expected = (value & 31) << 11 | (value & 0x3E0) << 1 | (value >> 10 & 31);
                const uint8_t *q = frame.pixels + fy * frame.pitch + fx * 2;
                EXPECT((q[0] | (unsigned)q[1] << 8) == expected);
            }
            EXPECT(vram_s[y * video_width + x] == expected);
        }
    gex_renderer_observe(observe_frame);
    EXPECT(!gex_renderer_upload_bgr555(pixels, sw, sh, pitch));
    EXPECT(observed == 1);
    gex_renderer_observe(NULL);
    observed = 0;
    memset(frame.pixels, 0, (size_t)frame.pitch * frame.height);
    EXPECT(!gex_renderer_commit());
    for (int i = 0; i < video_width * video_height; ++i) EXPECT(!vram_s[i]);
    free(storage);
}
int main(void) {
    EXPECT(!gex_renderer_init(gex_renderer_backend_get(), 1));
    check_upload(320,240,320,224,1408,0);
    check_upload(320,240,320,224,3,1);
    check_upload(640,480,320,224,0,0);
    check_upload(320,240,319,223,1,1);
    check_upload(427,240,320,224,0,0);
    gex_render_frame frame;
    EXPECT(!gex_renderer_frame(&frame));
    memset(frame.pixels,0,(size_t)frame.pitch * frame.height);
    EXPECT(!gex_renderer_commit());
    for (int i=0;i<vid_mode->width*vid_mode->height;++i) EXPECT(!vram_s[i]);
    gex_renderer_shutdown();
    printf("Dreamcast framebuffer regression: %s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
