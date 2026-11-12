#include "platform/dreamcast/title_cel.h"
#include "platform/dreamcast/title_layout.h"
#include "platform/dreamcast/title_plan.h"
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <string.h>

static void put16(uint8_t *p, unsigned value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    put16(p, value);
    put16(p + 2, value >> 16);
}

static int read_guest(void *context, uint32_t address, void *out, size_t size) {
    const uint8_t *memory = context;
    if (address > 1024 || size > 1024 - address) return -1;
    memcpy(out, memory + address, size);
    return 0;
}

static int match_guest(void *context, uint32_t address, const void *data, size_t size) {
    const uint8_t *memory = context;
    if (address > 1024 || size > 1024 - address) return -1;
    return memcmp(memory + address, data, size) == 0;
}

static int match_error(void *context, uint32_t address, const void *data, size_t size) {
    (void)context; (void)address; (void)data; (void)size;
    return -1;
}

static void tile(uint8_t *p, unsigned source, unsigned width, unsigned height, int x, int y) {
    put16(p, source);
    p[2] = (uint8_t)width;
    p[3] = (uint8_t)height;
    put16(p + 4, (uint16_t)x);
    put16(p + 6, (uint16_t)y);
}

static void check_rectangular_twiddle(unsigned width, unsigned height) {
    uint16_t source[256];
    uint32_t output[128];
    unsigned image_width = width - 3, image_height = height - 1;
    gex_title_cel cel = {.width = image_width, .height = image_height};
    for (unsigned i = 0; i < image_width * image_height; ++i)
        source[i] = (uint16_t)(i + 1);
    memset(output, 0xff, sizeof output);
    assert(gex_title_cel_twiddle(&cel, source, image_width * image_height,
                                 width, height, output, width * height / 2) == GEX_TITLE_CEL_OK);
    unsigned side = width < height ? width : height;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            unsigned index = (x / side + y / side) * side * side;
            for (unsigned bit = 1, out = 1; bit < side; bit <<= 1, out <<= 2) {
                if (y & bit) index += out;
                if (x & bit) index += out * 2;
            }
            unsigned actual = (output[index / 2] >> ((index & 1) * 16)) & 0xffff;
            unsigned expected = x < image_width && y < image_height ?
                                source[y * image_width + x] : 0;
            assert(actual == expected);
        }
}

int main(void) {
    uint8_t image[128] = {0};
    uint32_t pixels[256];
    gex_title_cel cel;

    uint8_t compressed[] = {7, 0, 0, 0, 0x82, 2, 0,
                            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    uint8_t uncompressed[16];
    const uint8_t expected[] = {1, 2, 3, 4, 1, 2, 3, 4,
                                5, 6, 7, 8, 9, 10, 11, 12};
    assert(gex_title_cel_decompress(compressed, sizeof compressed, uncompressed,
                                    sizeof uncompressed) == GEX_TITLE_CEL_OK);
    assert(!memcmp(uncompressed, expected, sizeof expected));
    assert(gex_title_cel_decompress(compressed, sizeof compressed - 1, uncompressed,
                                    sizeof uncompressed) == GEX_TITLE_CEL_INVALID);
    assert(gex_title_cel_decompress(compressed, sizeof compressed, uncompressed,
                                    sizeof uncompressed - 4) == GEX_TITLE_CEL_INVALID);
    compressed[0] = 5;
    assert(gex_title_cel_decompress(compressed, sizeof compressed, uncompressed,
                                    sizeof uncompressed) == GEX_TITLE_CEL_INVALID);
    uint8_t run_128[] = {5, 0, 0, 0, 0x80, 1, 2, 3, 4};
    uint8_t long_run[512];
    assert(gex_title_cel_decompress(run_128, sizeof run_128, long_run,
                                    sizeof long_run) == GEX_TITLE_CEL_OK);
    for (size_t i = 0; i < sizeof long_run; ++i)
        assert(long_run[i] == 1 + i % 4);

    tile(image + 20, 44, 3, 2, -2, 1);
    tile(image + 28, 52, 2, 1, 3, 3);
    image[44] = 0x10; image[45] = 0x02;
    image[48] = 0x43; image[49] = 0x05;
    image[52] = 0x76;
    assert(gex_title_cel_bounds(image, sizeof image, &cel) == GEX_TITLE_CEL_OK);
    assert(cel.left == -2 && cel.top == 1 && cel.width == 7 && cel.height == 3);
    assert(cel.tiles == 2 && cel.format == 0);
    assert(gex_title_cel_decode(image, sizeof image, &cel, pixels, 21) == GEX_TITLE_CEL_OK);
    assert(pixels[0] == 0x10000 && pixels[1] == 0x10001 && pixels[2] == 0x10002);
    assert(pixels[7] == 0x10003 && pixels[8] == 0x10004 && pixels[9] == 0x10005);
    assert(pixels[19] == 0x10006 && pixels[20] == 0x10007);
    assert(pixels[3] == 0 && pixels[14] == 0);
    assert(gex_title_cel_decode(image, sizeof image, &cel, pixels, 20) == GEX_TITLE_CEL_INVALID);
    assert(gex_title_cel_bounds(image, 55, &cel) == GEX_TITLE_CEL_INVALID);

    put16(image + 32, (uint16_t)-1);
    put16(image + 34, 1);
    assert(gex_title_cel_bounds(image, sizeof image, &cel) == GEX_TITLE_CEL_OK);
    assert(gex_title_cel_decode(image, sizeof image, &cel, pixels, 256) == GEX_TITLE_CEL_OVERLAP);

    memset(image, 0, sizeof image);
    tile(image + 20, 36, 3, 2, 0, 0);
    image[16] = 1;
    image[36] = 7; image[37] = 0; image[38] = 9;
    image[44] = 11; image[45] = 12; image[46] = 13;
    assert(gex_title_cel_bounds(image, sizeof image, &cel) == GEX_TITLE_CEL_OK);
    assert(gex_title_cel_decode(image, sizeof image, &cel, pixels, 256) == GEX_TITLE_CEL_OK);
    assert(pixels[0] == 0x10007 && pixels[1] == 0x10000 && pixels[2] == 0x10009);
    assert(pixels[3] == 0x1000b && pixels[4] == 0x1000c && pixels[5] == 0x1000d);

    image[16] = 2;
    put16(image + 36, 0x0000); put16(image + 38, 0x8001); put16(image + 40, 0x7fff);
    put16(image + 52, 0x1234); put16(image + 54, 0x8000); put16(image + 56, 0xffff);
    assert(gex_title_cel_bounds(image, sizeof image, &cel) == GEX_TITLE_CEL_OK);
    assert(gex_title_cel_decode(image, sizeof image, &cel, pixels, 256) == GEX_TITLE_CEL_OK);
    assert(pixels[0] == 0x10000 && pixels[1] == 0x18001 && pixels[2] == 0x17fff);
    assert(pixels[3] == 0x11234 && pixels[4] == 0x18000 && pixels[5] == 0x1ffff);

    image[16] = 3;
    assert(gex_title_cel_bounds(image, sizeof image, &cel) == GEX_TITLE_CEL_UNSUPPORTED);
    image[16] = 4;
    assert(gex_title_cel_bounds(image, sizeof image, &cel) == GEX_TITLE_CEL_UNSUPPORTED);
    image[16] = 0x40;
    assert(gex_title_cel_bounds(image, sizeof image, &cel) == GEX_TITLE_CEL_UNSUPPORTED);
    image[16] = 0;
    put16(image + 20, 0xffff);
    assert(gex_title_cel_bounds(image, sizeof image, &cel) == GEX_TITLE_CEL_INVALID);

    uint8_t palette[32] = {0}, modulation[1024];
    uint32_t samples[] = {0, 0x10000, 0x10001, 0x10002, 0x10003};
    uint16_t opaque[5], blended[5];
    for (unsigned tint = 0; tint < 32; ++tint)
        for (unsigned channel = 0; channel < 32; ++channel)
            modulation[tint * 32 + channel] = (uint8_t)(tint * channel / 16 > 31 ?
                                                         31 : tint * channel / 16);
    put16(palette, 0x001f);
    put16(palette + 2, 0x8000);
    put16(palette + 4, 0x0000);
    put16(palette + 6, 0xfc00);
    cel = (gex_title_cel){0, 0, 5, 1, 0, 1};
    assert(gex_title_cel_colors(&cel, samples, 5, palette, sizeof palette, modulation,
                                0x808080, 1, opaque, blended) == GEX_TITLE_CEL_OK);
    assert(opaque[0] == 0 && opaque[1] == 0xfc00 && opaque[2] == 0 &&
           opaque[3] == 0 && opaque[4] == 0);
    assert(blended[0] == 0 && blended[1] == 0 && blended[2] == 0x8000 &&
           blended[3] == 0 && blended[4] == 0x801f);
    modulation[16 * 32] = 1;
    assert(gex_title_cel_colors(&cel, samples, 5, palette, sizeof palette, modulation,
                                0x808080, 1, opaque, blended) == GEX_TITLE_CEL_OK);
    assert(opaque[3] == 0 && blended[3] == 0);
    modulation[16 * 32] = 0;
    assert(gex_title_cel_colors(&cel, samples, 5, palette, 31, modulation,
                                0x808080, 1, opaque, blended) == GEX_TITLE_CEL_INVALID);
    assert(gex_title_cel_colors(&cel, samples, 5, palette, sizeof palette, modulation,
                                0x4d4d4d, 1, opaque, blended) == GEX_TITLE_CEL_OK);
    assert(opaque[1] == 0xfc00 && blended[4] == 0x801f);
    assert(gex_title_cel_colors(&cel, samples, 5, palette, sizeof palette, modulation,
                                0, 0, opaque, blended) == GEX_TITLE_CEL_OK);
    assert(opaque[1] == 0 && opaque[2] == 0x8000);
    cel.format = 2;
    samples[1] = 0x1801f;
    assert(gex_title_cel_colors(&cel, samples, 5, NULL, 0, NULL,
                                0x808080, 0, opaque, blended) == GEX_TITLE_CEL_OK);
    assert(opaque[1] == 0xfc00 && blended[1] == 0);
    samples[1] = 0x10001;
    assert(gex_title_cel_colors(&cel, samples, 5, NULL, 0, NULL,
                                0x000000, 0, opaque, blended) == GEX_TITLE_CEL_OK);
    assert(opaque[1] == 0x8400);

    uint16_t grid[] = {1, 2, 3, 4, 5, 6};
    uint32_t words[32];
    cel.width = 3; cel.height = 2;
    assert(gex_title_cel_twiddle(&cel, grid, 6, 8, 8, words, 32) == GEX_TITLE_CEL_OK);
    assert(words[0] == 0x00040001 && words[1] == 0x00050002);
    assert(words[4] == 0x00060003 && words[5] == 0);
    for (unsigned i = 0; i < 32; ++i)
        if (i != 0 && i != 1 && i != 4) assert(words[i] == 0);
    assert(gex_title_cel_twiddle(&cel, grid, 5, 8, 8, words, 32) == GEX_TITLE_CEL_INVALID);
    assert(gex_title_cel_twiddle(&cel, grid, 6, 7, 8, words, 32) == GEX_TITLE_CEL_INVALID);
    assert(gex_title_cel_twiddle(&cel, grid, 6, 8, 8, words, 31) == GEX_TITLE_CEL_INVALID);
    check_rectangular_twiddle(16, 8);
    check_rectangular_twiddle(8, 16);

    uint32_t colors[32];
    for (unsigned i = 0; i < 32; ++i) colors[i] = 0x12000000u | i * 0x010101u;
    uint8_t fade_rgb[3] = {128, 64, 32};
    assert(gex_title_packet_rgb(0x8000, 0x12345678, fade_rgb, colors) == 0x001a2b3c);
    assert(gex_title_packet_rgb(0x80c0, 0x12345678, fade_rgb, colors) == 0x12345678);
    assert(gex_title_packet_rgb(0x1f00, 0x00808080, fade_rgb, colors) == colors[31]);
    for (unsigned intensity = 0; intensity < 256; ++intensity) {
        colors[3] = 0xaaff0000u | intensity;
        uint32_t expected = intensity |
            (uint32_t)(intensity / 2) << 8 |
            (uint32_t)(intensity / 4) << 16;
        assert(gex_title_packet_rgb(0x0300, 0, fade_rgb, colors) == expected);
    }

    cel.width = 1;
    cel.height = 1;
    for (unsigned color = 0; color <= 0xffff; ++color) {
        samples[0] = 0x10000 | color;
        uint16_t expected = color ? (uint16_t)(0x8000 | (color & 31) << 10 |
                                               (color & 0x3e0) | (color >> 10 & 31)) : 0;
        assert(gex_title_cel_colors(&cel, samples, 1, NULL, 0, NULL, 0, 1,
                                    opaque, blended) == GEX_TITLE_CEL_OK);
        assert(opaque[0] == ((color & 0x8000) ? 0 : expected));
        assert(blended[0] == ((color & 0x8000) ? expected : 0));
    }

    uint32_t random = 0x12345678;
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        for (size_t i = 0; i < sizeof image; ++i) {
            random = random * 1664525u + 1013904223u;
            image[i] = (uint8_t)(random >> 24);
        }
        size_t size = random % (sizeof image + 1);
        if (gex_title_cel_bounds(image, size, &cel) == GEX_TITLE_CEL_OK) {
            size_t n = (size_t)cel.width * cel.height;
            if (n <= sizeof pixels / sizeof *pixels) {
                int result = gex_title_cel_decode(image, size, &cel, pixels, n);
                assert(result == GEX_TITLE_CEL_OK || result == GEX_TITLE_CEL_OVERLAP);
            }
        }
    }

    gex_title_layout_input in = {
        .object_x = 100u << 16, .object_y = 50u << 16,
        .camera_x = 10u << 16, .camera_y = 20u << 16,
        .part_position = (2u << 16) | 3u,
        .image_width = 16u << 16, .image_height = 8u << 16,
        .image_origin_x = 4u << 16, .image_origin_y = 1u << 16,
        .view_width = 320, .view_height = 240
    };
    gex_title_layout_output out;
    assert(gex_title_layout_part(&in, &out));
    assert(out.x == 96 && out.y == 34 && out.flags == 0);
    in.object_flags = 0x80000000u;
    assert(gex_title_layout_part(&in, &out));
    assert(out.x == 84 && out.y == 34 && out.flags == 0x80000000u);
    in.part_flags = 0xc0000000u;
    assert(gex_title_layout_part(&in, &out));
    assert(out.x == 76 && out.y == 40 && out.flags == 0x40000000u);
    in.object_x = (uint32_t)-65537;
    in.camera_x = in.camera_y = in.object_y = in.part_position = 0;
    in.object_flags = in.part_flags = 0;
    in.image_origin_x = in.image_origin_y = 0;
    assert(gex_title_layout_part(&in, &out) && out.x == -2);
    in.object_x = (uint32_t)-(17 * 65536);
    assert(!gex_title_layout_part(&in, &out));

    gex_title_wrap_input wrap = {
        .x = 20u << 16, .y = 30u << 16,
        .previous_x = 10u << 16, .previous_y = 32u << 16,
        .object_frame = 10, .current_frame = 11,
        .screen_x = 1, .screen_y = 2
    };
    int wx, wy;
    gex_title_layout_wrap(&wrap, &wx, &wy);
    assert(wx == 4 && wy == -3);
    wrap.screen_x = 100;
    gex_title_layout_wrap(&wrap, &wx, &wy);
    assert(wx == -95 && wy == -3);
    wrap.screen_x = 103;
    gex_title_layout_wrap(&wrap, &wx, &wy);
    assert(wx == 0 && wy == 0);
    wrap.object_flags = 0x01000000u;
    wrap.screen_x = 0;
    gex_title_layout_wrap(&wrap, &wx, &wy);
    assert(wx == 0 && wy == 0);
    wrap.object_flags = 0;
    wrap.object_frame = 11;
    gex_title_layout_wrap(&wrap, &wx, &wy);
    assert(wx == 0 && wy == 0);

    uint8_t memory[1024] = {0};
    const unsigned object = 16, cel_address = 600, list = 640, part = 680, image_address = 720;
    put32(memory + cel_address + 0x18, list);
    put32(memory + list, part);
    put32(memory + part, 2u << 16 | 3u);
    put32(memory + part + 8, image_address);
    put32(memory + part + 12, 0x1234);
    put32(memory + part + 16, 0x5678);
    put32(memory + image_address, 16u << 16);
    put32(memory + image_address + 4, 8u << 16);
    put32(memory + image_address + 8, 4u << 16);
    put32(memory + image_address + 12, 1u << 16);
    put16(memory + image_address + 20, 36);
    put32(memory + object + 0x78, 100u << 16);
    put32(memory + object + 0x7c, 50u << 16);
    put32(memory + object + 0x1f4, 10);
    put32(memory + object + 0x1f8, 90u << 16);
    put32(memory + object + 0x1fc, 48u << 16);
    gex_title_part result[1];
    size_t count = 99;
    assert(gex_title_plan(read_guest, memory, object, cel_address, 10u << 16,
                          20u << 16, 11, 0, 0, 320, 240,
                          result, 1, &count) == GEX_TITLE_PLAN_OK);
    assert(count == 1 && result[0].x == 96 && result[0].y == 34);
    assert(result[0].image == image_address && result[0].palette == 0x1234 &&
           result[0].tint == 0x5678 && result[0].wrap_x == 5 && result[0].wrap_y == 1);
    put32(memory + object + 0xbc, 0x1111);
    put32(memory + object + 0xc0, 0x2222);
    assert(gex_title_plan(read_guest, memory, object, cel_address, 0, 0, 11,
                          0, 0, 320, 240, result, 1, &count) == GEX_TITLE_PLAN_OK);
    assert(result[0].tint == 0x1111 && result[0].palette == 0x2222);
    assert(gex_title_plan(read_guest, memory, object, cel_address, 0, 0, 11,
                          0, 0, 320, 240, NULL, 0, &count) == GEX_TITLE_PLAN_CAPACITY);
    assert(count == 0);
    put32(memory + list, 1020);
    assert(gex_title_plan(read_guest, memory, object, cel_address, 0, 0, 11,
                          0, 0, 320, 240, result, 1, &count) == GEX_TITLE_PLAN_INVALID);
    assert(count == 0);
    assert(gex_title_plan(read_guest, memory, object, 0, 0, 0, 11,
                          0, 0, 320, 240, result, 1, &count) == GEX_TITLE_PLAN_OK);
    assert(count == 0);

    memset(memory, 0, sizeof memory);
    const unsigned capture_image = 128, capture_palette = 800;
    tile(memory + capture_image + 20, 44, 3, 2, -2, 1);
    tile(memory + capture_image + 28, 52, 2, 1, 3, 3);
    memory[capture_image + 44] = 0x10;
    memory[capture_image + 48] = 0x32;
    memory[capture_image + 52] = 0x54;
    put16(memory + capture_palette, 0x001f);
    put16(memory + capture_palette + 2, 0x03e0);
    gex_title_cel_snapshot snapshot;
    assert(gex_title_cel_capture(read_guest, memory, capture_image, capture_palette,
                                 &snapshot) == GEX_TITLE_CEL_OK);
    assert(snapshot.cel.left == -2 && snapshot.cel.top == 1 &&
           snapshot.cel.width == 7 && snapshot.cel.height == 3);
    assert(snapshot.palette_size == 32 && snapshot.pixels[0] == 0x10000 &&
           snapshot.pixels[1] == 0x10001 && snapshot.pixels[19] == 0x10004);
    assert(gex_title_cel_matches(read_guest, NULL, memory, capture_image, capture_palette, &snapshot));
    assert(gex_title_cel_matches(read_guest, match_guest, memory, capture_image, capture_palette, &snapshot));
    assert(!gex_title_cel_matches(read_guest, match_error, memory, capture_image, capture_palette, &snapshot));
    memory[capture_palette] ^= 1;
    assert(!gex_title_cel_matches(read_guest, match_guest, memory, capture_image, capture_palette, &snapshot));
    assert(!gex_title_cel_matches(read_guest, NULL, memory, capture_image, capture_palette, &snapshot));
    memory[capture_palette] ^= 1;
    memory[capture_image + 44] ^= 1;
    assert(!gex_title_cel_matches(read_guest, match_guest, memory, capture_image, capture_palette, &snapshot));
    assert(!gex_title_cel_matches(read_guest, NULL, memory, capture_image, capture_palette, &snapshot));
    memory[capture_image + 44] ^= 1;
    memory[capture_image + 24] ^= 1;
    assert(!gex_title_cel_matches(read_guest, match_guest, memory, capture_image, capture_palette, &snapshot));
    memory[capture_image + 24] ^= 1;
    assert(gex_title_cel_matches(read_guest, match_guest, memory, capture_image, capture_palette, &snapshot));
    memset(memory + capture_image, 0, 80);
    memset(memory + capture_palette, 0, 32);
    assert(snapshot.pixels[1] == 0x10001 && snapshot.palette[0] == 0x1f &&
           snapshot.palette[2] == 0xe0);
    gex_title_cel_release(&snapshot);
    assert(!snapshot.pixels && !snapshot.palette_size);

    tile(memory + capture_image + 20, 44, 3, 2, 0, 0);
    tile(memory + capture_image + 28, 52, 2, 1, 1, 1);
    assert(gex_title_cel_capture(read_guest, memory, capture_image, capture_palette,
                                 &snapshot) == GEX_TITLE_CEL_OVERLAP);
    assert(!snapshot.pixels && !snapshot.palette_size);
    put16(memory + capture_image + 28, 0);
    assert(gex_title_cel_capture(read_guest, memory, capture_image, 1000,
                                 &snapshot) == GEX_TITLE_CEL_INVALID);
    assert(!snapshot.pixels);
    assert(gex_title_cel_capture(read_guest, memory, UINT32_MAX - 10, capture_palette,
                                 &snapshot) == GEX_TITLE_CEL_INVALID);
    assert(!snapshot.pixels);
    memset(memory + capture_image, 0, 100);
    memory[capture_image + 16] = 4;
    tile(memory + capture_image + 20, 36, 8, 2, 0, 0);
    put32(memory + capture_image + 36, 5);
    memory[capture_image + 40] = 2;
    memory[capture_image + 41] = 0x10;
    memory[capture_image + 45] = 0x32;
    assert(gex_title_cel_capture(read_guest, memory, capture_image, capture_palette,
                                 &snapshot) == GEX_TITLE_CEL_UNSUPPORTED);
    assert(gex_title_cel_capture_map(read_guest, memory, capture_image, capture_palette,
                                     &snapshot) == GEX_TITLE_CEL_OK);
    assert(snapshot.cel.width == 8 && snapshot.cel.height == 2 &&
           snapshot.pixels[0] == 0x10000 && snapshot.pixels[1] == 0x10001 &&
           snapshot.pixels[8] == 0x10002 && snapshot.pixels[9] == 0x10003);
    assert(gex_title_cel_matches_image(read_guest, match_guest, memory, capture_image, &snapshot));
    memory[capture_image + 45] ^= 1;
    assert(!gex_title_cel_matches_image(read_guest, match_guest, memory, capture_image, &snapshot));
    assert(snapshot.pixels[8] == 0x10002);
    gex_title_cel_release(&snapshot);
    memory[capture_image + 40] = 3;
    assert(gex_title_cel_capture_map(read_guest, memory, capture_image, capture_palette,
                                     &snapshot) == GEX_TITLE_CEL_INVALID);
    put16(memory + capture_image + 20, 40);
    assert(gex_title_cel_capture_map(read_guest, memory, capture_image, capture_palette,
                                     &snapshot) == GEX_TITLE_CEL_UNSUPPORTED);
    memset(memory + capture_image, 0, 100);
    memory[capture_image + 16] = 4;
    tile(memory + capture_image + 20, 44, 8, 2, 0, 0);
    tile(memory + capture_image + 28, 52, 8, 1, 8, 0);
    put32(memory + capture_image + 44, 6);
    memory[capture_image + 48] = 0x82;
    memory[capture_image + 49] = 1;
    memory[capture_image + 50] = 0x10;
    memory[capture_image + 54] = 0x32;
    assert(gex_title_cel_capture_map(read_guest, memory, capture_image, capture_palette,
                                     &snapshot) == GEX_TITLE_CEL_OK);
    assert(snapshot.cel.width == 16 && snapshot.cel.height == 2 &&
           snapshot.pixels[1] == 0x10001 && snapshot.pixels[8] == 0x10002 &&
           snapshot.pixels[9] == 0x10003 && snapshot.pixels[17] == 0x10001);
    gex_title_cel_release(&snapshot);
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        for (size_t i = 0; i < sizeof memory; ++i) {
            random = random * 1664525u + 1013904223u;
            memory[i] = (uint8_t)(random >> 24);
        }
        memory[capture_image + 16] &= 3;
        int status = gex_title_cel_capture(read_guest, memory, capture_image,
                                            capture_palette, &snapshot);
        if (status == GEX_TITLE_CEL_OK) {
            assert(snapshot.pixels && snapshot.cel.width && snapshot.cel.height);
            gex_title_cel_release(&snapshot);
        } else {
            assert(status == GEX_TITLE_CEL_INVALID || status == GEX_TITLE_CEL_UNSUPPORTED ||
                   status == GEX_TITLE_CEL_OVERLAP || status == GEX_TITLE_CEL_NO_MEMORY);
            assert(!snapshot.pixels && !snapshot.palette_size);
        }
    }
    return 0;
}
