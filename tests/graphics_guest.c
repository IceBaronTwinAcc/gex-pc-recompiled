#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "win32hle.h"
#include "renderer.h"
#include "graphics_guest.h"
#include "raster.h"

static int failures;
#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

static unsigned rgb565(unsigned pixel) {
    return ((pixel & 31) << 11) | ((pixel & 0x3E0) << 1) | ((pixel >> 10) & 31);
}

static void viewport(uint32_t atlas, int width, int height) {
    CHECK(!gex_renderer_resize(width, height));
    int mx = width / 2, my = height / 2;
    CHECK(gex_graphics_screen_point(&mx, &my));
    CHECK(mx == 160 && my == 120);
    mx = width - 1; my = height - 1;
    CHECK(gex_graphics_screen_point(&mx, &my));
    CHECK(mx == (width - 1) * 320 / width && my == (height - 1) * 240 / height);
    memset(hle_ptr(atlas), 0xFF, 0x100000);
    for (unsigned y = 8; y < 232; ++y)
        for (unsigned x = 0; x < 320; ++x)
            MEM16(atlas + y * 2048 + x * 2) = 1 + (y - 8) * 32 + x % 31;
    MEM32(0x451040) = MEM32(0x4A294C) = 0;
    unsigned counter = MEM32(0x487A90);
    uint32_t stack = g_esp;
    g_ebx = 0x12345678; g_ebp = 0x23456789;
    g_esi = 0x3456789A; g_edi = 0x456789AB;
    gex_graphics_flush();
    CHECK(g_esp == stack + 4);
    g_esp = stack;
    CHECK(g_ebx == 0x12345678 && g_ebp == 0x23456789);
    CHECK(g_esi == 0x3456789A && g_edi == 0x456789AB);
    CHECK(MEM32(0x487A90) == counter + 1);
    gex_render_frame frame;
    CHECK(!gex_renderer_frame(&frame));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            unsigned source = 1 + (y * 224 / height) * 32 + (x * 320 / width) % 31;
            const uint8_t *pixel = frame.pixels + y * frame.pitch + x * 2;
            CHECK((pixel[0] | (pixel[1] << 8)) == rgb565(source));
        }
    for (unsigned y = 0; y < 512; ++y)
        for (unsigned x = 0; x < 1024; ++x) {
            unsigned expected = y >= 8 && y < 232 && x < 320
                ? 1 + (y - 8) * 32 + x % 31 : 0xFFFF;
            CHECK(MEM16(atlas + y * 2048 + x * 2) == expected);
        }
    unsigned sequence = frame.sequence;
    MEM32(0x451040) = 1;
    gex_graphics_flush();
    g_esp = stack;
    CHECK(MEM32(0x487A90) == counter + 1);
    CHECK(!gex_renderer_frame(&frame) && frame.sequence == sequence);
    MEM32(0x451040) = 0;
    MEM32(0x4A294C) = 2;
    for (unsigned y = 0; y < 224; ++y)
        for (unsigned x = 0; x < 320; ++x)
            MEM16(atlas + 0x7C000 + y * 2048 + x * 2) = 0x7C00;
    gex_graphics_flush();
    g_esp = stack;
    CHECK(!gex_renderer_frame(&frame) && frame.sequence == sequence + 1);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const uint8_t *pixel = frame.pixels + y * frame.pitch + x * 2;
            CHECK((pixel[0] | (pixel[1] << 8)) == rgb565(0x7C00));
        }
    CHECK(MEM16(atlas) == 0xFFFF && MEM16(atlas + 232 * 2048) == 0xFFFF);
    MEM32(0x4A294C) = 0;
}

static void prepare_sprite(uint32_t atlas, uint32_t quad, int width, int height) {
    uint32_t texture = atlas + 0x80000;
    memset(hle_ptr(atlas), 0, 0x100000);
    for (unsigned i = 0; i < 256; ++i)
        MEM16(atlas + 0xF0000 + i * 2) = i ? (0x8000 | i) : 0;
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
            MEM8(texture + (y + 32) * 2048 + x + 32) = (x + y) % 3 ? 1 + x + y * 15 : 0;
    memset(hle_ptr(quad), 0, 40);
    MEM32(quad) = 0xFFFFFF;
    MEM8(quad + 4) = MEM8(quad + 5) = MEM8(quad + 6) = 128;
    MEM8(quad + 7) = 0x2C;
    MEM16(quad + 8) = MEM16(quad + 24) = 100;
    MEM16(quad + 16) = MEM16(quad + 32) = 100 + width;
    MEM16(quad + 10) = MEM16(quad + 18) = 80;
    MEM16(quad + 26) = MEM16(quad + 34) = 80 + height;
    MEM8(quad + 12) = MEM8(quad + 28) = 32;
    MEM8(quad + 20) = MEM8(quad + 36) = 48;
    MEM8(quad + 13) = MEM8(quad + 21) = 32;
    MEM8(quad + 29) = MEM8(quad + 37) = 48;
    MEM16(quad + 14) = 0xF0000 / 32;
    MEM16(quad + 22) = 0x90;
    MEM32(0x4A2F78) = texture;
}

static void scaled_sprite(uint32_t atlas, uint32_t quad, int width, int height) {
    prepare_sprite(atlas, quad, width, height);
    uint32_t args[] = {quad, 0};
    hle_call_guest(0x446890, 2, args);
    unsigned mismatches = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            unsigned sx = (x * ((16 << 16) / width)) >> 16;
            unsigned sy = (y * ((16 << 16) / height)) >> 16;
            unsigned index = MEM8(atlas + 0x80000 + (sy + 32) * 2048 + sx + 32);
            unsigned expected = index ? 0x8000 | index : 0;
            unsigned actual = MEM16(atlas + (y + 80) * 2048 + (x + 100) * 2);
            if (actual != expected) {
                if (!mismatches)
                    fprintf(stderr, "%dx%d sprite pixel (%d,%d): %04X, expected %04X\n",
                            width, height, x, y, actual, expected);
                ++mismatches;
            }
        }
    CHECK(mismatches == 0);
}

static void sprite_sampling(uint32_t atlas, uint32_t quad) {
    const int sizes[][2] = {{32, 32}, {23, 21}, {8, 8}, {47, 39}};
    for (unsigned format = 0; format < 3; ++format)
        for (unsigned flip = 0; flip < 4; ++flip)
            for (unsigned variant = 0; variant < 4; ++variant) {
                prepare_sprite(atlas, quad, sizes[variant][0], sizes[variant][1]);
                for (unsigned y = 0; y < 256; ++y)
                    for (unsigned x = 0; x < 256; ++x) {
                        unsigned index = (x + y * 7) % (format == 0 ? 16 : 256);
                        uint32_t row = atlas + 0x80000 + y * 2048;
                        if (format == 0) {
                            unsigned shift = (x & 1) * 4;
                            MEM8(row + x / 2) = (MEM8(row + x / 2) & ~(15u << shift)) |
                                               (index << shift);
                        } else if (format == 1) MEM8(row + x) = index;
                        else MEM16(row + x * 2) = index ? 0x8000 | index : 0;
                    }
                for (unsigned i = 0; i < 256; ++i)
                    MEM16(atlas + 0x78000 + i * 2) = i ? 0x8000 | i : 0;
                MEM16(quad + 14) = 0x78000 / 32;
                MEM16(quad + 22) = 16 | (format << 7);
                int left = variant == 3 ? -5 : 100, top = variant == 3 ? 3 : 80;
                for (unsigned i = 0; i < 4; ++i) {
                    MEM16(quad + 8 + i * 8) = left + (((i ^ flip) & 1) ? sizes[variant][0] : 0);
                    MEM16(quad + 10 + i * 8) = top + (((i ^ flip) & 2) ? sizes[variant][1] : 0);
                    MEM8(quad + 12 + i * 8) = (variant == 2 ? 224 : 31) + ((i & 1) ? 32 : 0);
                    MEM8(quad + 13 + i * 8) = (variant == 2 ? 224 : 193) + ((i & 2) ? 32 : 0);
                }
                for (unsigned factor = 0; factor < 32; ++factor)
                    for (unsigned value = 0; value < 32; ++value)
                        MEM8(0x4A2B50 + factor * 32 + value) =
                            factor * value / 16 > 31 ? 31 : factor * value / 16;
                gex_raster_state state = {
                    hle_ptr(atlas), hle_ptr(0x4A2B50), 0, 0, 0, 320, 240, 0
                };
                gex_raster *r = gex_raster_create(320, 224);
                CHECK(r != NULL);
                if (!r) return;
                gex_raster_packet(r, hle_ptr(quad), &state);
                uint32_t args[] = {quad};
                hle_call_guest(0x444DC0, 1, args);
                unsigned mismatches = 0;
                const uint8_t *pixels = gex_raster_pixels(r);
                for (unsigned y = 0; y < 224; ++y)
                    for (unsigned x = 0; x < 320; ++x) {
                        unsigned offset = (y * 320 + x) * 2;
                        unsigned actual = pixels[offset] | (unsigned)pixels[offset + 1] << 8;
                        mismatches += actual != MEM16(atlas + (y + 8) * 2048 + x * 2);
                    }
                if (mismatches)
                    fprintf(stderr, "sprite format=%u flip=%u variant=%u: %u pixel mismatches\n",
                            format, flip, variant, mismatches);
                CHECK(!mismatches);
                gex_raster_destroy(r);
            }
}

static void loading_trails(uint32_t atlas, uint32_t quad) {
    prepare_sprite(atlas, quad, 8, 8);
    MEM32(0x4A2958) = 1;
    MEM32(0x4A2990) = MEM32(0x455B84) = 0x4A2A50;
    uint32_t args[] = {quad};
    hle_call_guest(0x445140, 1, args);
    CHECK(MEM16(atlas + 80 * 2048 + 101 * 2) == 0x8003);
    CHECK(MEM16(atlas + 80 * 2048 + 101 * 2) != 0);
    MEM16(quad + 8) = MEM16(quad + 24) = 124;
    MEM16(quad + 16) = MEM16(quad + 32) = 132;
    hle_call_guest(0x445140, 1, args);
    CHECK(MEM16(atlas + 80 * 2048 + 125 * 2) != 0);
    for (unsigned y = 8; y < 232; ++y)
        for (unsigned x = 0; x < 320; ++x)
            if (x < 124 || x >= 132 || y < 80 || y >= 88)
                CHECK(MEM16(atlas + y * 2048 + x * 2) == 0);

    uint32_t old_stack = g_esp;
    uint32_t sentinel = atlas + 50 * 2048 + 10 * 2;
    MEM16(sentinel) = 0x7C1F;
    MEM32(0x4A2958) = 0;
    gex_graphics_begin_frame();
    CHECK(MEM16(sentinel) == 0x7C1F);
    MEM32(0x4A2958) = 1;
    MEM32(0x4A2990) = 0x4A29C0;
    gex_graphics_begin_frame();
    CHECK(MEM16(sentinel) == 0x7C1F);
    MEM32(0x4A2990) = MEM32(0x455B84);
    MEM32(0x451040) = 1;
    gex_graphics_begin_frame();
    CHECK(MEM16(sentinel) == 0x7C1F);
    MEM32(0x451040) = 0;
    MEM32(0x4A294C) = 2;
    gex_graphics_begin_frame();
    CHECK(MEM16(sentinel) == 0x7C1F);
    MEM32(0x4A294C) = 0;
    MEM16(atlas) = MEM16(atlas + 232 * 2048) = MEM16(atlas + 50 * 2048 + 640) = 0x1234;
    gex_graphics_begin_frame();
    CHECK(MEM16(sentinel) == 0 && g_esp == old_stack);
    CHECK(MEM16(atlas) == 0x1234 && MEM16(atlas + 232 * 2048) == 0x1234);
    CHECK(MEM16(atlas + 50 * 2048 + 640) == 0x1234);
    CHECK(MEM16(atlas + 0xF0000 + 2) == 0x8001);
    MEM32(0x4A2958) = 0;
    MEM32(0x4A2990) = 0;
}

static unsigned frame_pixel(const gex_render_frame *frame, int x, int y) {
    const uint8_t *p = frame->pixels + y * frame->pitch + x * 2;
    return p[0] | (unsigned)p[1] << 8;
}

static void hud_layout(void) {
    uint32_t object = hle_va(hle_alloc(0x210));
    const unsigned fields[] = {0x78, 0x7C, 0xC8, 0xCC};
    const int sizes[][2] = {{427, 240}, {854, 480}, {1920, 1080},
                            {2560, 1080}, {3440, 1440}, {320, 224}, {160, 112},
                            {640, 448}, {240, 427}};
    MEM32(0x4A2A38) = 700u << 16; MEM32(0x4A2A1C) = 300u << 16;
    MEM32(0x4A2974) = 123u << 16; MEM32(0x4A2988) = 45u << 16;
    for (unsigned size = 0; size < sizeof sizes / sizeof *sizes; ++size) {
        int width = sizes[size][0], height = sizes[size][1];
        CHECK(!gex_graphics_set_render_size(width, height));
        int64_t scale = (int64_t)height * 65536 / 224;
        int64_t fit = (int64_t)width * 65536 / 320;
        if (scale > fit) scale = fit;
        int native = scale >= 65536;
        if (native) scale = 65536;
        for (int anchor = 0; anchor < 4; ++anchor)
            for (int scaled = 0; scaled < 2; ++scaled) {
                uint32_t saved[] = {(700u + (anchor == 1 ? 296 : anchor == 2 ? 144 : 28)) << 16,
                                    (300u + 32) << 16, 0xA000, 0x8000};
                saved[0] += 0x1000; saved[1] += 0x1000;
                for (int i = 0; i < 4; ++i) MEM32(object + fields[i]) = saved[i];
                uint32_t stack = g_esp, eax = g_eax, esi = g_esi;
                CHECK(gex_graphics_hud_begin(object, anchor, scaled));
                int fraction = anchor == 1 ? 3 : anchor == 2 ? 1 : anchor == 3 ? 2 : 0;
                int64_t expected_x = (anchor == 1 ? 296 : anchor == 2 ? 144 : 28) * scale;
                if (!native)
                    expected_x += ((int64_t)width * 65536 - 320 * scale) * fraction / 3;
                expected_x -= (expected_x % 65536 + 65536) % 65536;
                CHECK(MEM32(object + 0x78) == (123u << 16) + (uint32_t)expected_x);
                CHECK(MEM32(object + 0x7C) == (45u << 16) + 8 * 65536 +
                      (uint32_t)(24 * scale / 65536 * 65536));
                CHECK(!(MEM32(object + 0x78) & 0xFFFF));
                CHECK(!(MEM32(object + 0x7C) & 0xFFFF));
                CHECK(MEM32(object + 0xC8) == (scaled ? 0xA000 * scale / 65536 : scale));
                CHECK(MEM32(object + 0xCC) == (scaled ? 0x8000 * scale / 65536 : scale));
                MEM32(object + 0x54) = 7; /* Animation updates must survive restoration. */
                gex_graphics_hud_end(object);
                for (int i = 0; i < 4; ++i) CHECK(MEM32(object + fields[i]) == saved[i]);
                CHECK(MEM32(object + 0x54) == 7);
                CHECK(g_esp == stack && g_eax == eax && g_esi == esi);
                CHECK(MEM32(0x4A2A38) == 700u << 16 && MEM32(0x4A2A1C) == 300u << 16);
            }
    }
    gex_graphics_shutdown();
    uint32_t saved = MEM32(object + 0x78);
    CHECK(!gex_graphics_hud_begin(object, 1, 0));
    gex_graphics_hud_end(object);
    CHECK(MEM32(object + 0x78) == saved);
    hle_free(hle_ptr(object));
}

static void expanded_viewport(uint32_t atlas, uint32_t quad) {
    CHECK(!gex_graphics_set_render_size(640, 448));
    MEM32(0x450010) = 8; MEM32(0x450014) = 232;
    for (unsigned factor = 0; factor < 32; ++factor)
        for (unsigned value = 0; value < 32; ++value)
            MEM8(0x4A2B50 + factor * 32 + value) =
                factor * value / 16 > 31 ? 31 : factor * value / 16;
    prepare_sprite(atlas, quad, 8, 8);
    uint32_t args[] = {quad};
    hle_call_guest(0x445140, 1, args);
    uint32_t stack = g_esp;
    gex_graphics_flush(); g_esp = stack;
    gex_render_frame frame;
    CHECK(!gex_renderer_frame(&frame));
    CHECK(frame.width == 640 && frame.height == 448);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            unsigned index = MEM8(atlas + 0x80000 + (y * 2 + 32) * 2048 + x * 2 + 32);
            unsigned expected = index ? rgb565(0x8000 | index) : 0;
            CHECK(frame_pixel(&frame, 100 + x, 72 + y) == expected);
        }
    CHECK(frame_pixel(&frame, 108, 72) == 0);
    unsigned neighbor = frame_pixel(&frame, 101, 72);
    uint32_t extended = hle_va(hle_alloc(40));
    memcpy(hle_ptr(extended), hle_ptr(quad), 40);
    for (int i = 0; i < 4; ++i) {
        MEM16(extended + 8 + i * 8) += 400;
        MEM16(extended + 10 + i * 8) += 300;
    }
    uint32_t extended_args[] = {extended};
    hle_call_guest(0x445140, 1, extended_args);
    gex_graphics_flush(); g_esp = stack;
    CHECK(frame_pixel(&frame, 501, 372) == neighbor);
    CHECK(frame_pixel(&frame, 508, 372) == 0);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
            CHECK(MEM8(atlas + 0x80000 + (y + 32) * 2048 + x + 32) ==
                  ((x + y) % 3 ? 1 + x + y * 15 : 0));
    hle_free(hle_ptr(extended));
    /* Original display mode changes do not reset the level viewport. */
    const uint32_t modes[] = {0, 0x320240, 0x640480};
    for (unsigned i = 0; i < sizeof modes / sizeof *modes; ++i) {
        MEM32(stack + 4) = modes[i];
        gex_graphics_resolution(); g_esp = stack;
        CHECK(g_eax == 1 && MEM32(0x451038) == modes[i]);
        CHECK(!gex_renderer_frame(&frame) && frame.width == 640 && frame.height == 448);
    }
    CHECK(gex_graphics_set_render_size(8193, 240) == -1);
    CHECK(!gex_renderer_frame(&frame) && frame.width == 640 && frame.height == 448);

    /* External framebuffer writes affect one pixel, regardless of viewport. */
    MEM16(atlas + 80 * 2048 + 100 * 2) = 0x7C00;
    gex_graphics_flush(); g_esp = stack;
    CHECK(frame_pixel(&frame, 100, 72) == rgb565(0x7C00));
    CHECK(frame_pixel(&frame, 101, 72) == neighbor);

    /* Rasterize the second draw plane, then exercise the actual pause-copy path. */
    MEM32(0x450010) = 240; MEM32(0x450014) = 480;
    MEM16(quad + 10) += 240; MEM16(quad + 18) += 240;
    MEM16(quad + 26) += 240; MEM16(quad + 34) += 240;
    hle_call_guest(0x445140, 1, args);
    MEM32(0x4A294C) = 2;
    gex_graphics_flush(); g_esp = stack;
    CHECK(frame_pixel(&frame, 101, 72) == neighbor);
    CHECK(MEM16(atlas + 80 * 2048 + 101 * 2) ==
          MEM16(atlas + 320 * 2048 + 101 * 2));
    MEM32(0x4A294C) = 0;
    MEM32(0x450010) = 0; MEM32(0x450014) = 240;
    CHECK(!gex_renderer_frame(&frame));
    unsigned sequence = frame.sequence;
    MEM32(0x451040) = 1;
    gex_graphics_flush(); g_esp = stack;
    CHECK(!gex_renderer_frame(&frame) && frame.sequence == sequence);
    MEM32(0x451040) = 0;
    gex_graphics_packet_arena();
    quad = MEM32(0x4A2AE0);
    args[0] = quad;
    prepare_sprite(atlas, quad, 8, 8);
    MEM32(0x4A2958) = 1;
    MEM32(0x4A2990) = MEM32(0x455B84) = 0x4A2A50;
    hle_call_guest(0x445140, 1, args);
    gex_graphics_flush(); g_esp = stack;
    CHECK(frame_pixel(&frame, 202, 144) != 0);
    MEM16(quad + 8) = MEM16(quad + 24) = 124;
    MEM16(quad + 16) = MEM16(quad + 32) = 132;
    hle_call_guest(0x445140, 1, args);
    gex_graphics_flush(); g_esp = stack;
    CHECK(frame_pixel(&frame, 202, 144) == 0);
    CHECK(frame_pixel(&frame, 250, 144) != 0);
    MEM32(0x4A2958) = 0;
    gex_graphics_shutdown();
}

static void world_submission(void) {
    uint32_t scene = hle_va(hle_alloc(64)), map = hle_va(hle_alloc(44 + 9 * 6 * 4));
    uint32_t chunk = hle_va(hle_alloc(8 + 64 * 2));
    uint32_t tiles = hle_va(hle_alloc(4)), definitions = hle_va(hle_alloc(32));
    uint32_t textures = hle_va(hle_alloc(24)), cel = hle_va(hle_alloc(32));
    uint32_t pages = hle_va(hle_alloc(8));
    MEM32(scene + 4) = map; MEM32(0x4A2990) = scene;
    MEM32(map + 4) = 2304u << 16; MEM32(map + 8) = 1536u << 16;
    MEM32(map + 12) = 9; MEM32(map + 16) = 6;
    for (int i = 0; i < 54; ++i) MEM32(map + 44 + i * 4) = chunk;
    for (int i = 0; i < 64; ++i) MEM16(chunk + 8 + i * 2) = 1;
    MEM32(tiles) = definitions; MEM16(definitions + 16) = 1;
    MEM32(textures + 12) = cel; MEM32(0x4A2AE8) = tiles;
    MEM16(cel + 20) = 1; MEM8(cel + 22) = MEM8(cel + 23) = 32;
    MEM8(cel + 16) = 2; MEM32(0x460E08) = pages; MEM16(pages) = 0x100;
    MEM8(0x4A2AF8) = MEM8(0x4A2AF9) = MEM8(0x4A2AFA) = 128;
    MEM16(0x4A2A94) = MEM16(0x4A2A96) = 0;
    const int sizes[][2] = {{320, 224}, {640, 448}, {1920, 1080}};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        int width = sizes[i][0], height = sizes[i][1];
        CHECK(!gex_graphics_set_render_size(width, height));
        CHECK(gex_graphics_view_width() == width && gex_graphics_view_height() == height + 16);
        hle_call_guest(0x441130, 0, NULL);
        uint32_t start = MEM32(0x4A2AE0), end = MEM32(0x4A2ADC);
        CHECK(end - start >= 1048576 && MEM32(0x4A2AE4) == start);
        uint32_t args[] = {map, textures, 0, 0};
        uint32_t stack = g_esp;
        g_ebx = 0x12345678; g_esi = 0x23456789; g_edi = 0x3456789A; g_ebp = 0x456789AB;
        hle_call_guest(0x43FB40, 4, args);
        CHECK(g_esp == stack);
        CHECK(g_ebx == 0x12345678 && g_esi == 0x23456789);
        CHECK(g_edi == 0x3456789A && g_ebp == 0x456789AB);
        /* The original layer splicer closes this open tail later in a frame. */
        MEM32(MEM32(0x46BC74)) = 0xFFFFFF;
        int sprites = 0, nodes = 0, max_x = 0, max_y = 0;
        uint32_t node = MEM32(0x46BC64);
        while ((node & 0xFFFFFF) != 0xFFFFFF && nodes++ < 10000) {
            CHECK(node >= start && node + 20 <= end);
            if (node < start || node + 20 > end) break;
            if ((MEM8(node + 7) & ~2) == 0x64) {
                ++sprites;
                int x = (int16_t)MEM16(node + 8), y = (int16_t)MEM16(node + 10);
                if (x > max_x) max_x = x;
                if (y > max_y) max_y = y;
            }
            node = MEM32(node);
        }
        CHECK((node & 0xFFFFFF) == 0xFFFFFF && nodes < 10000);
        CHECK(sprites == ((width + 31) / 32) * ((height + 47) / 32));
        CHECK(max_x == ((width + 31) / 32 - 1) * 32);
        CHECK(max_y == ((height + 47) / 32 - 1) * 32);
        CHECK(MEM32(0x4A2AE4) > start && MEM32(0x4A2AE4) <= end);
        uint32_t cursor = MEM32(0x4A2AE4);
        args[2] = 2304u << 16; args[3] = 1536u << 16;
        hle_call_guest(0x43FB40, 4, args);
        CHECK(MEM32(0x46BC64) == 0xFFFFFF && MEM32(0x46BC60) == 0xFFFFFF);
        CHECK(MEM32(0x4A2AE4) == cursor);
    }
    CHECK(!gex_graphics_set_render_size(640, 448));
    const uint32_t addresses[] = {0x4A2974, 0x4A2988, 0x4A293C, 0x4A2978};
    const uint32_t saved[] = {500u << 16, 400u << 16, 480u << 16, 380u << 16};
    for (int i = 0; i < 4; ++i) MEM32(addresses[i]) = saved[i];
    MEM32(0x4A2A38) = 0x12345678; MEM32(0x4A2A1C) = 0x23456789;
    uint32_t solid = hle_va(hle_alloc(20));
    MEM8(solid + 4) = 248; MEM8(solid + 7) = 0x60;
    MEM16(solid + 8) = 600; MEM16(solid + 10) = 400;
    MEM16(solid + 12) = MEM16(solid + 14) = 8;
    gex_graphics_draw_packet(solid);
    uint32_t stack = g_esp;
    gex_graphics_flush(); g_esp = stack;
    gex_render_frame frame;
    CHECK(!gex_renderer_frame(&frame));
    CHECK(frame_pixel(&frame, 600, 392) != 0);
    gex_graphics_scene_begin();
    CHECK(MEM32(0x4A2974) == 340u << 16 && MEM32(0x4A2988) == 288u << 16);
    CHECK(MEM32(0x4A293C) == 320u << 16 && MEM32(0x4A2978) == 268u << 16);
    gex_graphics_scene_begin();
    gex_graphics_scene_end();
    gex_graphics_flush(); g_esp = stack;
    CHECK(!gex_renderer_frame(&frame));
    CHECK(frame_pixel(&frame, 600, 392) == 0);
    for (int i = 0; i < 4; ++i) CHECK(MEM32(addresses[i]) == saved[i]);
    CHECK(MEM32(0x4A2A38) == 0x12345678 && MEM32(0x4A2A1C) == 0x23456789);
    MEM32(map + 4) = 320u << 16; MEM32(map + 8) = 240u << 16;
    gex_graphics_scene_begin();
    CHECK(!MEM32(0x4A2974) && !MEM32(0x4A2988));
    gex_graphics_shutdown();
    for (int i = 0; i < 4; ++i) CHECK(MEM32(addresses[i]) == saved[i]);
    CHECK(gex_graphics_view_width() == 320 && gex_graphics_view_height() == 240);
    hle_free(hle_ptr(scene)); hle_free(hle_ptr(map)); hle_free(hle_ptr(chunk));
    hle_free(hle_ptr(tiles)); hle_free(hle_ptr(definitions)); hle_free(hle_ptr(textures));
    hle_free(hle_ptr(cel)); hle_free(hle_ptr(pages));
    hle_free(hle_ptr(solid));
}

static void camera_boundaries(void) {
    uint32_t fixture = hle_va(hle_alloc(0x300));
    uint32_t scene = fixture, map = fixture + 0x40, object = fixture + 0x80;
    MEM32(scene + 4) = map;
    MEM32(0x4A2990) = scene; MEM32(0x4A2964) = 3;
    MEM32(map + 4) = 4096u << 16; MEM32(map + 8) = 2048u << 16;
    const uint32_t addresses[] = {0x4A2974, 0x4A2988, 0x4A293C, 0x4A2978};
    const int sizes[][2] = {{320, 224}, {427, 240}, {854, 480}, {1237, 701},
                            {2560, 1080}, {240, 427}, {1, 1}};
    const int positions[][2] = {{128, 96}, {1000, 700}, {1984, 1296}};
    const int lower[] = {128, 96}, upper[] = {2304, 1536};
    for (unsigned size = 0; size < sizeof sizes / sizeof *sizes; ++size)
        for (unsigned position = 0; position < sizeof positions / sizeof *positions; ++position) {
            int width = sizes[size][0], height = sizes[size][1];
            int x = positions[position][0], y = positions[position][1];
            CHECK(!gex_graphics_set_render_size(width, height));
            const uint32_t saved[] = {x * 65536u, y * 65536u,
                                     (x - 20) * 65536u, (y - 20) * 65536u};
            for (int i = 0; i < 4; ++i) MEM32(addresses[i]) = saved[i];
            MEM32(0x4A2A38) = saved[0]; MEM32(0x4A2A1C) = saved[1];
            gex_graphics_camera_begin();
            for (unsigned direction = 0; direction < 4; ++direction) {
                unsigned axis = direction / 2;
                MEM32(object + 0x98) = direction;
                MEM32(object + 0x78) = (axis ? x + 100 :
                    direction & 1 ? upper[0] : lower[0]) * 65536u;
                MEM32(object + 0x7C) = (!axis ? y + 80 :
                    direction & 1 ? upper[1] : lower[1]) * 65536u;
                gex_graphics_camera_barrier(object);
            }
            gex_graphics_scene_begin();
            const int extents[] = {width, height + 16}, original[] = {320, 240};
            for (int axis = 0; axis < 2; ++axis) {
                int expected = (positions[position][axis] * 2 - extents[axis] + original[axis]) / 2;
                int limit = upper[axis] - extents[axis];
                if (limit < lower[axis]) limit = lower[axis];
                if (expected > limit) expected = limit;
                if (expected < lower[axis]) expected = lower[axis];
                CHECK(MEM32(addresses[axis]) == expected * 65536u);
                CHECK(MEM32(addresses[axis + 2]) == (expected - 20) * 65536u);
            }
            gex_graphics_scene_end();
            for (int i = 0; i < 4; ++i) CHECK(MEM32(addresses[i]) == saved[i]);
            CHECK(MEM32(0x4A2A38) == saved[0] && MEM32(0x4A2A1C) == saved[1]);
            /* Runtime-only observations expire; authored records are tested below. */
            MEM32(scene + 4) = map + 8;
            MEM32(map + 12) = 4096u << 16; MEM32(map + 16) = 2048u << 16;
            gex_graphics_scene_begin();
            CHECK(MEM32(0x4A2974) != 128u << 16 || x != 128 || width == 320);
            gex_graphics_scene_end();
            MEM32(scene + 4) = map;
            gex_graphics_camera_begin();
            gex_graphics_scene_begin();
            if (x == 128 && width > 320) CHECK(MEM32(0x4A2974) < 128u << 16);
            gex_graphics_scene_end();
            gex_graphics_shutdown();
        }
    const int heights[] = {480, 112};
    for (unsigned size = 0; size < sizeof heights / sizeof *heights; ++size) {
        int height = heights[size];
        CHECK(!gex_graphics_set_render_size(854, height));
        gex_graphics_packet_arena();
        MEM32(0x4A2974) = MEM32(0x4A293C) = 128u << 16;
        MEM32(0x4A2988) = MEM32(0x4A2978) = 96u << 16;
        gex_graphics_camera_begin();
        for (unsigned direction = 0; direction < 4; ++direction) {
            MEM32(object + 0x98) = direction;
            MEM32(object + 0x78) = (direction < 2 ? direction ? 448 : 128 : 228) * 65536u;
            MEM32(object + 0x7C) = (direction >= 2 ? direction == 3 ? 336 : 96 : 176) * 65536u;
            gex_graphics_camera_barrier(object);
        }
        gex_graphics_scene_begin();
        gex_graphics_scene_end();
        uint32_t packet = MEM32(0x4A2AE0);
        memset(hle_ptr(packet), 0, 80);
        MEM32(0x450008) = 0; MEM32(0x45000C) = 320;
        MEM32(0x450010) = 8; MEM32(0x450014) = 232;
        MEM8(packet + 4) = 248; MEM8(packet + 7) = 0x60;
        MEM16(packet + 10) = 8;
        MEM16(packet + 12) = 854; MEM16(packet + 14) = height;
        gex_graphics_draw_packet(packet);
        MEM32(0x4A2AE4) = packet + 40;
        CHECK(gex_graphics_hud_begin(object, 0, 0));
        MEM32(0x4A2AE4) = packet + 80;
        gex_graphics_hud_end(object);
        MEM8(packet + 45) = 248; MEM8(packet + 47) = 0x60;
        MEM16(packet + 48) = size ? 700 : 300;
        MEM16(packet + 50) = size ? 50 : 210;
        MEM16(packet + 52) = MEM16(packet + 54) = 8;
        gex_graphics_draw_packet(packet + 40);
        uint32_t stack = g_esp;
        gex_graphics_end_frame(); gex_graphics_flush(); g_esp = stack;
        gex_render_frame frame;
        CHECK(!gex_renderer_frame(&frame));
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < 854; ++x) {
                unsigned expected = x < 320 && y < 232 ? rgb565(31) : 0;
                int hud_x = size ? 700 : 600, hud_y = size ? 42 : 404;
                int hud_size = size ? 8 : 16;
                if (x >= hud_x && x < hud_x + hud_size &&
                    y >= hud_y && y < hud_y + hud_size) expected = rgb565(31 << 5);
                CHECK(frame_pixel(&frame, x, y) == expected);
            }
        gex_graphics_shutdown();
    }
    MEM32(0x4A2990) = 0;
    hle_free(hle_ptr(fixture));
}

static void authored_camera_boundaries(void) {
    uint32_t fixture = hle_va(hle_alloc(0x500));
    memset(hle_ptr(fixture), 0, 0x500);
    uint32_t scene = fixture, map = fixture + 0x40, group = fixture + 0x80;
    uint32_t groups = fixture + 0xC0, object = fixture + 0x100;
    uint32_t records = fixture + 0x340, properties = fixture + 0x380;
    uint32_t presets = fixture + 0x3C0, defaults = fixture + 0x3E0;
    uint32_t saved_groups = MEM32(0x4A2A78), saved_count = MEM32(0x4626FC);
    uint32_t saved_presets = MEM32(0x4A2934);
    MEM32(scene + 4) = map;
    MEM32(0x4A2990) = scene; MEM32(0x4A2964) = 3;
    MEM32(map + 4) = 4096u << 16; MEM32(map + 8) = 2048u << 16;
    MEM32(0x4A2A78) = groups; MEM32(0x4626FC) = 1;
    MEM32(groups) = group;
    MEM32(group) = records; MEM32(group + 4) = 0x419870; MEM32(group + 8) = 4;
    MEM32(0x4A2934) = presets; MEM32(presets + 4) = defaults;
    MEM32(defaults) = 12; MEM32(defaults + 4) = 1;
    const int points[][2] = {{128, 768}, {2304, 768}, {420, 96}, {420, 1536}};
    for (unsigned i = 0; i < 4; ++i) {
        MEM32(records + i * 16) = (uint32_t)points[i][0] << 16 | points[i][1];
        MEM32(records + i * 16 + 4) = 206; /* Original camera-marker archetype. */
        MEM32(records + i * 16 + 8) = 0x03000001;
        MEM32(records + i * 16 + 12) = properties + i * 16;
        MEM32(properties + i * 16) = 0x4000000C;
        MEM32(properties + i * 16 + 4) = i;
    }
    CHECK(!gex_graphics_set_render_size(854, 480));
    for (int x = 128; x <= 397; ++x) {
        MEM32(0x4A2974) = MEM32(0x4A293C) = x * 65536u;
        MEM32(0x4A2988) = MEM32(0x4A2978) = 682u << 16;
        gex_graphics_camera_begin();
        gex_graphics_scene_begin();
        int expected = x - 267;
        if (expected < 128) expected = 128;
        CHECK(MEM32(0x4A2974) == expected * 65536u);
        gex_graphics_scene_end();
        CHECK(MEM32(0x4A2974) == x * 65536u);
    }
    MEM32(0x4A2974) = MEM32(0x4A293C) = 334u << 16;
    /* A live moved or disabled marker supersedes its authored position. */
    for (unsigned direction = 0; direction <= 4; direction += 4) {
        gex_graphics_camera_begin();
        MEM32(object + 0x18C) = records;
        MEM32(object + 0x78) = 200u << 16; MEM32(object + 0x7C) = 768u << 16;
        MEM32(object + 0x98) = 0;
        gex_graphics_camera_barrier(object);
        MEM32(object + 0x78) = 160u << 16; MEM32(object + 0x7C) = 768u << 16;
        MEM32(object + 0x98) = direction;
        gex_graphics_camera_barrier(object);
        gex_graphics_scene_begin();
        CHECK(MEM32(0x4A2974) == (direction ? 67u : 160u) << 16);
        gex_graphics_scene_end();
    }
    /* Wrong-side planes and markers outside the native segment do not clamp. */
    const uint32_t excluded[] = {(400u << 16) | 768, (128u << 16) | 1000};
    for (unsigned i = 0; i < sizeof excluded / sizeof *excluded; ++i) {
        MEM32(records) = excluded[i];
        gex_graphics_camera_begin();
        gex_graphics_scene_begin();
        CHECK(MEM32(0x4A2974) == 67u << 16);
        gex_graphics_scene_end();
    }
    MEM32(records) = (128u << 16) | 768;
    /* Removing or replacing level data must not leave a cached boundary. */
    for (unsigned mode = 0; mode < 3; ++mode) {
        gex_graphics_camera_begin();
        if (mode == 0) MEM32(scene + 4) = map + 8;
        if (mode == 1) MEM32(group + 8) = 0;
        if (mode == 2) MEM32(records + 4) = 0;
        MEM32(map + 12) = 4096u << 16; MEM32(map + 16) = 2048u << 16;
        gex_graphics_scene_begin();
        CHECK(MEM32(0x4A2974) == 67u << 16);
        gex_graphics_scene_end();
        MEM32(scene + 4) = map; MEM32(group + 8) = 4; MEM32(records + 4) = 206;
    }
    gex_graphics_camera_begin();
    gex_graphics_scene_begin();
    CHECK(MEM32(0x4A2974) == 128u << 16);
    gex_graphics_scene_end();
    MEM32(group + 8) = 1;
    for (unsigned direction = 0; direction < 4; ++direction) {
        MEM32(0x4A2974) = MEM32(0x4A293C) = 1000u << 16;
        MEM32(0x4A2988) = MEM32(0x4A2978) = 700u << 16;
        const int planes[] = {900, 1350, 650, 1000};
        const int expected[] = {900, 496, 650, 504};
        unsigned axis = direction / 2;
        MEM32(records) = (axis ? 1100u : (unsigned)planes[direction]) << 16 |
                        (axis ? (unsigned)planes[direction] : 780u);
        MEM32(properties + 4) = direction;
        gex_graphics_camera_begin();
        gex_graphics_scene_begin();
        CHECK(MEM32(axis ? 0x4A2988 : 0x4A2974) == expected[direction] * 65536u);
        gex_graphics_scene_end();
    }
    gex_graphics_shutdown();
    MEM32(0x4A2990) = 0;
    MEM32(0x4A2A78) = saved_groups; MEM32(0x4626FC) = saved_count;
    MEM32(0x4A2934) = saved_presets;
    hle_free(hle_ptr(fixture));
}

static void camera_motion(void) {
    uint32_t fixture = hle_va(hle_alloc(0x300));
    memset(hle_ptr(fixture), 0, 0x300);
    uint32_t scene = fixture, map = fixture + 0x40, object = fixture + 0x80;
    MEM32(scene + 4) = map;
    MEM32(0x4A2990) = scene; MEM32(0x4A2964) = 3;
    MEM32(map + 4) = 4096u << 16; MEM32(map + 8) = 2048u << 16;
    const uint32_t addresses[] = {0x4A2974, 0x4A2988, 0x4A293C, 0x4A2978};
    const int extents[] = {427, 256}, native_extents[] = {320, 240};
    const int plane[] = {128, 2304, 96, 1536};
    CHECK(!gex_graphics_set_render_size(427, 240));
    /* Walk away from each edge in native two-pixel half-frame steps. The
     * widened view must remain still until both motion endpoints clear it. */
    for (unsigned direction = 0; direction < 4; ++direction) {
        unsigned axis = direction / 2;
        int sign = direction & 1 ? -1 : 1;
        int margin = extents[axis] - native_extents[axis] + !(direction & 1);
        margin /= 2;
        int edge = plane[direction] - (direction & 1 ? native_extents[axis] : 0);
        for (int motion_sign = -1; motion_sign <= 1; ++motion_sign)
        for (int step = 0; step < 70; ++step) {
            int position[] = {1000, 700};
            position[axis] = edge + step * sign;
            uint32_t saved[4];
            for (int i = 0; i < 4; ++i)
                MEM32(addresses[i]) = saved[i] = position[i % 2] * 65536u;
            MEM16(0x4A2A96) = (uint16_t)(axis ? -3 : 2 * sign * motion_sign);
            MEM16(0x4A2A94) = (uint16_t)(axis ? 2 * sign * motion_sign : -3);
            uint16_t motion[] = {MEM16(0x4A2A96), MEM16(0x4A2A94)};
            gex_graphics_camera_begin();
            MEM32(object + 0x98) = direction;
            MEM32(object + 0x78) = (axis ? 1100 : plane[direction]) * 65536u;
            MEM32(object + 0x7C) = (axis ? plane[direction] : 780) * 65536u;
            gex_graphics_camera_barrier(object);
            gex_graphics_scene_begin();
            int movement = step - margin, previous = step - 4 * motion_sign - margin;
            if (movement < 0) movement = 0;
            if (previous < 0) previous = 0;
            int expected = (movement - previous) / 2 * sign;
            CHECK((int16_t)MEM16(axis ? 0x4A2A94 : 0x4A2A96) == expected);
            CHECK((int16_t)MEM16(axis ? 0x4A2A96 : 0x4A2A94) == -3);
            gex_graphics_scene_end();
            for (int i = 0; i < 4; ++i) CHECK(MEM32(addresses[i]) == saved[i]);
            CHECK(MEM16(0x4A2A96) == motion[0] && MEM16(0x4A2A94) == motion[1]);
        }
    }
    gex_graphics_shutdown();
    MEM32(0x4A2990) = 0;
    hle_free(hle_ptr(fixture));
}

static void hud_packets(uint32_t atlas, int results, int hardware) {
    uint32_t object = hle_va(hle_alloc(0x210));
    uint32_t stack = g_esp;
    const int sizes[][2] = {{320, 224}, {854, 480}, {1920, 1080}, {2560, 1080}};
    for (unsigned size = 0; size < sizeof sizes / sizeof *sizes; ++size)
    for (int anchor = 0; anchor < 4; ++anchor) {
        int width = sizes[size][0], height = sizes[size][1];
        int scale = height / 224;
        if (scale > width / 320) scale = width / 320;
        CHECK(!gex_graphics_set_render_size(width, height));
        gex_render_rect menu = gex_renderer_fit(width, height, 320, 224);
        int mx = menu.x, my = menu.y;
        CHECK(gex_graphics_screen_point(&mx, &my));
        CHECK(mx == 0 && my == 0);
        mx = menu.x + menu.width; my = menu.y;
        CHECK(!gex_graphics_screen_point(&mx, &my));
        mx = menu.x; my = menu.y + menu.height;
        CHECK(!gex_graphics_screen_point(&mx, &my));
        gex_graphics_packet_arena();
        uint32_t packet = MEM32(0x4A2AE0);
        prepare_sprite(atlas, packet, 16, 11);
        MEM32(0x450008) = 0; MEM32(0x45000C) = 320;
        MEM32(0x450010) = 8; MEM32(0x450014) = 232;
        MEM32(0x4A2964) = results ? 68 : 3; MEM32(0x4A2990) = MEM32(0x4A2958) = 0;
        MEM32(0x451040) = MEM32(0x4A294C) = 0;
        MEM32(object + 0x78) = MEM32(0x4A2A38) + 100 * 65536;
        MEM32(object + 0x7C) = MEM32(0x4A2A1C) + 80 * 65536;
        gex_graphics_end_frame();
        if (hardware) CHECK(!gex_graphics_enable_gpu());
        if (anchor == 2) MEM32(0x4A2AE4) = MEM32(0x4A2ADC) - 40;
        CHECK(gex_graphics_hud_begin(object, anchor, 0));
        MEM32(0x4A2AE4) = packet + 40;
        gex_graphics_hud_end(object);
        gex_graphics_draw_packet(packet);
        gex_graphics_end_frame();
        gex_graphics_flush(); g_esp = stack;
        gex_render_frame frame;
        CHECK(!gex_renderer_frame(&frame));
        gex_raster *original = gex_raster_create(320, 224);
        CHECK(original != NULL);
        if (original) {
            gex_raster_state state = {
                (const uint8_t *)hle_ptr(atlas), (const uint8_t *)ADDR(0x4A2B50),
                0x90, 0, 8, 320, 232, 0
            };
#if !GEX_ENABLE_SOFTWARE_RENDERER
            CHECK(!gex_raster_enable_gpu(original, gex_packet_gpu_get()));
#endif
            gex_raster_packet(original, (const uint8_t *)hle_ptr(packet), &state);
            int fraction = anchor == 1 ? 3 : anchor == 2 ? 1 : anchor == 3 ? 2 : 0;
            int offset = (width - 320 * scale) * fraction / 3;
            const uint8_t *pixels = gex_raster_pixels(original);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    int sx = (x - offset) / scale, sy = y / scale;
                    unsigned expected = 0;
                    if (x >= offset && sx < 320 && sy < 224) {
                        const uint8_t *p = pixels + (sy * 320 + sx) * 2;
                        expected = rgb565(p[0] | p[1] << 8);
                    }
                    CHECK(frame_pixel(&frame, x, y) == expected);
                }
            gex_raster_destroy(original);
        }
        gex_graphics_shutdown();
    }
    hle_free(hle_ptr(object));
    MEM32(0x4A2964) = 3;
}

static void portal_player_draws(void) {
    uint32_t fixture = hle_va(hle_alloc(0x500));
    uint32_t object = fixture, asset = fixture + 0x210, animations = fixture + 0x400;
    uint32_t frames = fixture + 0x234, frame = fixture + 0x240;
    uint32_t components = fixture + 0x260, component = fixture + 0x270;
    uint32_t cel = fixture + 0x280, pages = fixture + 0x2A0;
    uint32_t scene = fixture + 0x2B0, map = fixture + 0x2C0;
    MEM32(object + 0xC) = asset; MEM32(object + 0x50) = 0x30;
    MEM32(asset) = animations;
    MEM32(animations + 0x30 * 4) = frames; MEM32(frames) = frame;
    MEM32(frame + 0x18) = components; MEM32(components) = component;
    MEM32(component + 8) = cel;
    MEM32(cel) = MEM32(cel + 4) = 16u << 16;
    MEM8(cel + 16) = 0x42; MEM16(cel + 20) = 1;
    MEM8(cel + 22) = MEM8(cel + 23) = 16;
    MEM32(0x460F6C) = pages; MEM16(pages) = 0x100;
    MEM32(scene + 4) = map;
    MEM32(map + 4) = 4096u << 16; MEM32(map + 8) = 2048u << 16;
    MEM32(0x4A2964) = 3; MEM32(0x4A2990) = scene;
    MEM32(0x455C4C) = MEM32(0x4A2878) = 0;
    const uint32_t addresses[] = {0x4A2974, 0x4A2988, 0x4A293C, 0x4A2978};
    const int sizes[][2] = {{320, 224}, {427, 240}, {854, 480}, {1237, 701}};
    for (unsigned size = 0; size < sizeof sizes / sizeof *sizes; ++size)
        for (int edge = 0; edge < 3; ++edge)
            for (unsigned state = 1; state <= 2; ++state) {
                int width = sizes[size][0], height = sizes[size][1];
                CHECK(!gex_graphics_set_render_size(width, height));
                gex_graphics_packet_arena();
                MEM32(0x4A2B18) = fixture + 0x3E0;
                MEM32(0x4A2B14) = fixture + 0x3E4;
                unsigned x = edge == 0 ? 500 : edge == 1 ? 0 : 3776;
                unsigned y = edge == 0 ? 400 : edge == 1 ? 0 : 1808;
                const uint32_t saved[] = {x << 16, y << 16, x << 16, y << 16};
                for (int i = 0; i < 4; ++i) MEM32(addresses[i]) = saved[i];
                MEM16(0x4A2A96) = 2; MEM16(0x4A2A94) = (uint16_t)-3;
                MEM32(0x4A2A38) = x << 16; MEM32(0x4A2A1C) = y << 16;
                MEM32(object + 0x78) = (x + 100) << 16;
                MEM32(object + 0x7C) = (y + 80) << 16;
                MEM32(object + 0xC8) = MEM32(object + 0xCC) = 0xC000;
                MEM32(object + 0x70) = state;
                uint32_t stack = g_esp, args[] = {object};
                uint8_t expected[36];
                gex_graphics_scene_begin();
                uint32_t camera_x = MEM32(0x4A2974), camera_y = MEM32(0x4A2988);
                uint32_t packet = MEM32(0x4A2AE4);
                hle_call_guest(0x416320, 1, args);
                CHECK(MEM32(0x4A2AE4) == packet + 80);
                CHECK((int16_t)MEM16(packet + 8) ==
                      (int)((MEM32(object + 0x78) - camera_x) >> 16));
                CHECK((int16_t)MEM16(packet + 10) ==
                      (int)((MEM32(object + 0x7C) - camera_y) >> 16));
                memcpy(expected, hle_ptr(packet + 4), sizeof expected);
                CHECK(MEM32(0x4A2974) == camera_x && MEM32(0x4A2988) == camera_y);
                gex_graphics_scene_end();
                for (int i = 0; i < 4; ++i) CHECK(MEM32(addresses[i]) == saved[i]);
                CHECK(MEM16(0x4A2A96) == 2 && (int16_t)MEM16(0x4A2A94) == -3);
                packet = MEM32(0x4A2AE4);
                hle_call_guest(0x416320, 1, args);
                CHECK(MEM32(0x4A2AE4) == packet + 80);
                CHECK(!memcmp(expected, hle_ptr(packet + 4), sizeof expected));
                for (int i = 0; i < 4; ++i) CHECK(MEM32(addresses[i]) == saved[i]);
                CHECK(MEM16(0x4A2A96) == 2 && (int16_t)MEM16(0x4A2A94) == -3);
                CHECK(MEM32(0x4A2A38) == x << 16 && MEM32(0x4A2A1C) == y << 16);
                CHECK(MEM32(object + 0x78) == (x + 100) << 16);
                CHECK(MEM32(object + 0x7C) == (y + 80) << 16);
                CHECK(MEM32(object + 0xC8) == 0xC000 && MEM32(object + 0xCC) == 0xC000);
                CHECK(g_esp == stack);
                gex_graphics_shutdown();
            }
    MEM32(0x4A2990) = 0;
    hle_free(hle_ptr(fixture));
}

static void screen_layouts(uint32_t atlas, uint32_t packet) {
    const int sizes[][2] = {{320, 224}, {427, 240}, {854, 480}, {1920, 1080},
                            {2560, 1080}, {3440, 1440}, {240, 427}, {1, 1}};
    const uint32_t levels[] = {63, 3, 68, 49, 50, 51, 52, 53, 54, 55, 66};
    uint32_t stack = g_esp;
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        int width = sizes[i][0], height = sizes[i][1];
        CHECK(!gex_graphics_set_render_size(width, height));
        gex_graphics_packet_arena();
        packet = MEM32(0x4A2AE0);
        memset(hle_ptr(atlas), 0, 0x100000);
        memset(hle_ptr(packet), 0, 40);
        MEM8(packet + 7) = 0x60;
        MEM16(packet + 10) = 8; MEM16(packet + 12) = 160; MEM16(packet + 14) = 224;
        MEM32(0x450010) = 8; MEM32(0x450014) = 232;
        for (unsigned screen = 0; screen < sizeof levels / sizeof *levels; ++screen) {
            int loading = screen == 1;
            MEM32(0x4A2964) = levels[screen];
            MEM32(0x4A2958) = loading;
            MEM32(0x455B84) = 0x4A2A50;
            MEM32(0x4A2990) = loading ? 0x4A2A50 : 0x4A29C0;
            MEM32(0x4A29C4) = 0x1234 + screen;
            CHECK(gex_graphics_view_width() == 320 && gex_graphics_view_height() == 240);
            /* Results reuse the gameplay HUD; other authored screens do not. */
            for (int anchor = 0; anchor < 4; ++anchor)
                for (int scaled = 0; scaled < 2; ++scaled) {
                    MEM32(0x463678) = 0x12345678;
                    CHECK(gex_graphics_hud_begin(0x463600, anchor, scaled) == (levels[screen] == 68));
                    gex_graphics_hud_end(0x463600);
                    CHECK(MEM32(0x463678) == 0x12345678);
                }
            uint32_t camera = MEM32(0x4A2974);
            gex_graphics_scene_begin();
            CHECK(MEM32(0x4A2974) == camera);
            gex_graphics_packet_arena();
            gex_graphics_begin_frame();
            MEM32(0x4A2AE4) = loading ? MEM32(0x4A2ADC) - 40 : packet;
            gex_graphics_backdrop_begin();
            gex_graphics_packet_allocated(packet + 40);
            MEM32(0x4A2AE4) = packet + 40;
            gex_graphics_backdrop_end();
            /* Static callbacks can submit during updates, before scene draws. */
            gex_graphics_scene_begin();
            MEM8(packet + 4) = 248; MEM8(packet + 6) = 0;
            MEM16(packet + 8) = 0;
            MEM16(packet + 10) = 8; MEM16(packet + 12) = 160; MEM16(packet + 14) = 224;
            gex_graphics_draw_packet(packet);
            MEM8(packet + 4) = 0; MEM8(packet + 6) = 248;
            MEM16(packet + 8) = 160;
            gex_graphics_draw_packet(packet);
            /* Fade-out replays the prior display list without resubmission. */
            gex_graphics_end_frame();
            /* Destination state changes before the displayed scene retires. */
            MEM32(0x4A2964) = 3; MEM32(0x4A2958) = 0;
            CHECK(gex_graphics_view_width() == 320);
            /* A palette-only callback must not retire cached static CELs. */
            gex_graphics_backdrop_begin();
            gex_graphics_backdrop_end();
            MEM8(packet + 4) = 248; MEM8(packet + 6) = 0;
            MEM16(packet + 8) = 0;
            gex_graphics_draw_packet(packet);
            MEM8(packet + 4) = 0; MEM8(packet + 6) = 248;
            MEM16(packet + 8) = 160;
            gex_graphics_draw_packet(packet);
            uint32_t foreground = packet + 40;
            memcpy(hle_ptr(foreground), hle_ptr(packet), 40);
            MEM8(foreground + 4) = MEM8(foreground + 5) = MEM8(foreground + 6) = 248;
            MEM16(foreground + 8) = 144; MEM16(foreground + 10) = 104;
            MEM16(foreground + 12) = MEM16(foreground + 14) = 32;
            gex_graphics_draw_packet(foreground);
            gex_graphics_end_frame();
            gex_graphics_flush(); g_esp = stack;
            gex_render_frame frame;
            CHECK(!gex_renderer_frame(&frame));
            gex_render_rect rect = gex_renderer_fit(width, height, 320, 224);
            int left = rect.x + 144 * rect.width / 320;
            int right = rect.x + 176 * rect.width / 320;
            int top = rect.y + 96 * rect.height / 224;
            int bottom = rect.y + 128 * rect.height / 224;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    unsigned expected = x % 320 < 160 ? 31 : 0x7C00;
                    if (levels[screen] == 66) {
                        int split = rect.x + 160 * rect.width / 320;
                        expected = x < split ? 31 : 0x7C00;
                        if (x < rect.x || x >= rect.x + rect.width ||
                            y < rect.y || y >= rect.y + rect.height)
                            expected = 0;
                    }
                    if (x >= left && x < right && y >= top && y < bottom)
                        expected = 0x7FFF;
                    CHECK(frame_pixel(&frame, x, y) == rgb565(expected));
                }
            MEM8(packet + 5) = 0;
        }
        MEM32(0x4A2964) = 3; MEM32(0x4A2958) = 0;
        /* Reusing the scene object with a different map ends fixed layout. */
        MEM32(0x4A29C4) = 0x4321;
        CHECK(gex_graphics_view_width() == width);
        MEM32(0x4A2964) = 48;
        CHECK(gex_graphics_view_width() == width);
        MEM32(0x4A2964) = 56;
        CHECK(gex_graphics_view_width() == width);
        MEM32(0x4A2964) = 3;
        MEM32(0x4A2990) = 0;
        CHECK(gex_graphics_view_width() == width);
        /* Pause must retain and dim the complete expanded world, not just 320x224. */
        memset(hle_ptr(atlas), 0, 0x100000);
        gex_graphics_end_frame();
        MEM8(packet + 4) = 248; MEM8(packet + 5) = MEM8(packet + 6) = 0;
        MEM16(packet + 8) = 0; MEM16(packet + 10) = 8;
        MEM16(packet + 12) = 320; MEM16(packet + 14) = 224;
        gex_graphics_draw_packet(packet);
        for (int n = 0; n < 138 * 27; ++n) MEM16(0x4517F0 + n * 2) = 0x7FFF;
        MEM16(0x4517F0) = 0;
        hle_call_guest(0x4053A0, 0, NULL);
        MEM32(0x4A294C) = 2;
        gex_graphics_flush(); g_esp = stack;
        gex_render_frame frame;
        CHECK(!gex_renderer_frame(&frame));
        gex_render_rect rect = gex_renderer_fit(width, height, 320, 224);
        int left = rect.x + 91 * rect.width / 320;
        int right = rect.x + 229 * rect.width / 320;
        int top = rect.y + 98 * rect.height / 224;
        int bottom = rect.y + 125 * rect.height / 224;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                unsigned expected = (31 & 0x7BDE) >> 1;
                if (x >= left && x < right && y >= top && y < bottom &&
                    ((x - left) * 138 / (right - left) || (y - top) * 27 / (bottom - top)))
                    expected = 0x7FFF;
                CHECK(frame_pixel(&frame, x, y) == rgb565(expected));
            }
        /* Repeated pause presentation must neither dim again nor move the banner. */
        unsigned center = frame_pixel(&frame, width / 2, height / 2);
        gex_graphics_flush(); g_esp = stack;
        CHECK(frame_pixel(&frame, width / 2, height / 2) == center);
        MEM32(0x4A294C) = 0;
        gex_graphics_shutdown();
    }
    MEM32(0x4A2990) = MEM32(0x455B84) = 0;
}

static void menu_clipping(uint32_t atlas, int hardware) {
    const int sizes[][2] = {{320, 224}, {854, 480}, {240, 427}};
    uint32_t stack = g_esp;
    for (unsigned size = 0; size < sizeof sizes / sizeof *sizes; ++size) {
        int width = sizes[size][0], height = sizes[size][1];
        CHECK(!gex_graphics_set_render_size(width, height));
        gex_graphics_packet_arena();
        uint32_t packet = MEM32(0x4A2AE0);
        memset(hle_ptr(atlas), 0, 0x100000);
        memset(hle_ptr(packet), 0, 120);
        MEM32(0x4A2964) = 63;
        MEM32(0x4A2990) = MEM32(0x4A2958) = 0;
        MEM32(0x451040) = MEM32(0x4A294C) = 0;
        MEM32(0x450008) = 0; MEM32(0x45000C) = 320;
        MEM32(0x450010) = 8; MEM32(0x450014) = 232;
        if (hardware) CHECK(!gex_graphics_enable_gpu());
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t p = packet + i * 40;
            MEM8(p + 7) = 0x60;
            MEM16(p + 10) = 8; MEM16(p + 14) = 224;
            MEM16(p + 12) = i == 1 ? 320 : 160;
        }
        MEM8(packet + 4) = MEM8(packet + 5) = MEM8(packet + 6) = 248;
        MEM8(packet + 84) = 248;
        MEM16(packet + 88) = 160;
        for (int pass = 0; pass < 3; ++pass) {
            gex_graphics_begin_frame();
            MEM32(0x4A2AE4) = pass == 1 ? MEM32(0x4A2ADC) - 40 : packet;
            gex_graphics_menu_clip_begin(50, 197);
            MEM32(0x4A2AE4) = packet + 40;
            gex_graphics_menu_clip_end();
            if (pass == 2) gex_graphics_end_frame();
            gex_graphics_draw_packet(packet + 40);
            gex_graphics_draw_packet(packet);
            gex_graphics_draw_packet(packet + 80);
            gex_graphics_end_frame();
            gex_graphics_flush(); g_esp = stack;
            gex_render_frame frame;
            CHECK(!gex_renderer_frame(&frame));
            gex_render_rect rect = gex_renderer_fit(width, height, 320, 224);
            int split = rect.x + 160 * rect.width / 320;
            int top = rect.y + 50 * rect.height / 224;
            int bottom = rect.y + 197 * rect.height / 224;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    unsigned expected = 0;
                    if (x >= rect.x && x < rect.x + rect.width &&
                        y >= rect.y && y < rect.y + rect.height) {
                        if (x >= split) expected = 31;
                        else if (pass == 2 || (y >= top && y < bottom)) expected = 0x7FFF;
                    }
                    CHECK(frame_pixel(&frame, x, y) == rgb565(expected));
                }
        }
        gex_graphics_shutdown();
    }
}

static void scene_blackout(uint32_t atlas, int hardware) {
    CHECK(!gex_graphics_set_render_size(320, 224));
    gex_graphics_packet_arena();
    uint32_t packet = MEM32(0x4A2AE0), stack = g_esp;
    memset(hle_ptr(atlas), 0, 0x100000);
    memset(hle_ptr(packet), 0, 40);
    MEM8(packet + 4) = 248; MEM8(packet + 7) = 0x60;
    MEM16(packet + 10) = 8; MEM16(packet + 12) = 320; MEM16(packet + 14) = 224;
    MEM32(0x4A2964) = 63;
    MEM32(0x4A2990) = MEM32(0x4A2958) = 0;
    MEM32(0x451040) = MEM32(0x4A294C) = 0;
    MEM32(0x450008) = 0; MEM32(0x45000C) = 320;
    MEM32(0x450010) = 8; MEM32(0x450014) = 232;
    if (hardware) CHECK(!gex_graphics_enable_gpu());
    for (int pass = 0; pass < 4; ++pass) {
        gex_graphics_hold_black(pass == 1 || pass == 2);
        gex_graphics_frame_shade(pass >= 2 ? 128 : 255);
        gex_graphics_begin_frame();
        gex_graphics_draw_packet(packet);
        gex_graphics_end_frame();
        gex_graphics_flush(); g_esp = stack;
        gex_render_frame frame;
        CHECK(!gex_renderer_frame(&frame));
        unsigned expected = pass == 0 ? 0xF800 : pass == 3 ? 0x7800 : 0;
        for (int y = 0; y < frame.height; ++y)
            for (int x = 0; x < frame.width; ++x)
                CHECK(frame_pixel(&frame, x, y) == expected);
    }
    gex_graphics_shutdown();
}

static void screen_transitions(uint32_t atlas) {
    const int sizes[][2] = {{427, 240}, {854, 480}, {240, 427}};
    uint32_t stack = g_esp;
    for (unsigned size = 0; size < sizeof sizes / sizeof *sizes; ++size) {
        int width = sizes[size][0], height = sizes[size][1];
        CHECK(!gex_graphics_set_render_size(width, height));
        gex_graphics_packet_arena();
        uint32_t packet = MEM32(0x4A2AE0);
        memset(hle_ptr(atlas), 0, 0x100000);
        memset(hle_ptr(packet), 0, 40);
        MEM8(packet + 7) = 0x60;
        MEM32(0x451040) = MEM32(0x4A294C) = MEM32(0x4A2958) = 0;
        MEM32(0x450008) = 0; MEM32(0x45000C) = 320;
        MEM32(0x450010) = 8; MEM32(0x450014) = 232;
        MEM32(0x455B84) = 0x4A2A50;
        MEM32(0x4A2990) = 0x4A29C0;
        for (unsigned level = 49; level <= 55; ++level) {
            MEM32(0x4A2964) = 66;
            MEM32(0x4A29C4) = 0x1234;
            gex_graphics_begin_frame();
            MEM8(packet + 4) = 248; MEM8(packet + 5) = MEM8(packet + 6) = 0;
            MEM16(packet + 8) = 0; MEM16(packet + 10) = 8;
            MEM16(packet + 12) = 320; MEM16(packet + 14) = 224;
            gex_graphics_draw_packet(packet);
            MEM32(0x450010) = MEM16(packet + 10) = 248;
            MEM32(0x450014) = 472;
            gex_graphics_draw_packet(packet);
            MEM32(0x450010) = MEM16(packet + 10) = 8;
            MEM32(0x450014) = 232;
            gex_graphics_end_frame();
            MEM32(0x4A2964) = level;
            MEM32(0x4A29C4) = 0x5678;
            gex_graphics_begin_frame();
            MEM8(packet + 4) = 0; MEM8(packet + 5) = 248;
            /* The map covers its authored picture, not the side borders. */
            MEM16(packet + 12) = 160;
            gex_graphics_draw_packet(packet);
            MEM16(packet + 8) = 160;
            gex_graphics_draw_packet(packet);
            MEM16(packet + 8) = 144; MEM16(packet + 10) = 104;
            MEM16(packet + 12) = MEM16(packet + 14) = 32;
            MEM8(packet + 5) = 0; MEM8(packet + 6) = 248;
            gex_graphics_draw_packet(packet);
            gex_graphics_end_frame();
            /* Incremental drawing and outgoing fades retain the same map. */
            MEM32(0x4A2964) = 3;
            gex_graphics_begin_frame();
            gex_graphics_end_frame();
            gex_graphics_flush(); g_esp = stack;
            gex_render_frame frame;
            CHECK(!gex_renderer_frame(&frame));
            gex_render_rect rect = gex_renderer_fit(width, height, 320, 224);
            int stale = 0;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    if (x < rect.x || x >= rect.x + rect.width ||
                        y < rect.y || y >= rect.y + rect.height)
                        stale += frame_pixel(&frame, x, y) != 0;
            CHECK(stale == 0);
            CHECK(frame_pixel(&frame, width / 2, height / 2) == rgb565(0x7C00));
            CHECK(frame_pixel(&frame, rect.x + rect.width / 4,
                              rect.y + rect.height / 2) == rgb565(0x3E0));
            /* A later second-plane presentation cannot revive the static. */
            MEM32(0x4A294C) = 2;
            gex_graphics_flush(); g_esp = stack;
            CHECK(!gex_renderer_frame(&frame));
            int retained = 0;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    retained += frame_pixel(&frame, x, y) != 0;
            CHECK(retained == 0);
            MEM32(0x4A294C) = 0;
        }
        gex_graphics_shutdown();
    }
    MEM32(0x4A2964) = 3;
    MEM32(0x4A2990) = MEM32(0x455B84) = 0;
}

static void transition_layout(uint32_t atlas) {
    const int sizes[][2] = {{427, 240}, {854, 480}, {1920, 1080}, {240, 427}};
    uint32_t stack = g_esp;
    for (unsigned size = 0; size < sizeof sizes / sizeof *sizes; ++size) {
        int width = sizes[size][0], height = sizes[size][1];
        CHECK(!gex_graphics_set_render_size(width, height));
        gex_graphics_packet_arena();
        uint32_t packet = MEM32(0x4A2AE0);
        memset(hle_ptr(atlas), 0, 0x100000);
        memset(hle_ptr(packet), 0, 40);
        MEM8(packet + 7) = 0x60; MEM8(packet + 4) = 248;
        MEM16(packet + 10) = 8; MEM16(packet + 12) = 160;
        MEM16(packet + 14) = 224;
        MEM32(0x451040) = MEM32(0x4A294C) = MEM32(0x4A2958) = 0;
        MEM32(0x450008) = 0; MEM32(0x45000C) = 320;
        MEM32(0x450010) = 8; MEM32(0x450014) = 232;
        MEM32(0x4A2964) = 49;
        MEM32(0x4A2990) = 0x4A29C0;
        MEM32(0x4A29C4) = 0x1234;
        gex_graphics_backdrop_begin();
        gex_graphics_packet_allocated(packet + 40);
        MEM32(0x4A2AE4) = packet + 40;
        gex_graphics_backdrop_end();
        MEM16(packet + 8) = 0;
        gex_graphics_draw_packet(packet);
        MEM16(packet + 8) = 160;
        gex_graphics_draw_packet(packet);
        gex_graphics_end_frame();

        gex_graphics_transition_begin();
        /* The destination ID is not a reliable transition-layout indicator. */
        MEM32(0x4A2964) = 3;
        CHECK(gex_graphics_view_width() == 320 && gex_graphics_view_height() == 240);
        gex_graphics_begin_frame();
        MEM16(packet + 8) = 0;
        gex_graphics_draw_packet(packet);
        MEM16(packet + 8) = 160;
        gex_graphics_draw_packet(packet);
        gex_graphics_end_frame();
        /* Cached static remains confined across repeated animation frames. */
        gex_graphics_begin_frame();
        MEM8(packet + 4) = 0;
        MEM16(packet + 8) = 0; MEM16(packet + 12) = 320;
        MEM16(packet + 14) = 96;
        gex_graphics_draw_packet(packet);
        MEM16(packet + 10) = 136;
        gex_graphics_draw_packet(packet);
        gex_graphics_end_frame();
        gex_graphics_flush(); g_esp = stack;
        gex_render_frame frame;
        CHECK(!gex_renderer_frame(&frame));
        gex_render_rect rect = gex_renderer_fit(width, height, 320, 224);
        int top = rect.y + 96 * rect.height / 224;
        int bottom = rect.y + 128 * rect.height / 224;
        int mismatches = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                unsigned expected = x >= rect.x && x < rect.x + rect.width &&
                    y >= top && y < bottom ? rgb565(31) : 0;
                mismatches += frame_pixel(&frame, x, y) != expected;
            }
        CHECK(mismatches == 0);
        gex_graphics_transition_end();
        MEM32(0x4A29C4) = 0x5678;
        CHECK(gex_graphics_view_width() == width);
        gex_graphics_shutdown();
    }
    MEM32(0x4A2990) = 0;
}

static void recycled_backdrops(uint32_t atlas) {
    const unsigned reuse[][2] = {{0, 40}, {40, 80}, {80, 120}, {0, 120}, {160, 40}};
    uint32_t stack = g_esp;
    for (unsigned test = 0; test < sizeof reuse / sizeof *reuse; ++test) {
        CHECK(!gex_graphics_set_render_size(854, 480));
        gex_graphics_packet_arena();
        uint32_t packet = MEM32(0x4A2AE0);
        MEM32(0x4A2964) = 63;
        MEM32(0x4A2990) = 0x4A29C0;
        MEM32(0x4A29C4) = 0x1234;
        MEM32(0x451040) = MEM32(0x4A294C) = 0;
        MEM32(0x450008) = 0; MEM32(0x45000C) = 320;
        MEM32(0x450010) = 8; MEM32(0x450014) = 232;
        gex_graphics_backdrop_begin();
        gex_graphics_packet_allocated(packet + 120);
        MEM32(0x4A2AE4) = packet + 120;
        gex_graphics_backdrop_end();
        gex_graphics_end_frame();
        gex_graphics_backdrop_begin();
        gex_graphics_backdrop_end();
        MEM32(0x4A2AE4) = test == 4 ? MEM32(0x4A2ADC) - 8 : packet + reuse[test][0];
        gex_graphics_packet_allocated(packet + reuse[test][1]);
        MEM32(0x4A2AE4) = packet + reuse[test][1];
        gex_raster *reference = gex_raster_create(854, 480);
        CHECK(reference != NULL);
        if (reference) {
            for (unsigned i = 0; i < 3; ++i) {
                uint32_t p = packet + i * 40;
                memset(hle_ptr(p), 0, 40);
                MEM8(p + 7) = 0x60;
                MEM8(p + 4 + i) = 248;
                MEM16(p + 8) = 20 + i * 80;
                MEM16(p + 10) = 40;
                MEM16(p + 12) = MEM16(p + 14) = 24;
                gex_graphics_draw_packet(p);
                unsigned begin = test == 4 ? 0 : reuse[test][0];
                int reused = i * 40 >= begin && i * 40 < reuse[test][1];
                gex_raster_state state = {
                    (const uint8_t *)hle_ptr(atlas), (const uint8_t *)ADDR(0x4A2B50),
                    0, 0, 8, 320, 232, 0, reused ? 1 : 2
                };
                gex_raster_packet(reference, (const uint8_t *)hle_ptr(p), &state);
            }
            gex_graphics_end_frame();
            gex_graphics_flush(); g_esp = stack;
            gex_render_frame frame;
            CHECK(!gex_renderer_frame(&frame));
            const uint8_t *pixels = gex_raster_pixels(reference);
            for (int y = 0; y < 480; ++y)
                for (int x = 0; x < 854; ++x) {
                    const uint8_t *p = pixels + (y * 854 + x) * 2;
                    CHECK(frame_pixel(&frame, x, y) == rgb565(p[0] | p[1] << 8));
                }
            gex_raster_destroy(reference);
        }
        gex_graphics_shutdown();
    }
    MEM32(0x4A2964) = 3;
    MEM32(0x4A2990) = MEM32(0x455B84) = 0;
}

static void gpu_solid(uint32_t packet, int x, int y, int w, int h, unsigned rgb) {
    memset(hle_ptr(packet), 0, 40);
    MEM32(packet + 4) = 0x60000000 | rgb;
    MEM16(packet + 8) = x; MEM16(packet + 10) = y + 8;
    MEM16(packet + 12) = w; MEM16(packet + 14) = h;
    CHECK(gex_graphics_draw_packet(packet) == 1);
}

static void atlas_upload(uint32_t atlas, uint32_t rectangle) {
    uint8_t *expected = malloc(0x100000);
    CHECK(expected != NULL);
    if (!expected) return;
    unsigned destination = 10 * 2048 + 400 * 2;
    const int source_offsets[] = {4096, -2, 5001};
    for (unsigned test = 0; test < sizeof source_offsets / sizeof *source_offsets; ++test) {
        for (unsigned i = 0; i < 0x100000; ++i) MEM8(atlas + i) = (i * 73 + i / 7) & 255;
        memcpy(expected, hle_ptr(atlas), 0x100000);
        unsigned source = destination + source_offsets[test];
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned x = 0; x < 9; ++x) {
                unsigned at = source + (row * 9 + x) * 2;
                unsigned value = expected[at] | (unsigned)expected[at + 1] << 8;
                at = destination + row * 2048 + x * 2;
                expected[at] = value; expected[at + 1] = value >> 8;
            }
        MEM16(rectangle) = 400; MEM16(rectangle + 2) = 10;
        MEM16(rectangle + 4) = 9; MEM16(rectangle + 6) = 3;
        uint32_t args[] = {rectangle, atlas + source};
        uint32_t stack = g_esp;
        g_ebx = 0x12345678; g_ebp = 0x23456789;
        g_esi = 0x3456789A; g_edi = 0x456789AB;
        CHECK(hle_call_guest(0x4451E0, 2, args) == 1);
        CHECK(!memcmp(expected, hle_ptr(atlas), 0x100000));
        CHECK(g_esp == stack);
        CHECK(g_ebx == 0x12345678 && g_ebp == 0x23456789);
        CHECK(g_esi == 0x3456789A && g_edi == 0x456789AB);
    }
    free(expected);
}

static void gpu_page(uint32_t packet, unsigned page) {
    memset(hle_ptr(packet), 0, 40);
    MEM32(packet + 4) = 0xE1000000 | page;
    CHECK(gex_graphics_draw_packet(packet) == 1);
    CHECK(MEM32(0x4A2F78) == MEM32(0x4A33AC) + (page & 15) * 128 +
                             ((page & 16) ? 0x80000 : 0));
    CHECK(MEM8(0x4A2F90) == (page >> 7 & 3));
    CHECK(MEM8(0x4A33A0) == ((page & 0x30) ? 0x30 : 0));
}

static void gpu_sprite(uint32_t packet, unsigned clut, int u, int v, int x, int y) {
    memset(hle_ptr(packet), 0, 40);
    MEM32(packet + 4) = 0x64808080;
    MEM16(packet + 8) = x; MEM16(packet + 10) = y + 8;
    MEM8(packet + 12) = u; MEM8(packet + 13) = v;
    MEM16(packet + 14) = clut;
    MEM16(packet + 16) = MEM16(packet + 18) = 1;
    CHECK(gex_graphics_draw_packet(packet) == 1);
    gex_graphics_cpu_access();
}

static void hardware_coherence(uint32_t atlas, uint32_t packet) {
    CHECK(!gex_renderer_init(gex_renderer_backend_get(), 1));
    CHECK(!gex_graphics_set_render_size(320, 224));
    if (gex_graphics_enable_gpu()) { ++failures; return; }
    memset(hle_ptr(atlas), 0, 0x100000);
    for (int factor = 0; factor < 32; ++factor)
        for (int value = 0; value < 32; ++value) {
            unsigned color = factor * value / 16;
            MEM8(0x4A2B50 + factor * 32 + value) = color > 31 ? 31 : color;
        }
    MEM32(0x4A2964) = 3;
    MEM32(0x450010) = 8; MEM32(0x450014) = 232;
    gex_graphics_begin_frame();
    gpu_solid(packet, 0, 0, 320, 224, 248);
    CHECK(MEM16(atlas + 0x4000) == 0);
    /* Asset uploads must not export unrelated pending framebuffer draws. */
    MEM16(packet) = 400; MEM16(packet + 2) = 8;
    MEM16(packet + 4) = MEM16(packet + 6) = 1;
    gex_graphics_atlas_access(packet);
    CHECK(MEM16(atlas + 0x4000) == 0);
    MEM16(packet) = 0;
    gex_graphics_atlas_access(packet);
    CHECK(MEM16(atlas + 0x4000) == 31);
    gpu_solid(packet, 0, 0, 1, 1, 248u << 16);
    MEM16(packet) = 1023; MEM16(packet + 2) = 7;
    MEM16(packet + 4) = 2; MEM16(packet + 6) = 1;
    gex_graphics_atlas_access(packet);
    CHECK(MEM16(atlas + 0x4000) == 0x7C00);
    gpu_solid(packet, 0, 0, 1, 1, 248);
    gex_graphics_cpu_access();
    CHECK(MEM16(atlas + 0x4000) == 31);
    CHECK(MEM16(atlas + 0x4000 + 640) == 0);
    gpu_solid(packet, 0, 0, 8, 8, 248u << 16);
    MEM16(atlas + 0x4000 + 20 * 2048 + 20 * 2) = 0x3E0;
    gex_graphics_cpu_access();
    CHECK(MEM16(atlas + 0x4000) == 0x7C00);
    CHECK(MEM16(atlas + 0x4000 + 20 * 2048 + 20 * 2) == 0x3E0);
    CHECK(MEM16(atlas + 0x4000 + 21 * 2048 + 21 * 2) == 31);

    /* This CLUT begins outside the picture and crosses into its first row. */
    gpu_page(packet, 0x90);
    MEM8(atlas + 0x80000) = 16;
    gpu_solid(packet, 100, 100, 1, 1, 248u << 16);
    gpu_solid(packet, 0, 0, 1, 1, 248);
    gpu_sprite(packet, (7 * 2048 + 2016) / 32, 0, 0, 100, 100);
    CHECK(MEM16(atlas + 0x4000 + 100 * 2048 + 200) == 31);

    /* An 8-bit page at x=960 can spill into a picture on the next row. */
    gpu_page(packet, 0x8F);
    MEM16(atlas + 0xF0000 + 2) = 0x801F;
    MEM16(atlas + 0xF0000 + 62) = 0x83E0;
    gpu_solid(packet, 0, 0, 1, 1, 8);
    gpu_sprite(packet, 0xF0000 / 32, 128, 7, 101, 100);
    CHECK(MEM16(atlas + 0x4000 + 100 * 2048 + 202) == 0x801F);

    gpu_page(packet, 0x100);
    gpu_solid(packet, 0, 0, 1, 1, 248u << 16);
    gpu_sprite(packet, 0, 0, 8, 120, 120);
    CHECK(MEM16(atlas + 0x4000 + 120 * 2048 + 240) == 0x7C00);
    gex_graphics_pause_begin();
    for (int y = 0; y < 224; ++y)
        for (int x = 0; x < 320; ++x)
            MEM16(atlas + 0x7C000 + y * 2048 + x * 2) =
                (MEM16(atlas + 0x4000 + y * 2048 + x * 2) & 0x7BDE) >> 1;
    gex_graphics_pause_end();
    gex_graphics_cpu_access();
    CHECK(MEM16(atlas + 0x7C000) == 0x3C00);
    CHECK(MEM16(atlas + 0x4000) == 0x7C00);

    gpu_solid(packet, 4, 4, 1, 1, 248);
    uint32_t stack = g_esp;
    g_ebx = 0x12345678; g_ebp = 0x23456789;
    g_esi = 0x3456789A; g_edi = 0x456789AB;
    hle_call_guest(0x444DC0, 1, &packet);
    CHECK(g_esp == stack);
    CHECK(g_ebx == 0x12345678 && g_ebp == 0x23456789);
    CHECK(g_esi == 0x3456789A && g_edi == 0x456789AB);
    gex_graphics_end_frame();

    /* A level-name clear must retire exported picture history before new
     * masked tiles or framebuffer-backed textures trigger synchronization. */
    gpu_solid(packet, 0, 0, 320, 224, 248);
    gex_graphics_cpu_access();
    MEM32(0x4A2958) = 1;
    MEM32(0x4A2990) = MEM32(0x455B84) = 0x4A2A50;
    for (int pass = 0; pass < 2; ++pass) {
        gex_graphics_begin_frame();
        gpu_solid(packet, 40 + pass * 8, 40, 8, 8, 248u << 16);
        gex_graphics_cpu_access();
        CHECK(MEM16(atlas + 0x4000 + 40 * 2048 + (40 + pass * 8) * 2) == 0x7C00);
        CHECK(MEM16(atlas + 0x4000) == 0);
        if (pass) CHECK(MEM16(atlas + 0x4000 + 40 * 2048 + 40 * 2) == 0);
    }
    MEM32(0x4A2958) = 0;
    MEM32(0x4A2990) = MEM32(0x455B84) = 0;
    CHECK(!gex_graphics_set_render_size(427, 240));
    gpu_page(packet, 0x90);
    gex_graphics_shutdown();
    gex_renderer_shutdown();
}

static void screen_fade(void) {
    const int sizes[][2] = {{3, 2}, {320, 224}, {854, 480}};
    const unsigned shades[] = {255, 192, 128, 64, 0, 255};
    for (unsigned size = 0; size < sizeof sizes / sizeof *sizes; ++size) {
        CHECK(!gex_renderer_resize(sizes[size][0], sizes[size][1]));
        gex_render_frame frame;
        CHECK(!gex_renderer_frame(&frame));
        for (int y = 0; y < frame.height; ++y)
            for (int x = 0; x < frame.width; ++x) {
                unsigned value = (x * 199 + y * 1237) & 65535;
                uint8_t *p = frame.pixels + y * frame.pitch + x * 2;
                p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
            }
        gex_graphics_fade_capture();
        unsigned sequence = frame.sequence;
        for (unsigned i = 0; i < sizeof shades / sizeof *shades; ++i) {
            gex_graphics_fade_present(shades[i]);
            CHECK(!gex_renderer_frame(&frame));
            CHECK(frame.sequence == ++sequence);
            for (int y = 0; y < frame.height; ++y)
                for (int x = 0; x < frame.width; ++x) {
                    unsigned value = (x * 199 + y * 1237) & 65535;
                    unsigned expected = ((value >> 11) * shades[i] / 255) << 11 |
                        (((value >> 5 & 63) * shades[i] / 255) << 5) |
                        ((value & 31) * shades[i] / 255);
                    CHECK(frame_pixel(&frame, x, y) == expected);
                }
        }
        gex_graphics_fade_release();
    }
}

static void guest_frame_shade(uint32_t atlas) {
    gex_graphics_shutdown();
    CHECK(!gex_renderer_resize(427, 240));
    MEM32(0x451040) = MEM32(0x4A294C) = 0;
    for (unsigned y = 8; y < 232; ++y)
        for (unsigned x = 0; x < 320; ++x)
            MEM16(atlas + y * 2048 + x * 2) = (x * 199 + y * 1237) & 32767;
    const unsigned shades[] = {255, 128, 0, 64, 255};
    gex_render_frame frame;
    CHECK(!gex_renderer_frame(&frame));
    uint32_t sequence = frame.sequence, stack = g_esp;
    for (unsigned i = 0; i < sizeof shades / sizeof *shades; ++i) {
        gex_graphics_frame_shade(shades[i]);
        gex_graphics_flush();
        g_esp = stack;
        CHECK(!gex_renderer_frame(&frame));
        CHECK(frame.sequence == ++sequence);
        for (int y = 0; y < frame.height; ++y)
            for (int x = 0; x < frame.width; ++x) {
                unsigned sx = x * 320 / frame.width, sy = 8 + y * 224 / frame.height;
                unsigned value = (sx * 199 + sy * 1237) & 32767;
                CHECK(MEM16(atlas + sy * 2048 + sx * 2) == value);
                unsigned expected = rgb565(value);
                expected = ((expected >> 11) * shades[i] / 255) << 11 |
                    (((expected >> 5 & 63) * shades[i] / 255) << 5) |
                    ((expected & 31) * shades[i] / 255);
                CHECK(frame_pixel(&frame, x, y) == expected);
            }
    }
    gex_graphics_frame_shade(255);
}

int main(int argc, char **argv) {
    int hardware = argc == 3 && !strcmp(argv[2], "--packet-gpu");
#if GEX_ENABLE_BGFX_RENDERER
    int bgfx = argc == 3 && !strcmp(argv[2], "--bgfx");
    if (bgfx) {
        if (gex_packet_gpu_select_bgfx()) return 1;
        hardware = 1;
    }
#endif
    if (argc != 2 && !hardware) return 1;
    recomp_host_init();
    if (recomp_host_boot_permissive(argv[1])) return 1;
    mach_enter();
    uint32_t atlas = hle_va(hle_alloc(0x100000));
    uint32_t quad = hle_va(hle_alloc(40));
    MEM32(0x4A33AC) = MEM32(0x487F70) = atlas;
    MEM32(0x450008) = MEM32(0x450010) = 0;
    MEM32(0x45000C) = MEM32(0x450018) = 320;
    MEM32(0x450014) = MEM32(0x45001C) = 240;
    atlas_upload(atlas, quad);
    if (hardware) {
        hardware_coherence(atlas, quad);
        CHECK(!gex_renderer_init(gex_renderer_backend_get(), 1));
        hud_packets(atlas, 1, 1);
        menu_clipping(atlas, 1);
        scene_blackout(atlas, 1);
        gex_renderer_shutdown();
        mach_leave();
#if GEX_ENABLE_BGFX_RENDERER
        if (bgfx) gex_packet_gpu_shutdown_bgfx();
#endif
        return failures ? 1 : 0;
    }
    scaled_sprite(atlas, quad, 24, 24);
    scaled_sprite(atlas, quad, 23, 21);
    scaled_sprite(atlas, quad, 8, 8);
    sprite_sampling(atlas, quad);
    loading_trails(atlas, quad);
    CHECK(!gex_renderer_init(gex_renderer_backend_get(), 1));
    viewport(atlas, 320, 240);
    viewport(atlas, 640, 480);
    hud_layout();
    MEM32(0x4A2964) = 68;
    hud_layout();
    MEM32(0x4A2964) = 3;
    hud_packets(atlas, 0, 0);
    hud_packets(atlas, 1, 0);
    expanded_viewport(atlas, quad);
    screen_layouts(atlas, quad);
    menu_clipping(atlas, 0);
    scene_blackout(atlas, 0);
    screen_transitions(atlas);
    transition_layout(atlas);
    recycled_backdrops(atlas);
    world_submission();
    camera_boundaries();
    authored_camera_boundaries();
    camera_motion();
    portal_player_draws();
    screen_fade();
    guest_frame_shade(atlas);
    gex_renderer_shutdown();
    mach_leave();
    return failures ? 1 : 0;
}
