#ifdef GEX_GLDC_DREAMCAST
#include <kos.h>
KOS_INIT_FLAGS(INIT_DEFAULT);
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "packet_gpu.h"
#if defined(GEX_GLDC_DREAMCAST) && defined(GEX_PACKET_GPU_TEST_API)
#include "private.h"
unsigned gex_opengl12_test_image(void *target);
#endif

static uint8_t atlas[0x100000], table[1024];
static uint16_t pixels[320 * 240];
static char error[256];
static int failures;
#define EXPECT(e)                                                                                  \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            fprintf(stderr, "OPENGL12 FAIL line %d: %s (%s)\n", __LINE__, #e, error);              \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
static void put(unsigned at, unsigned value) {
    atlas[at & 0xfffff] = value;
    atlas[(at + 1) & 0xfffff] = value >> 8;
}
static int read_target(const gex_packet_gpu *gpu, void *target) {
#ifdef GEX_GLDC_DREAMCAST
    if (gpu->native->present(target, error, sizeof error))
        return -1;
#ifdef GEX_PACKET_GPU_TEST_API
    TextureObject *texture = _glGetTexture(gex_opengl12_test_image(target));
    const volatile uint16_t *image = texture->data;
    for (unsigned y = 0; y < 240; ++y)
        for (unsigned x = 0; x < 320; ++x) {
            unsigned c = image[y * texture->rttWidth + x];
            pixels[y * 320 + x] = c >> 11 | (c >> 6 & 31) << 5 | (c & 31) << 10;
        }
    return 0;
#else
    snprintf(error, sizeof error, "Dreamcast pixel fixture requires the GLdc source tree");
    return -1;
#endif
#else
    return gpu->download(target, pixels, error, sizeof error);
#endif
}
static int rectangle(const gex_packet_gpu *gpu, void *target, const gex_gpu_material *material,
                     float x, float y, float width, float height, float u, float v, float uw,
                     float vh) {
    gex_gpu_vertex q[] = {{x, y, u, v},
                          {x + width, y, u + uw, v},
                          {x, y + height, u, v + vh},
                          {x + width, y + height, u + uw, v + vh}};
    return gpu->native->polygon(target, q, 0, 0, 320, 240, material, error, sizeof error);
}
int main(void) {
#ifdef GEX_GLDC_DREAMCAST
    vid_set_mode(DM_320x240, PM_RGB565);
#endif
    for (unsigned a = 0; a < 32; ++a)
        for (unsigned b = 0; b < 32; ++b)
            table[a * 32 + b] = a * b / 16 > 31 ? 31 : a * b / 16;
    const gex_packet_gpu *gpu = gex_packet_gpu_gldc();
    void *t = gpu->create(320, 240, error, sizeof error);
    if (!t) {
        fprintf(stderr, "OPENGL12 initialization failed: %s\n", error);
        return 1;
    }
    void *copy = gpu->create(320, 240, error, sizeof error);
    if (!copy) {
        fprintf(stderr, "OPENGL12 copy initialization failed: %s\n", error);
        gpu->destroy(t);
        return 1;
    }
    const gex_gpu_native_ops *native = gpu->native;
    EXPECT(!native->clear(t, error, sizeof error));
    gex_gpu_material solid = {.opcode = 0x60, .rgb = 0x0000ff};
    EXPECT(!rectangle(gpu, t, &solid, 0, 0, 30, 30, 0, 0, 0, 0));
    solid.rgb = 0xff0000;
    EXPECT(!rectangle(gpu, t, &solid, 0, 200, 30, 30, 0, 0, 0, 0));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!read_target(gpu, copy));
    EXPECT(pixels[10 * 320 + 10] == 31);
    EXPECT(pixels[210 * 320 + 10] == 0x7c00);
    EXPECT(pixels[100 * 320 + 100] == 0);
    EXPECT(!native->clear(t, error, sizeof error));
    solid.rgb = 0x00ff00;
    EXPECT(!rectangle(gpu, t, &solid, 1, 1, 4, 4, 0, 0, 0, 0));
    solid.rgb = 0x0000ff;
    for (unsigned i = 0; i < 600; ++i)
        EXPECT(!rectangle(gpu, t, &solid, 40, 40, 4, 4, 0, 0, 0, 0));
    EXPECT(!read_target(gpu, t));
    EXPECT(pixels[2 * 320 + 2] == 0x3e0 && pixels[41 * 320 + 41] == 31);
    EXPECT(!native->clear(copy, error, sizeof error));
    uint16_t asset[] = {0x3e0, 0, 0x7c00, 31};
    EXPECT(!native->blit(copy, 5, 6, 2, 2, (uint8_t *)asset, 4, 0, error, sizeof error));
    EXPECT(!read_target(gpu, copy));
    EXPECT(pixels[6 * 320 + 5] == 0x3e0 && pixels[6 * 320 + 6] == 0);
    EXPECT(pixels[7 * 320 + 5] == 0x7c00 && pixels[7 * 320 + 6] == 31);
    native->atlas_write(atlas, 0, sizeof atlas);
    memset(atlas, 0, sizeof atlas);
    put(0x60002, 31);
    put(0x60004, 0x83e0);
    for (unsigned row = 0; row < 16; ++row)
        memset(atlas + row * 2048 + 512, row & 1 ? 0x22 : 0x11, 128);
    gex_gpu_material material = {4, 0x3000, 0x2c, 0x808080, atlas, table};
    EXPECT(!rectangle(gpu, copy, &material, 40, 40, 16, 32, 0, 0, 16, 16));
    EXPECT(!read_target(gpu, copy));
    for (unsigned row = 0; row < 32; ++row)
        EXPECT(pixels[(40 + row) * 320 + 45] == (row / 2 & 1 ? 0x3e0 : 31));
    material.opcode = 0x2e;
    EXPECT(!rectangle(gpu, copy, &material, 60, 40, 16, 32, 0, 0, 16, 16));
    EXPECT(!read_target(gpu, copy));
    EXPECT(pixels[40 * 320 + 65] == 31);
    EXPECT((pixels[42 * 320 + 65] >> 5 & 31) >= 14 && (pixels[42 * 320 + 65] >> 5 & 31) <= 16);
    material.opcode = 0x2c;
    EXPECT(!rectangle(gpu, copy, &material, 90, 40, 8, 1, 0, 0, 8, 1));
    native->atlas_write(atlas, 0x60002, 2);
    put(0x60002, 0x7c00);
    EXPECT(!rectangle(gpu, copy, &material, 90, 42, 8, 1, 0, 0, 8, 1));
    EXPECT(!read_target(gpu, copy));
    EXPECT(pixels[40 * 320 + 90] == 31 && pixels[42 * 320 + 90] == 0x7c00);
    material.page = 4 | 128;
    for (unsigned row = 0; row < 16; ++row) {
        native->atlas_write(atlas, ((248 + row) & 255) * 2048 + 512, 17);
        memset(atlas + ((248 + row) & 255) * 2048 + 512, row + 1, 17);
        native->atlas_write(atlas, 0x60000 + (row + 1) * 2, 2);
        put(0x60000 + (row + 1) * 2, row + 1);
    }
    EXPECT(!rectangle(gpu, copy, &material, 130, 90, -16, 16, 0, 248, 16, 16));
    EXPECT(!read_target(gpu, copy));
    for (unsigned row = 0; row < 16; ++row)
        EXPECT(pixels[(90 + row) * 320 + 120] == row + 1);
    material.page = 4;
    native->atlas_write(atlas, 512, 128);
    memset(atlas + 512, 0x11, 128);
    for (unsigned i = 0; i < 600; ++i) {
        native->atlas_write(atlas, 0x60000 + i * 32, 32);
        put(0x60000 + i * 32 + 2, i % 31 + 1);
        material.clut = 0x3000 + i;
        EXPECT(!rectangle(gpu, copy, &material, 160 + i % 40, 140 + i / 40, 1, 1, 0, 0, 1, 1));
    }
    EXPECT(!read_target(gpu, copy));
    for (unsigned i = 0; i < 600; ++i)
        EXPECT(pixels[(140 + i / 40) * 320 + 160 + i % 40] == i % 31 + 1);
    EXPECT(!native->copy(t, copy, 255, error, sizeof error));
    EXPECT(!native->copy(t, t, 255, error, sizeof error));
    EXPECT(!read_target(gpu, t));
    EXPECT(pixels[140 * 320 + 160] == 1);
    EXPECT(!native->present(t, error, sizeof error));
    native->bind(t, atlas, 0);
    EXPECT(gpu->download(t, pixels, error, sizeof error) == -1);
    EXPECT(strstr(error, "readback is forbidden") != NULL);
    void *view = gpu->create(320, 240, error, sizeof error);
    EXPECT(view != NULL);
    if (view) {
        EXPECT(!native->clear(t, error, sizeof error));
        solid.rgb = 0x0000ff;
        EXPECT(!rectangle(gpu, t, &solid, 10, 10, 8, 8, 0, 0, 0, 0));
        gex_gpu_material feedback = {256, 0, 0x2c, 0x808080, atlas, table};
        EXPECT(!rectangle(gpu, t, &feedback, 50, 50, 8, 8, 10, 10, 8, 8));
        EXPECT(!native->copy(view, t, 255, error, sizeof error));
        EXPECT(!read_target(gpu, view));
        EXPECT(pixels[10 * 320 + 10] == 31 && pixels[50 * 320 + 50] == 31);
        solid.rgb = 0x00ff00;
        EXPECT(!rectangle(gpu, t, &solid, 80, 80, 8, 8, 0, 0, 0, 0));
        EXPECT(!rectangle(gpu, t, &feedback, 80, 80, 8, 8, 100, 100, 8, 8));
        EXPECT(!native->copy(view, t, 255, error, sizeof error));
        EXPECT(!read_target(gpu, view));
        EXPECT(pixels[80 * 320 + 80] == 0);
        native->bind(copy, atlas, 1);
        EXPECT(!native->clear(copy, error, sizeof error));
        solid.rgb = 0xff0000;
        EXPECT(!rectangle(gpu, copy, &solid, 20, 20, 4, 4, 0, 0, 0, 0));
        feedback.page = 16 | 256;
        EXPECT(!rectangle(gpu, t, &feedback, 100, 100, 4, 4, 20, 4, 4, 4));
        EXPECT(!native->copy(view, t, 255, error, sizeof error));
        EXPECT(!read_target(gpu, view));
        EXPECT(pixels[100 * 320 + 100] == 0x7c00);
        feedback.page = 256;
        solid.rgb = 0x0000ff;
        EXPECT(!rectangle(gpu, t, &solid, 10, 230, 4, 2, 0, 0, 0, 0));
        for (unsigned row = 232; row < 236; ++row) {
            native->atlas_write(atlas, row * 2048 + 20, 8);
            for (unsigned col = 10; col < 14; ++col)
                put(row * 2048 + col * 2, 0x3e0);
        }
        EXPECT(!rectangle(gpu, t, &feedback, 120, 120, 4, 6, 10, 230, 4, 6));
        EXPECT(!native->copy(view, t, 255, error, sizeof error));
        EXPECT(!read_target(gpu, view));
        EXPECT(pixels[120 * 320 + 120] == 31 && pixels[121 * 320 + 120] == 31);
        EXPECT(pixels[122 * 320 + 120] == 0x3e0 && pixels[125 * 320 + 120] == 0x3e0);
        feedback.page = 0;
        EXPECT(rectangle(gpu, t, &feedback, 150, 150, 4, 4, 0, 10, 4, 4) == -1);
        EXPECT(strstr(error, "indexed sampling") != NULL);
        uint16_t movie[16 * 8];
        for (unsigned i = 0; i < 16 * 8; ++i)
            movie[i] = i % 16 < 8 ? 0xf800 : 0x001f;
        EXPECT(!native->movie(view, (const uint8_t *)movie, 16, 8, 32, error, sizeof error));
        EXPECT(!read_target(gpu, view));
        EXPECT(pixels[50 * 320 + 50] == 31 && pixels[50 * 320 + 250] == 0x7c00);
        EXPECT(pixels[10 * 320 + 50] == 0 && pixels[220 * 320 + 50] == 0);
        gpu->destroy(view);
    }
    gpu->destroy(copy);
    gpu->destroy(t);
    for (unsigned i = 0; i < 32; ++i) {
        t = gpu->create(320, 240, error, sizeof error);
        EXPECT(t != NULL);
        if (!t)
            break;
        EXPECT(!native->clear(t, error, sizeof error));
        solid.rgb = 0x0000ff;
        EXPECT(!rectangle(gpu, t, &solid, 10, 10, 8, 8, 0, 0, 0, 0));
        EXPECT(!read_target(gpu, t));
        EXPECT(pixels[10 * 320 + 10] == 31);
        gpu->destroy(t);
    }
    fprintf(stderr, "OPENGL12 %s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
