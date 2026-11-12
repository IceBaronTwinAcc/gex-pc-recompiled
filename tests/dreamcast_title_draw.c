#include "platform/dreamcast/title_draw.h"
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { MEMORY_SIZE = 0x4b0000, OBJECT = 0x10000, CEL = 0x11000,
       LIST = 0x12000, PART = 0x13000, IMAGE = 0x14000,
       PALETTE = 0x15000, ARENA = 0x30000, TAIL0 = 0x28000, TAIL1 = 0x28010 };

static uint8_t memory[MEMORY_SIZE];
static unsigned submissions, matches;
static uint16_t rendered[4];
static float rendered_x, rendered_y;
static uint64_t rendered_id;
static unsigned rendered_rgb;
static int rendered_vertex_tint;
static unsigned backdrop_submissions;
static uint16_t backdrop_color;

static void put16(uint32_t address, uint16_t value) {
    memory[address] = (uint8_t)value;
    memory[address + 1] = (uint8_t)(value >> 8);
}

static void put32(uint32_t address, uint32_t value) {
    put16(address, (uint16_t)value);
    put16(address + 2, (uint16_t)(value >> 16));
}

static uint32_t get32(uint32_t address) {
    return memory[address] | (uint32_t)memory[address + 1] << 8 |
           (uint32_t)memory[address + 2] << 16 | (uint32_t)memory[address + 3] << 24;
}

static int read_guest(void *context, uint32_t address, void *out, size_t size) {
    (void)context;
    if (address > MEMORY_SIZE || size > MEMORY_SIZE - address) return -1;
    memcpy(out, memory + address, size);
    return 0;
}

static int match_guest(void *context, uint32_t address, const void *data, size_t size) {
    (void)context;
    ++matches;
    if (address > MEMORY_SIZE || size > MEMORY_SIZE - address) return -1;
    return memcmp(memory + address, data, size) == 0;
}

static void write_guest(void *context, uint32_t address, const void *data, size_t size) {
    (void)context;
    assert(address <= MEMORY_SIZE && size <= MEMORY_SIZE - address);
    memcpy(memory + address, data, size);
}

static void allocated(void *context, uint32_t next) {
    (void)context;
    uint32_t begin = get32(0x4a2ae4);
    if (next < begin) begin = get32(0x4a2ae0);
    gex_dreamcast_title_forget(begin, next);
}

static int submit(void *target, const gex_gpu_vertex q[4],
                  int left, int top, int right, int bottom,
                  const gex_gpu_image *image, uint64_t image_id, unsigned rgb,
                  int vertex_tint, char *error, size_t size) {
    (void)target; (void)error; (void)size;
    assert(left == 0 && top == 8 && right == 320 && bottom == 232);
    assert(image->width == 2 && image->height == 2 && !image->half);
    memcpy(rendered, image->opaque, sizeof rendered);
    rendered_x = q[0].x; rendered_y = q[0].y;
    rendered_id = image_id;
    rendered_rgb = rgb;
    rendered_vertex_tint = vertex_tint;
    ++submissions;
    return 0;
}

static int submit_large(void *target, const gex_gpu_vertex q[4],
                        int left, int top, int right, int bottom,
                        const gex_gpu_image *image, uint64_t image_id, unsigned rgb,
                        int vertex_tint, char *error, size_t size) {
    (void)target; (void)q; (void)left; (void)top; (void)right; (void)bottom;
    (void)image_id; (void)rgb; (void)vertex_tint; (void)error; (void)size;
    assert(image->width == 255 && image->height == 255);
    rendered[0] = image->opaque[0];
    return 0;
}

static int submit_backdrop(void *target, const gex_gpu_vertex q[4],
                           int left, int top, int right, int bottom,
                           const gex_gpu_image *image, uint64_t image_id, unsigned rgb,
                           int vertex_tint, char *error, size_t size) {
    (void)target; (void)error; (void)size; (void)vertex_tint;
    assert(left == 0 && top == 8 && right == 320 && bottom == 232);
    assert(image->width == 64 && image->height == 32 && !image->half && image_id);
    assert(image->opaque[0] == backdrop_color);
    assert(q[0].x == (float)((backdrop_submissions / 8) * 64));
    assert(q[0].y == (float)((backdrop_submissions % 8) * 32));
    assert(rgb == 0x102030);
    ++backdrop_submissions;
    return 0;
}

static void setup(void) {
    memset(memory, 0, sizeof memory);
    put32(CEL + 0x18, LIST);
    put32(LIST, PART);
    put32(PART + 8, IMAGE);
    put32(PART + 12, PALETTE);
    put32(PART + 16, 0x100);
    put32(IMAGE, 2u << 16);
    put32(IMAGE + 4, 2u << 16);
    put16(IMAGE + 20, 44);
    memory[IMAGE + 22] = 2; memory[IMAGE + 23] = 2;
    memory[IMAGE + 44] = 0x21; memory[IMAGE + 48] = 0x43;
    put16(PALETTE + 2, 0x001f);
    put16(PALETTE + 4, 0x03e0);
    put16(PALETTE + 6, 0x7c00);
    put16(PALETTE + 8, 0x7fff);
    put32(OBJECT + 0x78, 10u << 16);
    put32(OBJECT + 0x7c, 20u << 16);
    put32(OBJECT + 0xe0, 0x01000000);
    put32(0x4a2afc, 0x00808080);
    put32(0x46004c, 0x00808080);
    put32(0x4a2ae0, ARENA);
    put32(0x4a2adc, ARENA + 1024);
    put32(0x4a2ae4, ARENA);
    put32(0x4a2b18, TAIL0);
    put32(0x4a2b14, TAIL1);
}

int main(void) {
    gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, match_guest};
    uint8_t modulation[1024];
    for (unsigned tint = 0; tint < 32; ++tint)
        for (unsigned channel = 0; channel < 32; ++channel) {
            unsigned value = tint * channel / 16;
            modulation[tint * 32 + channel] = (uint8_t)(value > 31 ? 31 : value);
        }
    gex_raster_state state = {.modulation = modulation, .left = 0, .top = 8,
                              .right = 320, .bottom = 232, .layout = 1};
    char error[128] = {0};
    setup();
    assert(gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error) == 1);
    assert(get32(TAIL0) == ARENA && get32(TAIL1) == ARENA + 8);
    assert(get32(0x4a2ae4) == ARENA + 16);
    assert(memory[ARENA + 7] == 0x7f && memory[ARENA + 15] == 0x7f);
    memset(memory + PALETTE, 0, 32);
    memset(memory + IMAGE + 44, 0, 8);
    assert(gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                      submit, error, sizeof error) == 1);
    assert(submissions == 1 && rendered_x == 10 && rendered_y == 20 &&
           rendered_vertex_tint);
    assert(rendered[0] == 0xfc00 && rendered[1] == 0x83e0 &&
           rendered[2] == 0x801f && rendered[3] == 0xffff);
    uint64_t first_id = rendered_id;
    assert(first_id);
    assert(gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                      submit, error, sizeof error) == 1);
    assert(rendered_id == first_id);
    memory[ARENA + 4] = memory[ARENA + 5] = memory[ARENA + 6] = 0x4d;
    assert(gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                      submit, error, sizeof error) == 1);
    assert(rendered_id == first_id && rendered_rgb == 0x4d4d4d);
    memory[ARENA + 4] = memory[ARENA + 5] = memory[ARENA + 6] = 0x80;
    memory[ARENA + 4] = 0;
    assert(gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                      submit, error, sizeof error) == 1);
    assert(rendered_rgb == 0x808000);
    assert(rendered[0] == 0 && rendered[1] == 0x83e0 && rendered[2] == 0x801f);
    assert(rendered_id && rendered_id != first_id);
    assert(gex_dreamcast_title_packet(NULL, ARENA + 8, memory + ARENA + 8, &state,
                                      submit, error, sizeof error) == 1);
    uint64_t before_modulation = rendered_id;
    modulation[16 * 32 + 31] = 15;
    assert(gex_dreamcast_title_packet(NULL, ARENA + 8, memory + ARENA + 8, &state,
                                      submit, error, sizeof error) == 1);
    assert(rendered[0] == 0xfc00 && rendered[1] == 0x83e0 && rendered[2] == 0x801f);
    assert(rendered_id == before_modulation);
    modulation[16 * 32 + 31] = 0;
    assert(gex_dreamcast_title_packet(NULL, ARENA + 8, memory + ARENA + 8, &state,
                                      submit, error, sizeof error) == 1);
    assert(rendered[0] == 0 && rendered[1] == 0 && rendered[2] == 0);
    assert(rendered_id != before_modulation);
    modulation[16 * 32 + 31] = 31;
    state.left = 12;
    unsigned before = submissions;
    assert(gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                      submit, error, sizeof error) == 1);
    assert(submissions == before);
    state.left = 0;
    gex_dreamcast_title_forget(ARENA + 2, ARENA + 3);
    assert(!gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                       submit, error, sizeof error));
    assert(gex_dreamcast_title_packet(NULL, ARENA + 8, memory + ARENA + 8, &state,
                                      submit, error, sizeof error) == 1);
    gex_dreamcast_title_forget(ARENA + 8, ARENA + 16);
    assert(!gex_dreamcast_title_packet(NULL, ARENA + 8, memory + ARENA + 8, &state,
                                       submit, error, sizeof error));
    gex_dreamcast_title_reset();
    setup();
    put32(PART + 4, 0x80000000);
    assert(!gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error));
    assert(get32(0x4a2ae4) == ARENA && get32(TAIL0) == 0 && get32(TAIL1) == 0);
    gex_dreamcast_title_reset();
    setup();
    put32(0x4a2adc, ARENA + 64);
    put32(0x4a2ae4, ARENA + 56);
    assert(gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error) == 1);
    assert(get32(TAIL0) == ARENA && get32(TAIL1) == ARENA + 8);
    assert(get32(0x4a2ae4) == ARENA + 16);
    gex_dreamcast_title_reset();
    setup();
    put32(0x4a2adc, ARENA + 64);
    put32(0x4a2ae4, ARENA + 56);
    put32(0x4a2b18, ARENA + 4);
    assert(!gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error));
    assert(get32(0x4a2ae4) == ARENA + 56 && get32(0x4a2b18) == ARENA + 4 &&
           get32(TAIL1) == 0);
    gex_dreamcast_title_reset();
    setup();
    assert(gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error) == 1);
    unsigned previous_matches = matches;
    put16(PALETTE + 2, 0x7c00);
    assert(gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error) == 1);
    assert(matches > previous_matches);
    assert(gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                      submit, error, sizeof error) == 1);
    assert(rendered[0] == 0xfc00);
    uint64_t red_id = rendered_id;
    assert(gex_dreamcast_title_packet(NULL, ARENA + 16, memory + ARENA + 16, &state,
                                      submit, error, sizeof error) == 1);
    assert(rendered[0] == 0x801f && rendered_id && rendered_id != red_id);
    gex_dreamcast_title_forget(ARENA, ARENA + 16);
    assert(gex_dreamcast_title_packet(NULL, ARENA + 16, memory + ARENA + 16, &state,
                                      submit, error, sizeof error) == 1);
    assert(rendered[0] == 0x801f);
    gex_dreamcast_title_reset();
    setup();
    assert(gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error) == 1);
    put16(PALETTE + 2, 0x7c00);
    assert(gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error) == 1);
    memory[IMAGE + 44] = 0x22;
    put16(PALETTE + 4, 0x03e0);
    assert(gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error) == 1);
    gex_dreamcast_title_forget(ARENA + 16, ARENA + 32);
    assert(gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                      submit, error, sizeof error) == 1);
    assert(rendered[0] == 0xfc00);
    assert(gex_dreamcast_title_packet(NULL, ARENA + 32, memory + ARENA + 32, &state,
                                      submit, error, sizeof error) == 1);
    assert(rendered[0] == 0x83e0);
    gex_dreamcast_title_reset();
    setup();
    memory[IMAGE + 16] = 2;
    put16(IMAGE + 44, 0x001f); put16(IMAGE + 46, 0x03e0);
    put16(IMAGE + 60, 0x7c00); put16(IMAGE + 62, 0x7fff);
    assert(gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error) == 1);
    assert(gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                      submit, error, sizeof error) == 1);
    assert(!rendered_vertex_tint && rendered[0] == 0xfc00);
    uint64_t direct_id = rendered_id;
    memory[ARENA + 4] = memory[ARENA + 5] = memory[ARENA + 6] = 0x4d;
    assert(gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                      submit, error, sizeof error) == 1);
    assert(rendered_vertex_tint && rendered_id == direct_id && rendered[0] == 0xfc00);
    memory[ARENA + 4] = 0x80;
    assert(gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                      submit, error, sizeof error) == 1);
    assert(!rendered_vertex_tint && rendered_id == direct_id);
    gex_dreamcast_title_reset();
    setup();
    enum { BIG_IMAGE = 0x60000 };
    put32(PART + 8, BIG_IMAGE);
    put32(BIG_IMAGE, 255u << 16);
    put32(BIG_IMAGE + 4, 255u << 16);
    put16(BIG_IMAGE + 20, 44);
    memory[BIG_IMAGE + 22] = 255;
    memory[BIG_IMAGE + 23] = 255;
    memset(memory + BIG_IMAGE + 44, 0x11, 128 * 255);
    unsigned recorded = 0;
    for (unsigned i = 0; i < 16; ++i) {
        memory[BIG_IMAGE + 44] = (uint8_t)(0x10 | (i & 1 ? 2 : 1));
        memory[BIG_IMAGE + 45] = (uint8_t)i;
        uint32_t previous = get32(0x4a2ae4);
        int result = gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error);
        assert(result >= 0);
        if (!result) {
            assert(get32(0x4a2ae4) == previous);
            break;
        }
        ++recorded;
    }
    assert(recorded > 1 && recorded < 16);
    assert(gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                      submit_large, error, sizeof error) == 1);
    assert(rendered[0] == 0xfc00);
    assert(gex_dreamcast_title_packet(NULL, ARENA + 16, memory + ARENA + 16, &state,
                                      submit_large, error, sizeof error) == 1);
    assert(rendered[0] == 0x83e0);
    gex_dreamcast_title_reset();
    setup();
    enum { BACKDROP = 0x460258 };
    put32(0x4a2964, 49);
    put32(0x46a534, PALETTE);
    put32(0x4a2adc, ARENA + 2048);
    memory[BACKDROP + 16] = 0x80;
    put16(BACKDROP + 20, 28);
    memory[BACKDROP + 22] = 64;
    memory[BACKDROP + 23] = 32;
    memset(memory + BACKDROP + 28, 0x11, 64 * 32 / 2);
    memory[0x4a2af8] = 0x10;
    memory[0x4a2af9] = 0x20;
    memory[0x4a2afa] = 0x30;
    assert(gex_dreamcast_backdrop_record(&io, 49, error, sizeof error) == 1);
    assert(get32(0x4a2ae4) == ARENA + 40 * 16);
    uint32_t first = get32(TAIL0), second = get32(TAIL1);
    memset(memory + BACKDROP + 28, 0x22, 64 * 32 / 2);
    put32(0x4a2964, 3);
    assert(gex_dreamcast_backdrop_record(&io, 49, error, sizeof error) == 1);
    backdrop_color = 0xfc00;
    for (unsigned i = 0; i < 40; ++i) {
        assert(first == ARENA + i * 16 && second == first + 8);
        assert(gex_dreamcast_title_packet(NULL, first, memory + first, &state,
                                          submit_backdrop, error, sizeof error) == 1);
        first = get32(first);
        second = get32(second);
    }
    assert(backdrop_submissions == 40);
    backdrop_submissions = 0;
    backdrop_color = 0x83e0;
    for (unsigned i = 0; i < 40; ++i)
        assert(gex_dreamcast_title_packet(NULL, ARENA + 40 * 16 + i * 16,
                                          memory + ARENA + 40 * 16 + i * 16, &state,
                                          submit_backdrop, error, sizeof error) == 1);
    assert(backdrop_submissions == 40);
    uint32_t next = get32(0x4a2ae4);
    memory[BACKDROP + 22] = 63;
    assert(!gex_dreamcast_backdrop_record(&io, 49, error, sizeof error));
    assert(get32(0x4a2ae4) == next);
    gex_dreamcast_title_reset();
    setup();
    put32(0x4a2964, 55);
    assert(!gex_dreamcast_backdrop_record(&io, 55, error, sizeof error));
    assert(get32(0x4a2ae4) == ARENA && !get32(TAIL0) && !get32(TAIL1));
    assert(!gex_dreamcast_map_cel_record(&io, 55, OBJECT, CEL, error, sizeof error));
    assert(get32(0x4a2ae4) == ARENA);
    assert(gex_dreamcast_map_cel_record(&io, 49, OBJECT, CEL, error, sizeof error) == 1);
    assert(get32(0x4a2ae4) == ARENA + 16);
    gex_dreamcast_title_reset();
    return 0;
}
