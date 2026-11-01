#include <stdio.h>
#include <string.h>
#include "renderer.h"
#include "raster.h"

static int failures;
#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

static uint8_t atlas[0x100000], table[1024], packet[40], picture[320 * 224 * 2];
static gex_raster_state state = {atlas, table, 0x90, 0, 0, 320, 240, 0};

static void set_word(uint8_t *p, int value) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }
static unsigned pixel(gex_raster *r, int width, int x, int y) {
    const uint8_t *p = gex_raster_pixels(r) + (y * width + x) * 2;
    return p[0] | (unsigned)p[1] << 8;
}

static void quad(int x, int y) {
    memset(packet, 0, sizeof packet);
    packet[4] = packet[5] = packet[6] = 128; packet[7] = 0x2C;
    for (int i = 0; i < 4; ++i) {
        set_word(packet + 8 + i * 8, x + ((i & 1) ? 8 : 0));
        set_word(packet + 10 + i * 8, y + ((i & 2) ? 8 : 0));
        packet[12 + i * 8] = (i & 1) ? 16 : 0;
        packet[13 + i * 8] = (i & 2) ? 16 : 0;
    }
    set_word(packet + 14, 0xF0000 / 32); set_word(packet + 22, 0x90);
}

static void fixed_density(void) {
    state.top = 8; state.bottom = 232;
    const int sizes[][2] = {{160, 112}, {320, 224}, {640, 448}, {853, 480},
        {1237, 701}, {1920, 1080}, {3840, 2160}};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        int w = sizes[i][0], h = sizes[i][1];
        gex_raster *r = gex_raster_create(w, h);
        CHECK(r != NULL);
        if (!r) continue;
        quad(10, 18);
        gex_raster_packet(r, packet, &state);
        /* Doubling the viewport must not double the sprite or recover extra
         * texels: the eight-pixel quad remains exactly eight pixels wide. */
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < w; ++x) {
                unsigned expected = x >= 10 && x < 18 && y >= 10 && y < 18
                    ? 0x8001u + (x - 10) * 2 : 0;
                CHECK(pixel(r, w, x, y) == expected);
            }
        quad(400, 308);
        gex_raster_packet(r, packet, &state);
        if (w >= 408 && h >= 308)
            CHECK(pixel(r, w, 400, 300) == 0x8001);
        gex_raster_remember(r, picture, 640);
        set_word(picture + (10 * 320 + 10) * 2, 0x1234);
        gex_raster_sync(r, picture, 640);
        CHECK(pixel(r, w, 10, 10) == 0x1234);
        CHECK(pixel(r, w, 11, 10) == 0x8003);
        gex_raster_export(r, picture, 640);
        CHECK((picture[(10 * 320 + 11) * 2] |
               (unsigned)picture[(10 * 320 + 11) * 2 + 1] << 8) == 0x8003);
        set_word(picture, 0x1357);
        gex_raster_export(r, picture, 640);
        CHECK(picture[0] == 0x57 && picture[1] == 0x13);
        gex_raster_sync(r, picture, 640);
        gex_raster_export(r, picture, 640);
        CHECK(pixel(r, w, 0, 0) == 0x1357);
        if (w >= 408 && h >= 308)
            CHECK(pixel(r, w, 400, 300) == 0x8001);
        memset(picture, 0, sizeof picture);
        gex_raster_destroy(r);
    }
    state.top = 0; state.bottom = 240;
}

static void clipping_and_formats(void) {
    gex_raster *r = gex_raster_create(640, 448);
    CHECK(r != NULL);
    if (!r) return;
    quad(10, 18);
    state.left = 12; state.top = 20; state.right = 16; state.bottom = 24;
    gex_raster_packet(r, packet, &state);
    CHECK(pixel(r, 640, 11, 12) == 0 && pixel(r, 640, 12, 11) == 0);
    CHECK(pixel(r, 640, 12, 12) == 0x8005 && pixel(r, 640, 15, 15) == 0x800B);
    CHECK(pixel(r, 640, 16, 12) == 0 && pixel(r, 640, 12, 16) == 0);
    state.left = state.top = 0; state.right = 320; state.bottom = 240;
    quad(400, 308); set_word(packet + 22, 0x10);
    memset(atlas + 0x80000, 0x21, 16 * 2048);
    gex_raster_packet(r, packet, &state);
    CHECK(pixel(r, 640, 400, 300) == 0x8001);
    memset(atlas + 0x80000, 0, 16 * 2048);
    gex_raster_packet(r, packet, &state);
    CHECK(pixel(r, 640, 400, 300) == 0x8001);
    packet[7] |= 2; memset(atlas + 0x80000, 0x21, 16 * 2048);
    gex_raster_packet(r, packet, &state);
    CHECK(pixel(r, 640, 400, 300) == 0);
    gex_raster_clear(r); set_word(packet + 22, 0x110);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) set_word(atlas + 0x80000 + y * 2048 + x * 2, 0x7C00);
    gex_raster_packet(r, packet, &state);
    CHECK(pixel(r, 640, 400, 300) == 0x7C00);
    memset(packet, 0, sizeof packet);
    packet[4] = packet[5] = packet[6] = 128; packet[7] = 0x64;
    set_word(packet + 8, 500); set_word(packet + 10, 348);
    set_word(packet + 16, 8); set_word(packet + 18, 8);
    state.page = 0x110;
    gex_raster_packet(r, packet, &state);
    CHECK(pixel(r, 640, 500, 340) == 0x7C00 && pixel(r, 640, 507, 347) == 0x7C00);
    CHECK(pixel(r, 640, 508, 340) == 0);
    gex_raster_destroy(r);
}

static void transformed_quads(void) {
    gex_raster *r = gex_raster_create(640, 448);
    CHECK(r != NULL);
    if (!r) return;
    quad(400, 308);
    packet[12] = packet[28] = 15; packet[20] = packet[36] = 0;
    /* Avoid the far-edge zero marker while reversing the texture coordinates. */
    packet[20] = packet[36] = 1;
    gex_raster_packet(r, packet, &state);
    CHECK(pixel(r, 640, 400, 300) == 0x8010);
    CHECK(pixel(r, 640, 407, 300) == 0x8003);
    gex_raster_clear(r);
    quad(0, 0);
    const int xy[][2] = {{500, 308}, {500, 316}, {492, 308}, {492, 316}};
    for (int i = 0; i < 4; ++i) {
        set_word(packet + 8 + i * 8, xy[i][0]);
        set_word(packet + 10 + i * 8, xy[i][1]);
    }
    gex_raster_packet(r, packet, &state);
    CHECK(pixel(r, 640, 492, 300) == 0x8002);
    CHECK(pixel(r, 640, 499, 307) == 0x8010);
    CHECK(pixel(r, 640, 491, 300) == 0 && pixel(r, 640, 500, 300) == 0);
    gex_raster_destroy(r);
}

static void hud_pixel_grid(void) {
    const int sizes[][3] = {{427, 240, 1}, {854, 480, 2}, {1920, 1080, 4},
                            {2560, 1080, 4}, {3440, 1440, 6}};
    state = (gex_raster_state){atlas, table, 0x90, 0, 8, 320, 232, 0};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            atlas[0x80000 + y * 2048 + x] = (uint8_t)((x + y) % 4 ? x + 1 : 0);
    for (unsigned size = 0; size < sizeof sizes / sizeof *sizes; ++size)
        for (int variant = 0; variant < 4; ++variant) {
            int w = sizes[size][0], h = sizes[size][1], scale = sizes[size][2];
            gex_raster *original = gex_raster_create(320, 224);
            gex_raster *expanded = gex_raster_create(w, h);
            CHECK(original && expanded);
            if (!original || !expanded) {
                gex_raster_destroy(original); gex_raster_destroy(expanded);
                continue;
            }
            quad(144, 28);
            /* The guest's scaled HUD glyphs have inclusive UV endpoints. */
            for (int i = 0; i < 4; ++i) {
                set_word(packet + 8 + i * 8, 144 + ((i & 1) ? 16 : 0));
                set_word(packet + 10 + i * 8, 28 + ((i & 2) ? 11 : 0));
                packet[12 + i * 8] = (i & 1) ? 15 : 0;
                packet[13 + i * 8] = (i & 2) ? 10 : 0;
            }
            if (variant == 1) {
                set_word(packet + 8, 160); set_word(packet + 24, 160);
                set_word(packet + 16, 144); set_word(packet + 32, 144);
            } else if (variant == 2) {
                set_word(packet + 8, 146); set_word(packet + 32, 158);
                packet[7] |= 2;
            } else if (variant == 3) {
                memset(packet, 0, sizeof packet);
                packet[4] = packet[5] = packet[6] = 128; packet[7] = 0x64;
                set_word(packet + 8, 144); set_word(packet + 10, 28);
                set_word(packet + 14, 0xF0000 / 32);
                set_word(packet + 16, 16); set_word(packet + 18, 11);
            }
            state.pixel_scale = state.offset_x = 0;
            gex_raster_packet(original, packet, &state);
            state.pixel_scale = scale;
            state.offset_x = (w - 320 * scale) / 2;
            gex_raster_packet(expanded, packet, &state);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    int sx = (x - state.offset_x) / scale, sy = y / scale;
                    unsigned expected = x >= state.offset_x && sx < 320 && sy < 224
                        ? pixel(original, 320, sx, sy) : 0;
                    CHECK(pixel(expanded, w, x, y) == expected);
                }
            gex_raster_destroy(original); gex_raster_destroy(expanded);
        }
    state.pixel_scale = state.offset_x = 0;
}

static void tiled_backdrop(void) {
    const int sizes[][2] = {{427, 240}, {1920, 1080}, {2560, 1080},
                            {3440, 1440}, {240, 427}, {1, 1}};
    state.layout = 2; state.top = 8; state.bottom = 232;
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        int w = sizes[i][0], h = sizes[i][1];
        gex_raster *r = gex_raster_create(w, h);
        CHECK(r != NULL);
        if (!r) continue;
        memset(packet, 0, sizeof packet);
        packet[4] = packet[5] = packet[6] = 128; packet[7] = 0x64;
        set_word(packet + 10, 8);
        set_word(packet + 16, 320); set_word(packet + 18, 224);
        state.page = 0x110;
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x)
                set_word(atlas + 0x80000 + y * 2048 + x * 2,
                         1 + x % 31 + (y % 31) * 32);
        gex_raster_packet(r, packet, &state);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                CHECK(pixel(r, w, x, y) ==
                      (unsigned)(1 + (x % 320 % 256) % 31 + (y % 224 % 31) * 32));
        /* A full-screen fade must run once, not once per repeated tile. */
        memset(packet, 0, sizeof packet); packet[7] = 0x62;
        set_word(packet + 10, 8);
        set_word(packet + 12, 320); set_word(packet + 14, 224);
        gex_raster_packet(r, packet, &state);
        CHECK(pixel(r, w, w - 1, h - 1) ==
              ((unsigned)(1 + ((w - 1) % 320 % 256) % 31 +
                          ((h - 1) % 224 % 31) * 32) >> 1 & 0x3DEF));
        gex_raster_destroy(r);
    }
    state.layout = 0;
}

static void full_picture_effects(void) {
    const int sizes[][2] = {{1, 1}, {160, 112}, {1237, 701}, {3840, 2160}};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        int w = sizes[i][0], h = sizes[i][1];
        gex_raster *r = gex_raster_create(w, h);
        CHECK(r != NULL);
        if (!r) continue;
        state.top = 8; state.bottom = 232;
        memset(packet, 0, sizeof packet); packet[7] = 0x60; packet[4] = 248;
        set_word(packet + 12, 320); set_word(packet + 14, 240);
        gex_raster_packet(r, packet, &state);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) CHECK(pixel(r, w, x, y) == 31);
        packet[4] = 0; packet[5] = 248;
        set_word(packet + 10, 8); set_word(packet + 14, 224);
        gex_raster_packet(r, packet, &state);
        CHECK(pixel(r, w, w - 1, h - 1) == 0x3E0);
        gex_raster_sync(r, picture, 640);
        gex_raster_destroy(r);
    }
    CHECK(!gex_raster_create(0, 240) && !gex_raster_create(8193, 240));
    state.top = 0; state.bottom = 240;
}

static void fitted_pixel_grid(void) {
    const int sizes[][2] = {{427, 240}, {854, 480}, {160, 112}, {320, 224}};
    const int pieces[][4] = {{223, 137, 8, 27}, {231, 137, 8, 27},
                            {239, 137, 16, 27}, {212, 90, 64, 64}};
    gex_raster_state native = {atlas, table, 0x110, 0, 8, 320, 232, 0};
    gex_raster_state fitted = native;
    fitted.layout = 1;
    memset(atlas, 0, sizeof atlas);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            if ((x + y) % 7)
                set_word(atlas + 0x80000 + y * 2048 + x * 2,
                         0x8000 | ((x + 1) & 31) | ((y & 31) << 5));
    for (unsigned size = 0; size < sizeof sizes / sizeof *sizes; ++size) {
        int w = sizes[size][0], h = sizes[size][1];
        gex_raster *reference = gex_raster_create(320, 224);
        gex_raster *actual = gex_raster_create(w, h);
        CHECK(reference && actual);
        if (!reference || !actual) {
            gex_raster_destroy(reference); gex_raster_destroy(actual);
            continue;
        }
        for (unsigned i = 0; i < sizeof pieces / sizeof *pieces; ++i) {
            memset(packet, 0, sizeof packet);
            packet[4] = packet[5] = packet[6] = 128;
            packet[7] = i == 3 ? 0x64 : 0x66;
            set_word(packet + 8, pieces[i][0]); set_word(packet + 10, pieces[i][1]);
            set_word(packet + 16, pieces[i][2]); set_word(packet + 18, pieces[i][3]);
            packet[12] = i == 3 ? 0 : (uint8_t)(i * 8);
            gex_raster_packet(reference, packet, &native);
            gex_raster_packet(actual, packet, &fitted);
        }
        gex_render_rect rect = gex_renderer_fit(w, h, 320, 224);
        int mismatch = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                unsigned expected = 0;
                if (x >= rect.x && x < rect.x + rect.width &&
                    y >= rect.y && y < rect.y + rect.height) {
                    int sx = ((x - rect.x + 1) * 320 + rect.width - 1) / rect.width - 1;
                    int sy = ((y - rect.y + 1) * 224 + rect.height - 1) / rect.height - 1;
                    expected = pixel(reference, 320, sx, sy);
                }
                mismatch += pixel(actual, w, x, y) != expected;
            }
        if (mismatch)
            fprintf(stderr, "fitted CEL grid %dx%d: %d mismatched pixels\n", w, h, mismatch);
        CHECK(mismatch == 0);
        gex_raster_destroy(reference); gex_raster_destroy(actual);
    }
}

int main(void) {
    for (int factor = 0; factor < 32; ++factor)
        for (int value = 0; value < 32; ++value)
            table[factor * 32 + value] = (uint8_t)(factor * value / 16 > 31 ? 31 : factor * value / 16);
    for (int i = 1; i < 256; ++i) set_word(atlas + 0xF0000 + i * 2, 0x8000 | i);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) atlas[0x80000 + y * 2048 + x] = (uint8_t)(x + 1);
    fixed_density();
    transformed_quads();
    clipping_and_formats();
    full_picture_effects();
    hud_pixel_grid();
    tiled_backdrop();
    fitted_pixel_grid();
    return failures ? 1 : 0;
}
