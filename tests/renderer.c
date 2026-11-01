#include <stdio.h>
#include <string.h>
#include "renderer.h"

static int failures, configured, presented, closed, pumped, observed;
static int reject_configure, reject_present, reject_open, reject_pump, device_configuration;

#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

static int open_renderer(int headless, char *error, size_t size) {
    CHECK(headless);
    if (reject_open) snprintf(error, size, "open failure");
    return reject_open;
}
static int configure(const gex_render_frame *frame, char *error, size_t size) {
    CHECK((frame->pixels != NULL) == !device_configuration &&
          frame->width > 0 && frame->pitch == ((frame->width * 2 + 3) & ~3));
    if (reject_configure) { snprintf(error, size, "configure failure"); return -1; }
    ++configured;
    return 0;
}
static int present(const gex_render_frame *frame, char *error, size_t size) {
    CHECK(frame->sequence);
    if (reject_present) { snprintf(error, size, "present failure"); return -1; }
    ++presented;
    return 0;
}
static int pump(char *error, size_t size) {
    ++pumped;
    if (reject_pump) snprintf(error, size, "pump failure");
    return reject_pump;
}
static void shutdown(void) { ++closed; }
static void observe(const gex_render_frame *frame) { CHECK(frame->pixels); ++observed; }
static void observe_device(const gex_render_frame *frame) {
    CHECK(!frame->pixels && frame->sequence && frame->width == 3 && frame->height == 2);
    ++observed;
}
static int upload_result = 1, uploads;
static int native_upload(const gex_render_frame *frame, const uint8_t *pixels,
                         int width, int height, int pitch, char *error, size_t size) {
    (void)pixels; (void)width; (void)height; (void)pitch;
    ++uploads;
    if (upload_result < 0) snprintf(error, size, "upload failure");
    if (!upload_result) memset(frame->pixels, 0x5A, (size_t)frame->pitch * frame->height);
    return upload_result;
}

static void check_upload(const uint8_t *input, int width, int height, int pitch) {
    gex_render_frame frame;
    CHECK(!gex_renderer_frame(&frame));
    memset(frame.pixels, 0xA5, (size_t)frame.pitch * frame.height);
    CHECK(!gex_renderer_upload_bgr555(input, width, height, pitch));
    for (int y = 0; y < frame.height; ++y) {
        int sy = y * height / frame.height;
        for (int x = 0; x < frame.width; ++x) {
            int sx = x * width / frame.width;
            const uint8_t *source = input + sy * pitch + sx * 2;
            unsigned value = source[0] | (unsigned)source[1] << 8;
            unsigned expected = (value & 31) << 11 |
                (value & 0x3E0) << 1 | (value >> 10 & 31);
            const uint8_t *pixel = frame.pixels + y * frame.pitch + x * 2;
            CHECK(pixel[0] == (uint8_t)expected && pixel[1] == (uint8_t)(expected >> 8));
        }
        for (int x = frame.width * 2; x < frame.pitch; ++x)
            CHECK(frame.pixels[y * frame.pitch + x] == 0xA5);
    }
}

static void check_conversion(void) {
    static uint32_t colors[32768];
    uint8_t *bytes = (uint8_t *)colors;
    for (unsigned i = 0; i < 65536; ++i) {
        bytes[i * 2] = (uint8_t)i;
        bytes[i * 2 + 1] = (uint8_t)(i >> 8);
    }
    CHECK(!gex_renderer_resize(256, 256));
    check_upload(bytes, 256, 256, 512);

    uint32_t storage[32];
    for (unsigned i = 0; i < sizeof storage; ++i)
        ((uint8_t *)storage)[i] = (uint8_t)(i * 73 + 19);
    for (int width = 1; width <= 9; ++width)
        for (int padding = 0; padding <= 3; ++padding)
            for (int offset = 0; offset <= 3; ++offset) {
                CHECK(!gex_renderer_resize(width, 5));
                check_upload((const uint8_t *)storage + offset, width, 3,
                             width * 2 + padding);
            }
    CHECK(!gex_renderer_resize(9, 2));
    check_upload(bytes, 3, 5, 6);
    CHECK(!gex_renderer_resize(3, 5));
    check_upload(bytes, 9, 2, 18);

    static uint32_t picture[320 * 224 / 2];
    for (unsigned i = 0; i < sizeof picture; ++i)
        ((uint8_t *)picture)[i] = (uint8_t)(i * 37 + i / 640);
    CHECK(!gex_renderer_resize(320, 240));
    check_upload((const uint8_t *)picture, 320, 224, 640);
    picture[0] ^= UINT32_MAX;
    check_upload((const uint8_t *)picture, 320, 224, 640);
    CHECK(!gex_renderer_resize(640, 480));
    check_upload((const uint8_t *)picture, 320, 224, 640);
}

int main(void) {
    static const gex_renderer_backend backend = {open_renderer, configure, present, pump, shutdown};
    gex_render_frame frame, old;
    CHECK(gex_renderer_commit() == -1);
    CHECK(gex_renderer_frame(&frame) == -1);
    CHECK(gex_renderer_pump() == -1);
    CHECK(gex_renderer_init(NULL, 1) == -1);
    reject_open = 1;
    CHECK(gex_renderer_init(&backend, 1) == -1);
    CHECK(!strcmp(gex_renderer_error(), "open failure") && closed == 1);
    reject_open = 0;
    CHECK(!gex_renderer_init(&backend, 1));
    CHECK(gex_renderer_init(&backend, 1) == -1);
    CHECK(gex_renderer_resize(0, 2) == -1);
    CHECK(gex_renderer_resize(8193, 2) == -1);
    CHECK(gex_renderer_resize(8192, 8192) == -1);
    int width = 0, height = 0;
    CHECK(!gex_renderer_parse_size("3840x2160", &width, &height));
    CHECK(width == 3840 && height == 2160);
    static const int fits[][8] = {
        {427, 240, 320, 224, 42, 0, 342, 240},
        {1920, 1080, 320, 224, 189, 0, 1542, 1080},
        {2560, 1080, 320, 224, 509, 0, 1542, 1080},
        {3440, 1440, 320, 224, 691, 0, 2057, 1440},
        {240, 427, 320, 224, 0, 129, 240, 168},
        {427, 240, 320, 240, 53, 0, 320, 240},
        {1920, 1080, 320, 240, 240, 0, 1440, 1080},
        {2560, 1080, 320, 240, 560, 0, 1440, 1080},
        {1, 8192, 320, 240, 0, 4095, 1, 1},
    };
    for (unsigned i = 0; i < sizeof fits / sizeof *fits; ++i) {
        const int *size = fits[i];
        gex_render_rect rect = gex_renderer_fit(size[0], size[1], size[2], size[3]);
        CHECK(rect.x == size[4] && rect.y == size[5] &&
              rect.width == size[6] && rect.height == size[7]);
    }
    const char *invalid[] = {"", "x", "10", "0x240", "-1x10", "10x-1",
        "10x", " 320x240", "320X240", "320x240 ", "10x10extra", "8193x1",
        "8192x8192", "999999999999999999999999999999x1"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i)
        CHECK(gex_renderer_parse_size(invalid[i], &width, &height) == -1);
    CHECK(!gex_renderer_resize(2, 2));
    CHECK(!gex_renderer_frame(&old));
    CHECK(!gex_renderer_resize(2, 2) && configured == 1);
    reject_configure = 1;
    CHECK(gex_renderer_resize(4, 4) == -1);
    CHECK(!strcmp(gex_renderer_error(), "configure failure"));
    CHECK(!gex_renderer_frame(&frame) && frame.pixels == old.pixels && frame.width == 2);
    reject_configure = 0;
    gex_renderer_observe(observe);
    const uint8_t input[] = {31, 0, 0xE0, 3, 0xAA, 0xBB, 0, 0x7C, 0xFF, 0x7F, 0xCC, 0xDD};
    const uint8_t expected[] = {0, 0xF8, 0xC0, 7, 31, 0, 0xDF, 0xFF};
    CHECK(!gex_renderer_upload_bgr555(input, 2, 2, 6));
    CHECK(presented == 1 && observed == 1);
    CHECK(!memcmp(frame.pixels, expected, sizeof expected));
    CHECK(gex_renderer_upload_bgr555(input, 2, 2, 3) == -1);
    CHECK(!gex_renderer_resize(4, 4));
    CHECK(!gex_renderer_upload_bgr555(input, 2, 2, 6));
    CHECK(!gex_renderer_frame(&frame));
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x)
            CHECK(!memcmp(frame.pixels + y * frame.pitch + x * 2,
                          expected + (y / 2 * 2 + x / 2) * 2, 2));
    reject_present = 1;
    CHECK(gex_renderer_commit() == -1);
    CHECK(!strcmp(gex_renderer_error(), "present failure") && observed == 2);
    CHECK(!gex_renderer_pump() && pumped == 1);
    reject_pump = 1;
    CHECK(gex_renderer_pump() == -1);
    CHECK(!strcmp(gex_renderer_error(), "pump failure"));
    CHECK(!gex_renderer_resize(3, 1));
    CHECK(!gex_renderer_frame(&frame) && frame.pitch == 8);
    reject_present = 0;
    check_conversion();
    gex_renderer_shutdown();
    CHECK(closed == 2 && gex_renderer_frame(&frame) == -1);
    gex_renderer_shutdown();
    CHECK(closed == 2);
    gex_renderer_backend fused = backend;
    fused.upload_bgr555 = native_upload;
    CHECK(!gex_renderer_init(&fused, 1));
    CHECK(!gex_renderer_resize(2, 2));
    CHECK(!gex_renderer_frame(&frame));
    check_upload(input, 2, 2, 6);
    uint32_t sequence = frame.sequence;
    upload_result = -1;
    CHECK(gex_renderer_upload_bgr555(input, 2, 2, 6) == -1);
    CHECK(!strcmp(gex_renderer_error(), "upload failure"));
    CHECK(!gex_renderer_frame(&frame) && frame.sequence == sequence + 1);
    upload_result = 0;
    gex_renderer_observe(observe);
    int observations = observed, presentations = presented;
    CHECK(!gex_renderer_upload_bgr555(input, 2, 2, 6));
    CHECK(observed == observations + 1 && presented == presentations && uploads == 3);
    CHECK(frame.pixels[0] == 0x5A && frame.pixels[7] == 0x5A);
    CHECK(!gex_renderer_resize(3, 2));
    CHECK(!gex_renderer_frame(&frame));
    sequence = frame.sequence;
    device_configuration = 1;
    CHECK(!gex_renderer_device_mode(1));
    CHECK(!gex_renderer_frame(&frame) && !frame.pixels);
    CHECK(gex_renderer_commit() == -1);
    CHECK(gex_renderer_upload_bgr555(input, 2, 2, 6) == -1);
    gex_renderer_observe(observe_device);
    observations = observed; presentations = presented;
    CHECK(!gex_renderer_device_commit());
    CHECK(observed == observations + 1 && presented == presentations);
    CHECK(!gex_renderer_frame(&frame) && frame.sequence == sequence + 1 && !frame.pixels);
    device_configuration = 0;
    CHECK(!gex_renderer_device_mode(0));
    CHECK(!gex_renderer_frame(&frame) && frame.pixels);
    CHECK(gex_renderer_device_commit() == -1);
    gex_renderer_shutdown();
    return failures ? 1 : 0;
}
