#include "platform/dreamcast/map_draw.h"
#include "platform/dreamcast/title_draw.h"
#undef NDEBUG
#include <assert.h>
#include <string.h>

enum { SIZE = 0x4b0000, CHUNK = 0x10000, TABLE = 0x11000, GROUP = 0x12000,
       DEFS = 0x13000, IMAGE = 0x14000, PAGES = 0x15000, ARENA = 0x30000 };
static uint8_t memory[SIZE];
static unsigned allocations, writes;
static unsigned palette_calls;
static uint32_t palette_addresses[8];
static int palette_retire_anchor;
static uint32_t allocation_next[256], tracked_markers[8];
static unsigned marker_count, retired_markers;
static int retire_title_markers;

static void put16(uint32_t at, uint16_t value) {
    memory[at] = (uint8_t)value;
    memory[at + 1] = (uint8_t)(value >> 8);
}
static void put32(uint32_t at, uint32_t value) {
    put16(at, (uint16_t)value);
    put16(at + 2, (uint16_t)(value >> 16));
}
static uint32_t get32(uint32_t at) {
    return memory[at] | (uint32_t)memory[at + 1] << 8 |
           (uint32_t)memory[at + 2] << 16 | (uint32_t)memory[at + 3] << 24;
}
static uint16_t get16(uint32_t at) {
    return memory[at] | (uint16_t)memory[at + 1] << 8;
}
static int read_guest(void *ctx, uint32_t at, void *out, size_t size) {
    (void)ctx;
    if (at > SIZE || size > SIZE - at) return -1;
    memcpy(out, memory + at, size);
    return 0;
}
static void write_guest(void *ctx, uint32_t at, const void *data, size_t size) {
    (void)ctx;
    assert(at <= SIZE && size <= SIZE - at);
    memcpy(memory + at, data, size);
    ++writes;
}
static void allocated(void *ctx, uint32_t next) {
    (void)ctx;
    assert(allocations < sizeof allocation_next / sizeof allocation_next[0]);
    allocation_next[allocations] = next;
    ++allocations;
    uint32_t begin = get32(0x4a2ae4);
    if (next < begin) begin = get32(0x4a2ae0);
    if (begin == next) return;
    if (retire_title_markers) gex_dreamcast_title_forget(begin, next);
    for (unsigned i = 0; i < marker_count; ++i)
        if (tracked_markers[i] < next && tracked_markers[i] + 8 > begin)
            retired_markers |= 1u << i;
}

static uint16_t prepare_palette(void *ctx, uint32_t address) {
    (void)ctx;
    assert(palette_calls < sizeof palette_addresses / sizeof palette_addresses[0]);
    palette_addresses[palette_calls++] = address;
    if (palette_retire_anchor && palette_calls == 2)
        gex_dreamcast_title_forget(ARENA, ARENA + 24);
    return get16(address);
}

static int submit_snapshot(void *target, const gex_gpu_vertex vertices[4],
                           int left, int top, int right, int bottom,
                           const gex_gpu_image *image, uint64_t image_id,
                           unsigned rgb, int vertex_tint, char *error, size_t size) {
    (void)target; (void)vertices; (void)left; (void)top; (void)right; (void)bottom;
    (void)image_id; (void)rgb; (void)vertex_tint; (void)error; (void)size;
    assert(image->width == 2 && image->height == 2 && image->opaque[0] == 0xfc00);
    return 0;
}

static void check_chunk_snapshot_retirement(void) {
    enum { OBJECT = 0x17000, CEL = 0x18000, LIST = 0x19000,
           PART = 0x1a000, TITLE_IMAGE = 0x1b000, PALETTE = 0x1c000 };
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    uint8_t modulation[1024];
    for (unsigned tint = 0; tint < 32; ++tint)
        for (unsigned channel = 0; channel < 32; ++channel) {
            unsigned value = tint * channel / 16;
            modulation[tint * 32 + channel] = (uint8_t)(value > 31 ? 31 : value);
        }
    gex_raster_state state = {.modulation = modulation, .left = 0, .top = 8,
                              .right = 320, .bottom = 232, .layout = 1};
    char error[128] = {0};
    memset(memory, 0, sizeof memory);
    allocations = writes = 0;
    retire_title_markers = 1;
    put32(0x4a2ae0, ARENA); put32(0x4a2adc, ARENA + 1024);
    put32(0x4a2ae4, ARENA); put32(0x4a2b18, 0x28000); put32(0x4a2b14, 0x28010);
    put32(CEL + 0x18, LIST); put32(LIST, PART);
    put32(PART + 8, TITLE_IMAGE); put32(PART + 12, PALETTE); put32(PART + 16, 0x100);
    put32(TITLE_IMAGE, 2u << 16); put32(TITLE_IMAGE + 4, 2u << 16);
    put16(TITLE_IMAGE + 20, 44);
    memory[TITLE_IMAGE + 22] = 2; memory[TITLE_IMAGE + 23] = 2;
    memory[TITLE_IMAGE + 44] = 0x21; memory[TITLE_IMAGE + 48] = 0x43;
    put16(PALETTE + 2, 0x001f);
    put32(OBJECT + 0x78, 10u << 16); put32(OBJECT + 0x7c, 20u << 16);
    put32(OBJECT + 0xe0, 0x01000000);
    put32(0x4a2afc, 0x00808080); put32(0x46004c, 0x00808080);
    assert(gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error) == 1);
    assert(gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error) == 1);
    put32(0x4a2ae4, ARENA + 80);
    assert(gex_dreamcast_title_record(&io, OBJECT, CEL, error, sizeof error) == 1);
    put32(0x4a2ae4, ARENA);
    memset(memory + PALETTE, 0, 32);
    memset(memory + TITLE_IMAGE + 44, 0, 8);

    put32(0x4a2ae8, TABLE); put32(0x460e08, PAGES);
    put32(0x46bc74, 0x46bc64); put32(0x46bc70, 0x46bc60);
    put32(0x46bc6c, 0x46bc58); put32(0x46bc68, 0x46bc5c);
    put32(TABLE, GROUP); put16(CHUNK + 8, 1); put16(GROUP + 16, 1);
    put32(DEFS + 12, IMAGE); memory[IMAGE + 16] = 2;
    put16(IMAGE + 20, 1); memory[IMAGE + 22] = 24; memory[IMAGE + 23] = 20;
    put16(PAGES, 0x103);
    assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 1);
    assert(!gex_dreamcast_title_packet(NULL, ARENA, memory + ARENA, &state,
                                       submit_snapshot, error, sizeof error));
    assert(!gex_dreamcast_title_packet(NULL, ARENA + 16, memory + ARENA + 16, &state,
                                       submit_snapshot, error, sizeof error));
    assert(gex_dreamcast_title_packet(NULL, ARENA + 80, memory + ARENA + 80, &state,
                                      submit_snapshot, error, sizeof error) == 1);
    gex_dreamcast_title_reset();
    retire_title_markers = 0;
}

static void check_chunk_retirement(void) {
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        memset(memory, 0, sizeof memory);
        allocations = writes = retired_markers = 0;
        marker_count = 4;
        put32(0x4a2ae8, TABLE); put32(0x460e08, PAGES);
        put32(0x4a2ae0, ARENA);
        put32(0x46bc74, 0x46bc64); put32(0x46bc70, 0x46bc60);
        put32(0x46bc6c, 0x46bc58); put32(0x46bc68, 0x46bc5c);
        put32(TABLE, GROUP); put16(CHUNK + 8, 1); put16(CHUNK + 10, 1);
        put16(CHUNK + 12, 1); put16(GROUP + 16, 1);
        put32(DEFS + 12, IMAGE);
        memory[IMAGE + 16] = 2; put16(IMAGE + 20, 1);
        memory[IMAGE + 22] = 24; memory[IMAGE + 23] = 20;
        put16(PAGES, 0x103);
        if (scenario == 0) {
            put32(0x4a2adc, ARENA + 256); put32(0x4a2ae4, ARENA);
            tracked_markers[0] = ARENA + 8; tracked_markers[1] = ARENA + 44;
            tracked_markers[2] = ARENA + 136; tracked_markers[3] = ARENA + 180;
            assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 1);
            assert(allocations == 1 && allocation_next[0] == ARENA + 144);
            assert(retired_markers == 7 && get32(0x4a2ae4) == ARENA + 144);
            assert(get32(ARENA + 40) == ARENA && get32(ARENA) == ARENA + 64);
            assert(get32(ARENA + 64) == ARENA + 104);
        } else if (scenario == 1) {
            put32(0x4a2adc, ARENA + 256); put32(0x4a2ae4, ARENA + 200);
            tracked_markers[0] = ARENA + 208; tracked_markers[1] = ARENA + 8;
            tracked_markers[2] = ARENA + 80; tracked_markers[3] = ARENA + 180;
            assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 1);
            assert(allocations == 2 && allocation_next[0] == ARENA + 240 &&
                   allocation_next[1] == ARENA + 104);
            assert(retired_markers == 7 && get32(0x4a2ae4) == ARENA + 104);
            assert(get32(0x46bc64) == ARENA && get32(ARENA) == ARENA + 200);
            assert(get32(ARENA + 200) == ARENA + 24);
        } else if (scenario == 2) {
            put16(CHUNK + 10, 0); put16(CHUNK + 12, 0);
            put32(0x4a2adc, ARENA + 64); put32(0x4a2ae4, ARENA + 40);
            tracked_markers[0] = ARENA + 8; tracked_markers[1] = ARENA + 40;
            tracked_markers[2] = ARENA + 56; tracked_markers[3] = ARENA + 80;
            assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 0);
            assert(!allocations && !writes && !retired_markers);
            assert(get32(0x4a2ae4) == ARENA + 40);
        } else if (scenario == 3) {
            put16(CHUNK + 12, 0);
            put32(0x4a2adc, ARENA + 128); put32(0x4a2ae4, ARENA + 100);
            tracked_markers[0] = ARENA + 8; tracked_markers[1] = ARENA + 48;
            tracked_markers[2] = ARENA + 80; tracked_markers[3] = ARENA + 112;
            assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 1);
            assert(allocations == 2 && allocation_next[0] == ARENA + 64 &&
                   allocation_next[1] == ARENA + 104);
            assert(retired_markers == 7 && get32(0x4a2ae4) == ARENA + 104);
        } else if (scenario == 4) {
            put32(0x4a2adc, ARENA + 112); put32(0x4a2ae4, ARENA + 80);
            assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 0);
            assert(!allocations && !writes && !retired_markers);
            assert(get32(0x4a2ae4) == ARENA + 80);
        } else {
            put16(CHUNK + 10, 0); put16(CHUNK + 12, 0);
            put32(0x4a2adc, ARENA + 64); put32(0x4a2ae4, ARENA + 28);
            assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 0);
            assert(!allocations && !writes && !retired_markers);
            assert(get32(0x4a2ae4) == ARENA + 28);
        }
    }
    marker_count = 0;
}

static void check_chunk_flips(void) {
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    for (unsigned flags = 0x4000; flags <= 0xc000; flags += 0x4000) {
        memset(memory, 0, sizeof memory);
        allocations = writes = 0;
        put32(0x4a2ae8, TABLE); put32(0x460e08, PAGES);
        put32(0x4a2ae0, ARENA); put32(0x4a2adc, ARENA + 1024);
        put32(0x4a2ae4, ARENA);
        put32(0x46bc74, 0x46bc64); put32(0x46bc70, 0x46bc60);
        put32(0x46bc6c, 0x46bc58); put32(0x46bc68, 0x46bc5c);
        put16(0x4a2a96, 2); put16(0x4a2a94, (uint16_t)-3);
        memory[0x4a2af8] = 0x11; memory[0x4a2af9] = 0x22; memory[0x4a2afa] = 0x33;
        put32(TABLE, GROUP); put16(CHUNK + 8, 1);
        put16(GROUP + 16, (uint16_t)(flags | 1)); put16(GROUP + 24, 1);
        put32(DEFS + 12, IMAGE);
        memory[IMAGE + 16] = 2;
        put16(IMAGE + 20, 1);
        memory[IMAGE + 22] = 24; memory[IMAGE + 23] = 20;
        put16(IMAGE + 24, 4); put16(IMAGE + 26, 6);
        put16(PAGES, 0x103); put16(PAGES + 2, 0x1234);
        put16(ARENA + 30, 0x9876);
        assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 1);
        assert(allocations == 1 && get32(0x4a2ae4) == ARENA + 120);
        assert(get32(0x46bc64) == ARENA && get32(0x46bc60) == ARENA + 40);
        assert(get32(ARENA) == ARENA + 80 && get32(ARENA + 40) == ARENA + 100);
        uint16_t x0 = (flags & 0x4000) ? 28 : 4;
        uint16_t y0 = (flags & 0x8000) ? 26 : 6;
        uint16_t x1 = (uint16_t)(x0 + ((flags & 0x4000) ? -24 : 24));
        uint16_t y1 = (uint16_t)(y0 + ((flags & 0x8000) ? -20 : 20));
        assert(get32(ARENA + 4) == 0x2c112233);
        assert(get16(ARENA + 8) == x0 && get16(ARENA + 10) == y0);
        assert(get16(ARENA + 16) == x1 && get16(ARENA + 26) == y1);
        assert(get16(ARENA + 32) == x1 && get16(ARENA + 34) == y1);
        assert(get16(ARENA + 48) == (uint16_t)(x0 + 2));
        assert(get16(ARENA + 50) == (uint16_t)(y0 - 3));
        assert(get16(ARENA + 12) == 0x1234 && get16(ARENA + 20) == 0x124b);
        assert(get16(ARENA + 28) == 0x2534 && get16(ARENA + 36) == 0x254b);
        assert(get16(ARENA + 22) == 0x103);
        assert(get16(ARENA + 30) == 0x9876 && get16(ARENA + 70) == 0x9876);
    }
}

typedef struct {
    gex_gpu_vertex q[4];
    gex_gpu_material material;
    int left, top, right, bottom;
    unsigned calls;
} tile_capture;

static int capture_tile(void *target, const gex_gpu_vertex q[4],
                        int left, int top, int right, int bottom,
                        const gex_gpu_material *material, char *error, size_t size) {
    (void)error; (void)size;
    tile_capture *capture = target;
    assert(!capture->calls++);
    memcpy(capture->q, q, sizeof capture->q);
    capture->material = *material;
    capture->left = left; capture->top = top;
    capture->right = right; capture->bottom = bottom;
    return 0;
}

static int capture_tile_order(void *target, const gex_gpu_vertex q[4],
                              int left, int top, int right, int bottom,
                              const gex_gpu_material *material, char *error, size_t size) {
    (void)q; (void)left; (void)top; (void)right; (void)bottom; (void)error; (void)size;
    tile_capture *capture = target;
    assert(capture->calls < 4);
    capture->q[capture->calls++].x = (float)material->rgb;
    return 0;
}

static int capture_tile_sequence(void *target, const gex_gpu_vertex q[4],
                                 int left, int top, int right, int bottom,
                                 const gex_gpu_material *material, char *error, size_t size) {
    tile_capture *captures = target;
    unsigned index = captures[0].calls;
    assert(index < 2);
    if (index) captures[1].calls = 0;
    int result = capture_tile(&captures[index], q, left, top, right, bottom,
                              material, error, size);
    if (index) ++captures[0].calls;
    return result;
}

static int capture_world_backdrop(void *target, const gex_gpu_vertex q[4],
                                  int left, int top, int right, int bottom,
                                  const gex_gpu_material *material, char *error, size_t size) {
    (void)error; (void)size;
    unsigned *count = target, i = (*count)++;
    assert(i < 40 && left == 3 && top == 1 && right == 300 && bottom == 232);
    assert(q[0].x == (float)((i / 8) * 64) && q[0].y == (float)((int)(i % 8) * 32 - 8));
    assert(q[3].x == q[0].x + 64 && q[3].y == q[0].y + 32);
    assert(q[0].u == 248 && q[0].v == 250 && q[3].u == 312 && q[3].v == 282);
    assert(material->page == 0x081 && material->clut == 0x456 &&
           material->opcode == 0x64 && material->rgb == 0x102030);
    return 0;
}

static void check_direct_world_backdrop(void) {
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    const uint32_t roots[2] = {0x28000, 0x28010};
    char error[128] = {0};
    memset(memory, 0, sizeof memory);
    allocations = writes = 0;
    put32(0x4a2ae4, ARENA);
    put32(0x4a2b18, roots[0]); put32(0x4a2b14, roots[1]);
    memory[PAGES + 12] = 0xfe; memory[PAGES + 14] = 250;
    memory[0x4a2af8] = 0x10; memory[0x4a2af9] = 0x20; memory[0x4a2afa] = 0x30;
    assert(gex_dreamcast_backdrop_tiles_direct(&io, PAGES, 0x456, roots) == 1);
    assert(!allocations && !writes && get32(0x4a2ae4) == ARENA);
    for (unsigned copy = 0; copy < 2; ++copy) {
        gex_raster_state state = {.page = 0x081, .left = 0, .top = 8,
                                  .right = 320, .bottom = 240, .world_clip = 1,
                                  .world_left = 3, .world_top = 9,
                                  .world_right = 300, .world_bottom = 240};
        gex_gpu_direct_state result = {0};
        unsigned count = 0;
        assert(gex_dreamcast_title_draw_direct_tiles(&count, roots[copy], roots[copy],
                   &state, NULL, capture_world_backdrop, &result, error, sizeof error) == 1);
        assert(count == 40 && !result.page_valid && !result.page_command_valid);
    }
    gex_dreamcast_title_reset();
    assert(!gex_dreamcast_backdrop_tiles_direct(&io, 0, 0x456, roots));
    assert(!gex_dreamcast_title_has_direct(roots[0], roots[0]));
}

static void setup_direct_chunk(unsigned flags) {
    memset(memory, 0, sizeof memory);
    allocations = writes = 0;
    put32(0x4a2ae8, TABLE); put32(0x460e08, PAGES);
    put32(0x4a2ae0, ARENA); put32(0x4a2adc, ARENA + 2048);
    put32(0x4a2ae4, ARENA);
    put32(0x46bc74, 0x46bc64); put32(0x46bc70, 0x46bc60);
    put32(0x46bc6c, 0x46bc58); put32(0x46bc68, 0x46bc5c);
    put16(0x4a2a96, 2); put16(0x4a2a94, (uint16_t)-3);
    memory[0x4a2af8] = 0x11; memory[0x4a2af9] = 0x22; memory[0x4a2afa] = 0x33;
    put32(TABLE, GROUP); put16(CHUNK + 8, 1);
    put16(GROUP + 16, (uint16_t)(flags | 1)); put16(GROUP + 24, 0x2001);
    put32(DEFS + 12, IMAGE);
    memory[IMAGE + 16] = 2; put16(IMAGE + 20, 1);
    memory[IMAGE + 22] = 24; memory[IMAGE + 23] = 20;
    put16(IMAGE + 24, 4); put16(IMAGE + 26, 6);
    put16(PAGES, 0x103); put16(PAGES + 2, 0x12e9);
}

static void check_direct_chunk(void) {
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    const uint32_t roots[2] = {0x28000, 0x28010};
    const uint32_t tails[2][2] = {{0x29000, 0x29010}, {0x29020, 0x29030}};
    const uint32_t source[2][2] = {{0x46bc64, 0x46bc60}, {0x46bc58, 0x46bc5c}};
    uint8_t expected[2][2][40];
    char error[128] = {0};
    for (unsigned flags = 0; flags <= 0xc000; flags += 0x4000) {
        setup_direct_chunk(flags);
        assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 1);
        for (unsigned layer = 0; layer < 2; ++layer)
            for (unsigned copy = 0; copy < 2; ++copy) {
                uint32_t packet = get32(source[layer][copy]);
                if (memory[packet + 7] == 0xe1) packet = get32(packet);
                memcpy(expected[layer][copy], memory + packet, 40);
            }
        setup_direct_chunk(flags);
        assert(gex_dreamcast_map_chunk_direct(&io, CHUNK, DEFS, 0, 0, 320, 240) == 1);
        assert(!allocations && !writes && get32(0x4a2ae4) == ARENA);
        for (unsigned layer = 0; layer < 2; ++layer) {
            gex_dreamcast_title_merge_lists(source[layer], roots, tails[layer]);
            for (unsigned copy = 0; copy < 2; ++copy) {
                gex_raster_state state = {.left = 0, .top = 8, .right = 320, .bottom = 240,
                                          .world_clip = 1, .world_left = 3, .world_top = 9,
                                          .world_right = 300, .world_bottom = 240};
                gex_gpu_direct_state result = {0};
                tile_capture capture = {0};
                assert(gex_dreamcast_title_draw_direct_tiles(&capture, roots[copy], tails[layer][copy],
                           &state, NULL, capture_tile, &result, error, sizeof error) == 1);
                assert(capture.calls == 1 && capture.left == 3 && capture.top == 1 &&
                       capture.right == 300 && capture.bottom == 232);
                const uint8_t *p = expected[layer][copy];
                int quad = (p[7] & ~2u) == 0x2c;
                assert(capture.material.page == 0x103 && capture.material.opcode == p[7]);
                assert(capture.material.rgb == 0x112233 && result.page_valid &&
                       result.page == 0x103 && result.page_command_valid == !quad);
                int x0 = (int16_t)(p[8] | (uint16_t)p[9] << 8);
                int y0 = (int16_t)(p[10] | (uint16_t)p[11] << 8);
                for (unsigned i = 0; i < 4; ++i) {
                    int x = quad ? (int16_t)(p[8 + 8*i] | (uint16_t)p[9 + 8*i] << 8) :
                                   x0 + ((i & 1) ? 24 : 0);
                    int y = quad ? (int16_t)(p[10 + 8*i] | (uint16_t)p[11 + 8*i] << 8) :
                                   y0 + ((i & 2) ? 20 : 0);
                    unsigned u = quad ? p[12 + 8*i] : p[12] + ((i & 1) ? 24 : 0);
                    unsigned v = quad ? p[13 + 8*i] : p[13] + ((i & 2) ? 20 : 0);
                    if (quad && (i == 1 || i == 3) && !u) u = 256;
                    if (quad && (i == 2 || i == 3) && !v) v = 256;
                    assert(capture.q[i].x == x && capture.q[i].y == y - 8);
                    assert(capture.q[i].u == u && capture.q[i].v == v);
                }
            }
        }
        gex_dreamcast_title_reset();
    }
    setup_direct_chunk(0);
    memory[IMAGE + 16] = 1;
    assert(!gex_dreamcast_map_chunk_direct(&io, CHUNK, DEFS, 0, 0, 320, 240));
    assert(!writes && !allocations && !gex_dreamcast_title_has_direct(source[0][0], source[0][0]));

    setup_direct_chunk(0);
    put16(GROUP + 24, 0x2002);
    put32(DEFS + 24, IMAGE + 0x100);
    memory[IMAGE + 0x110] = 1;
    put16(IMAGE + 0x114, 1);
    assert(!gex_dreamcast_map_chunk_direct(&io, CHUNK, DEFS, 0, 0, 320, 256));
    assert(!writes && !allocations && !gex_dreamcast_title_has_direct(source[0][0], source[0][0]));

    setup_direct_chunk(0);
    put16(0x4a2a96, 0); put16(0x4a2a94, 0);
    put16(CHUNK + 8, 0); put16(CHUNK + 8 + 7 * 16, 1);
    put16(GROUP + 24, 0);
    assert(gex_dreamcast_map_chunk_direct(&io, CHUNK, DEFS, 0, 16 << 16, 320, 240) == 1);
    assert(!gex_dreamcast_title_has_direct(source[0][0], source[0][0]));
    assert(gex_dreamcast_map_chunk_direct(&io, CHUNK, DEFS, 0, 16 << 16, 320, 256) == 1);
    assert(gex_dreamcast_title_has_direct(source[0][0], source[0][0]));
    gex_dreamcast_title_reset();

    uint32_t mixed_tails[4] = {source[0][0], source[0][1], source[1][0], source[1][1]};
    gex_dreamcast_tile_draw tile = {.width = 2, .height = 2, .page = 0x103,
                                     .page_change = 1, .rgb = 0x64010203};
    assert(gex_dreamcast_title_record_tiles(&io, &tile, 1, mixed_tails) == 1);
    mixed_tails[0] = ARENA; mixed_tails[1] = ARENA + 20;
    tile.rgb = 0x64040506;
    assert(gex_dreamcast_title_record_tiles(&io, &tile, 1, mixed_tails) == 1);
    gex_dreamcast_title_merge_lists(source[0], roots, tails[0]);
    gex_raster_state state = {.left = 0, .top = 8, .right = 320, .bottom = 240};
    gex_gpu_direct_state result = {0};
    tile_capture before = {0}, after = {0};
    assert(!gex_dreamcast_title_has_direct(source[0][0], source[0][0]));
    assert(gex_dreamcast_title_draw_direct_tiles(&before, roots[0], tails[0][0], &state,
                    NULL, capture_tile, &result, error, sizeof error) == 1);
    assert(gex_dreamcast_title_draw_direct_tiles(&after, roots[0], ARENA, &state,
                    NULL, capture_tile, &result, error, sizeof error) == 1);
    assert(before.material.rgb == 0x010203 && after.material.rgb == 0x040506);
    gex_dreamcast_title_reset_lists(roots[0], 0);
    assert(!gex_dreamcast_title_has_direct(roots[0], ARENA));
    assert(gex_dreamcast_title_has_direct(roots[1], ARENA + 20));
    gex_dreamcast_title_forget(ARENA + 20, ARENA + 24);
    assert(!gex_dreamcast_title_has_direct(roots[1], ARENA + 20));
    gex_dreamcast_title_reset();

    uint32_t empty_tails[4] = {source[0][0], source[0][1], source[1][0], source[1][1]};
    tile.layer = 0; tile.rgb = 0x64000001;
    assert(gex_dreamcast_title_record_tiles(&io, &tile, 1, empty_tails) == 1);
    tile.rgb = 0x64000002;
    assert(gex_dreamcast_title_record_tiles(&io, &tile, 1, empty_tails) == 1);
    gex_dreamcast_title_merge_lists(source[0], roots, tails[0]);
    tile.layer = 1; tile.rgb = 0x64000003;
    assert(gex_dreamcast_title_record_tiles(&io, &tile, 1, empty_tails) == 1);
    gex_dreamcast_title_merge_lists(source[1], roots, tails[0]);
    gex_dreamcast_title_merge_lists(source[1], roots, tails[0]);
    tile_capture ordered = {0};
    assert(gex_dreamcast_title_draw_direct_tiles(&ordered, roots[0], tails[0][0], &state,
                    NULL, capture_tile_order, &result, error, sizeof error) == 1);
    assert(ordered.calls == 3 && ordered.q[0].x == 1 && ordered.q[1].x == 2 &&
           ordered.q[2].x == 3);
    gex_dreamcast_title_reset();
}

static void check_direct_root_index(void) {
    const uint32_t source[2] = {0x28000, 0x283a4};
    const uint32_t roots[2] = {0x2a000, 0x2a010};
    const uint32_t tails[2] = {0x2b000, 0x2b010};
    const uint32_t packet_tails[2] = {ARENA, ARENA + 4};
    gex_dreamcast_tile_draw tile = {.width = 2, .height = 2, .page = 0x103,
                                     .page_change = 1, .rgb = 0x64000001};
    gex_raster_state state = {.left = 0, .top = 8, .right = 320, .bottom = 240};
    gex_gpu_direct_state result = {0};
    tile_capture ordered = {0};
    char error[128] = {0};

    assert(gex_dreamcast_title_record_cel_tiles(&tile, 1, source, source) == 1);
    tile.rgb = 0x64000002;
    assert(gex_dreamcast_title_record_cel_tiles(&tile, 1, source, packet_tails) == 1);
    tile.rgb = 0x64000003;
    assert(gex_dreamcast_title_record_cel_tiles(&tile, 1, source, source) == 1);
    gex_dreamcast_title_reset_lists(source[0], 0);
    assert(!gex_dreamcast_title_has_direct(source[0], source[0]));
    assert(gex_dreamcast_title_has_direct(source[1], source[1]));
    assert(gex_dreamcast_title_has_direct(source[1], ARENA + 4));
    tile.rgb = 0x64000004;
    assert(gex_dreamcast_title_record_cel_tiles(&tile, 1, source, source) == 1);
    gex_dreamcast_title_merge_lists(source, roots, tails);
    assert(gex_dreamcast_title_draw_direct_tiles(&ordered, roots[1], tails[1], &state,
                    NULL, capture_tile_order, &result, error, sizeof error) == 1);
    assert(ordered.calls == 3 && ordered.q[0].x == 1 && ordered.q[1].x == 3 &&
           ordered.q[2].x == 4);
    assert(gex_dreamcast_title_has_direct(roots[1], ARENA + 4));
    gex_dreamcast_title_forget(ARENA + 5, ARENA + 6);
    assert(!gex_dreamcast_title_has_direct(roots[1], ARENA + 4));
    gex_dreamcast_title_reset_lists(roots[1], 0);
    assert(gex_dreamcast_title_has_direct(roots[0], tails[0]));
    assert(!gex_dreamcast_title_has_direct(roots[1], tails[1]));
    gex_dreamcast_title_reset();
}

static void check_direct_indexed(void) {
    enum { PALETTE = 0x1c004, SECOND_PALETTE = 0x1c204 };
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL, prepare_palette};
    const uint32_t source[2][2] = {{0x46bc64, 0x46bc60}, {0x46bc58, 0x46bc5c}};
    const uint32_t roots[2] = {0x28000, 0x28010};
    const uint32_t tails[2][2] = {{0x29000, 0x29010}, {0x29020, 0x29030}};
    char error[128] = {0};
    for (unsigned flags = 0; flags <= 0xc000; flags += 0x4000) {
        setup_direct_chunk(flags);
        palette_calls = 0;
        put32(DEFS + 16, PALETTE);
        put16(PALETTE - 2, 0xffff); put16(PALETTE, 0x1234);
        memory[IMAGE + 16] = 0xc0;
        put16(PAGES, 0x003);
        put16(GROUP + 24, (uint16_t)(flags | 0x2002));
        put32(DEFS + 24, IMAGE + 0x100);
        put32(DEFS + 28, SECOND_PALETTE);
        memory[SECOND_PALETTE - 4] = 1;
        put16(SECOND_PALETTE - 2, 0xffff); put16(SECOND_PALETTE, 0x5678);
        memory[IMAGE + 0x110] = 1;
        put16(IMAGE + 0x112, 1); put16(IMAGE + 0x114, 1);
        memory[IMAGE + 0x116] = 24; memory[IMAGE + 0x117] = 20;
        put16(IMAGE + 0x118, 4); put16(IMAGE + 0x11a, 6);
        put16(PAGES + 8, 0x083); put16(PAGES + 10, 0x12e9);
        assert(gex_dreamcast_map_chunk_direct(&io, CHUNK, DEFS, 0, 0, 320, 256) == 1);
        assert(palette_calls == 2 && palette_addresses[0] == PALETTE &&
               palette_addresses[1] == SECOND_PALETTE && !writes && !allocations);
        for (unsigned layer = 0; layer < 2; ++layer) {
            gex_dreamcast_title_merge_lists(source[layer], roots, tails[layer]);
            for (unsigned copy = 0; copy < 2; ++copy) {
                gex_raster_state state = {.left = 0, .top = 8, .right = 320, .bottom = 240};
                gex_gpu_direct_state result = {0};
                tile_capture capture = {0};
                assert(gex_dreamcast_title_draw_direct_tiles(&capture, roots[copy],
                           tails[layer][copy], &state, NULL, capture_tile,
                           &result, error, sizeof error) == 1);
                assert(capture.calls == 1 && capture.material.clut == (layer ? 0x5678 : 0x1234));
                assert(capture.material.page == (layer ? 0x083 : 0x003));
                assert(capture.material.opcode == (flags ? 0x2c : 0x64));
            }
        }
        gex_dreamcast_title_reset();
        if (!flags) {
            put16(PALETTE, 0x4321);
            palette_calls = 0;
            assert(gex_dreamcast_map_chunk_direct(&io, CHUNK, DEFS, 0, 0, 320, 256) == 1);
            gex_dreamcast_title_merge_lists(source[0], roots, tails[0]);
            gex_raster_state state = {.left = 0, .top = 8, .right = 320, .bottom = 240};
            gex_gpu_direct_state result = {0};
            tile_capture capture = {0};
            assert(gex_dreamcast_title_draw_direct_tiles(&capture, roots[0], tails[0][0],
                       &state, NULL, capture_tile, &result, error, sizeof error) == 1);
            assert(capture.material.clut == 0x4321 && palette_calls == 2);
            gex_dreamcast_title_reset();
        }
    }

    setup_direct_chunk(0);
    memory[IMAGE + 16] = 0; put32(DEFS + 16, PALETTE);
    put16(PALETTE - 2, 0xffff); put16(PALETTE, 0x1234);
    put32(0x46bc74, ARENA); put32(0x46bc70, ARENA + 20);
    palette_calls = 0; palette_retire_anchor = 1;
    assert(gex_dreamcast_map_chunk_direct(&io, CHUNK, DEFS, 0, 0, 320, 256) == 1);
    assert(!gex_dreamcast_title_has_direct(source[0][0], ARENA));
    assert(!gex_dreamcast_title_has_direct(source[0][1], ARENA + 20));
    assert(gex_dreamcast_title_has_direct(source[1][0], source[1][0]));
    palette_retire_anchor = 0;
    gex_dreamcast_title_reset();

    setup_direct_chunk(0);
    palette_calls = 0;
    memory[IMAGE + 16] = 0; put32(DEFS + 16, PALETTE);
    put16(PALETTE - 2, 0xffff); put16(PALETTE, 0x1234);
    put16(GROUP + 24, 0x2002); put32(DEFS + 24, IMAGE + 0x100);
    memory[IMAGE + 0x110] = 3; put16(IMAGE + 0x114, 1);
    assert(gex_dreamcast_map_chunk_direct(&io, CHUNK, DEFS, 0, 0, 320, 256) == 0);
    assert(!palette_calls && !gex_dreamcast_title_has_direct(source[0][0], source[0][0]));
    memory[IMAGE + 0x110] = 0;
    put32(DEFS + 28, SECOND_PALETTE);
    put16(SECOND_PALETTE - 2, 384);
    assert(gex_dreamcast_map_chunk_direct(&io, CHUNK, DEFS, 0, 0, 320, 256) == 0);
    assert(!palette_calls && !gex_dreamcast_title_has_direct(source[0][0], source[0][0]));
}

static void check_flipped(void) {
    memset(memory, 0, sizeof memory);
    allocations = writes = 0;
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    put32(0x4a2ae0, ARENA); put32(0x4a2adc, ARENA + 1024);
    put32(0x4a2ae4, ARENA); put32(0x4a2b18, 0x46bc64);
    put32(0x4a2b14, 0x46bc60);
    put16(IMAGE + 20, 36);
    memory[IMAGE + 22] = 4; memory[IMAGE + 23] = 3;
    put16(IMAGE + 24, 2); put16(IMAGE + 26, 3);
    memory[IMAGE + 16] = 1;
    put16(PAGES + 16, 0x123);
    put32(ARENA, 0xdeadbeef);
    put16(ARENA + 30, 0x1234); put16(ARENA + 38, 0x5678);
    gex_gpu_cel_draw draw = {IMAGE, PAGES, 0xc0000000u, 1, 0x112233,
                             100, 200, 5, 7, 0x456};
    uint32_t previous_tail = 0;
    assert(gex_dreamcast_map_flipped_cel(&io, &draw, &previous_tail) == 1);
    assert(previous_tail == 0x46bc60);
    assert(allocations == 1 && get32(0x4a2ae4) == ARENA + 80);
    assert(get32(0x46bc64) == ARENA && get32(0x46bc60) == ARENA + 40);
    assert(get32(ARENA + 4) == 0x2c112233);
    assert(get16(ARENA + 8) == 98 && get16(ARENA + 10) == 197);
    assert(get16(ARENA + 16) == 94 && get16(ARENA + 26) == 194);
    assert(memory[ARENA + 12] == 127 && memory[ARENA + 13] == 0);
    assert(memory[ARENA + 20] == 131 && memory[ARENA + 29] == 2);
    assert(get16(ARENA + 14) == 0x456 && get16(ARENA + 22) == 0x122);
    assert(get16(ARENA + 48) == 93 && get16(ARENA + 50) == 190);
    assert(get32(ARENA + 40) == 0xdeadbeef);
    assert(get16(ARENA + 30) == 0x1234 && get16(ARENA + 70) == 0x1234);
    assert(get16(ARENA + 38) == 0x5678 && get16(ARENA + 78) == 0x5678);
    assert(get16(0x4a2b20) == 0x122);

    put32(0x4a2ae4, ARENA + 80);
    memory[IMAGE + 16] = 0x40;
    put16(PAGES, 0x103);
    memory[PAGES + 2] = 252; memory[PAGES + 3] = 0;
    draw.flags = 0x40000000u; draw.tint = 0x8080u;
    assert(gex_dreamcast_map_flipped_cel(&io, &draw, &previous_tail) == 1);
    assert(previous_tail == ARENA + 40);
    assert(get32(ARENA + 84) == 0x2e112233);
    assert(get16(ARENA + 88) == 102 && get16(ARENA + 90) == 197);
    assert(memory[ARENA + 92] == 252 && memory[ARENA + 100] == 255);
    assert(memory[ARENA + 109] == 2 && get16(ARENA + 102) == 0x123);

    unsigned before_writes = writes, before_allocations = allocations;
    draw.flags = 0;
    assert(gex_dreamcast_map_flipped_cel(&io, &draw, &previous_tail) == 0);
    draw.flags = 0x80000000u;
    put32(0x4a2b18, ARENA + 160);
    assert(gex_dreamcast_map_flipped_cel(&io, &draw, &previous_tail) == 0);
    assert(writes == before_writes && allocations == before_allocations);
    put16(IMAGE + 20, 0);
    assert(gex_dreamcast_map_flipped_cel(&io, &draw, &previous_tail) == 0);
    assert(writes == before_writes && allocations == before_allocations);

    put16(IMAGE + 20, 36);
    put16(IMAGE + 28, 44);
    memory[IMAGE + 30] = 2; memory[IMAGE + 31] = 2;
    put16(PAGES + 8, 0x105);
    put32(0x4a2ae4, ARENA + 1000);
    put32(0x4a2b18, 0x46bc64); put32(0x4a2b14, 0x46bc60);
    assert(gex_dreamcast_map_flipped_cel(&io, &draw, &previous_tail) == 1);
    assert(previous_tail == ARENA + 40);
    assert(get32(0x4a2ae4) == ARENA + 160);
    assert(get32(0x46bc64) == ARENA && get32(ARENA) == ARENA + 80);
    assert(get32(0x46bc60) == ARENA + 40 && get32(ARENA + 40) == ARENA + 120);
    assert(get16(0x4a2b20) == 0x125);
}

static void setup_direct_flipped(unsigned format, unsigned u, unsigned v) {
    memset(memory, 0, sizeof memory);
    allocations = writes = 0;
    put32(0x4a2ae0, ARENA); put32(0x4a2adc, ARENA + 1024);
    put32(0x4a2ae4, ARENA);
    put32(0x4a2b18, 0x28000); put32(0x4a2b14, 0x28010);
    memory[IMAGE + 16] = (uint8_t)(0x40 | format);
    put16(IMAGE + 20, 36);
    memory[IMAGE + 22] = 4; memory[IMAGE + 23] = 3;
    put16(IMAGE + 24, 2); put16(IMAGE + 26, (uint16_t)-3);
    put16(PAGES, 0x103);
    memory[PAGES + 2] = (uint8_t)u; memory[PAGES + 3] = (uint8_t)v;
}

static void check_direct_flipped(void) {
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    const uint32_t roots[2] = {0x28000, 0x28010};
    const unsigned positions[][2] = {{0, 0}, {1, 1}, {252, 253}};
    char error[128] = {0};
    for (unsigned format = 0; format < 3; ++format)
        for (unsigned flags = 1; flags <= 3; ++flags)
            for (unsigned position = 0; position < 3; ++position) {
                uint8_t expected[2][40];
                uint32_t previous;
                gex_gpu_cel_draw draw = {IMAGE, PAGES, flags << 30, 0x8080u,
                                         0x112233, 100, 200, 5, 7, 0x456};
                setup_direct_flipped(format, positions[position][0], positions[position][1]);
                assert(gex_dreamcast_map_flipped_cel(&io, &draw, &previous) == 1);
                memcpy(expected[0], memory + ARENA, 40);
                memcpy(expected[1], memory + ARENA + 40, 40);
                uint16_t final_page = get16(0x4a2b20);

                setup_direct_flipped(format, positions[position][0], positions[position][1]);
                assert(gex_dreamcast_map_cel_direct(&io, &draw, roots) == 1);
                assert(!allocations && writes == 1 &&
                       get32(0x4a2ae4) == ARENA && get16(0x4a2b20) == final_page);
                for (unsigned copy = 0; copy < 2; ++copy) {
                    gex_raster_state state = {.left = 0, .top = 8, .right = 320, .bottom = 240};
                    gex_gpu_direct_state result = {0};
                    tile_capture capture = {0};
                    assert(gex_dreamcast_title_draw_direct_tiles(&capture, roots[copy], roots[copy],
                               &state, NULL, capture_tile, &result, error, sizeof error) == 1);
                    const uint8_t *p = expected[copy];
                    assert(capture.calls == 1 && capture.material.page == final_page &&
                           capture.material.clut == 0x456 && capture.material.opcode == p[7] &&
                           capture.material.rgb == 0x112233 && result.page == final_page &&
                           result.page_valid && !result.page_command_valid);
                    for (unsigned i = 0; i < 4; ++i) {
                        unsigned at = 8 + i * 8;
                        int x = (int16_t)(p[at] | (uint16_t)p[at + 1] << 8);
                        int y = (int16_t)(p[at + 2] | (uint16_t)p[at + 3] << 8);
                        unsigned u = p[at + 4], v = p[at + 5];
                        if ((i == 1 || i == 3) && !u) u = 256;
                        if ((i == 2 || i == 3) && !v) v = 256;
                        assert(capture.q[i].x == x && capture.q[i].y == y - 8 &&
                               capture.q[i].u == u && capture.q[i].v == v);
                    }
                }
                gex_dreamcast_title_reset();
            }
    setup_direct_flipped(3, 0, 0);
    gex_gpu_cel_draw invalid = {IMAGE, PAGES, 0x80000000u, 0, 0, 0, 0, 0, 0, 0};
    assert(!gex_dreamcast_map_cel_direct(&io, &invalid, roots));
    assert(!writes && !allocations && !gex_dreamcast_title_has_direct(roots[0], roots[0]));

    setup_direct_flipped(1, 0, 0);
    invalid.flags = 0xc0000000u;
    put16(IMAGE + 28, 44);
    memory[IMAGE + 30] = 4; memory[IMAGE + 31] = 3;
    put16(PAGES + 8, 0x105);
    memory[PAGES + 10] = 4; memory[PAGES + 11] = 5;
    assert(gex_dreamcast_map_cel_direct(&io, &invalid, roots) == 1);
    assert(get16(0x4a2b20) == 0x125 && writes == 1 && !allocations);
    gex_raster_state state = {.left = 0, .top = 8, .right = 320, .bottom = 240};
    gex_gpu_direct_state result = {0};
    tile_capture captures[2] = {0};
    assert(gex_dreamcast_title_draw_direct_tiles(captures, roots[0], roots[0],
               &state, NULL, capture_tile_sequence, &result, error, sizeof error) == 1);
    assert(captures[0].calls == 2 && captures[0].material.page == 0x122 &&
           captures[1].material.page == 0x125 && result.page == 0x125);
    gex_dreamcast_title_reset();

    setup_direct_flipped(1, 0, 0);
    put16(IMAGE + 28, 44);
    memory[IMAGE + 30] = 0; memory[IMAGE + 31] = 3;
    assert(!gex_dreamcast_map_cel_direct(&io, &invalid, roots));
    assert(!writes && !allocations && !gex_dreamcast_title_has_direct(roots[0], roots[0]));
}

static void check_direct_unflipped(void) {
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    const uint32_t roots[2] = {0x28000, 0x28010};
    char error[128] = {0};
    for (unsigned format = 0; format < 3; ++format) {
        uint8_t expected[2][20];
        gex_gpu_cel_result legacy;
        gex_gpu_cel_draw draw = {IMAGE, PAGES, 0, 0x8080u, 0x112233,
                                 100, 200, 5, 7, 0x456};
        setup_direct_flipped(format, 252, 253);
        assert(gex_dreamcast_map_unflipped_cel(&io, &draw, &legacy) == 1);
        memcpy(expected[0], memory + ARENA, 20);
        memcpy(expected[1], memory + ARENA + 20, 20);
        uint16_t final_page = get16(0x4a2b20);

        setup_direct_flipped(format, 252, 253);
        assert(gex_dreamcast_map_cel_direct(&io, &draw, roots) == 1);
        assert(!allocations && writes == 1 && get32(0x4a2ae4) == ARENA &&
               get16(0x4a2b20) == final_page);
        for (unsigned copy = 0; copy < 2; ++copy) {
            gex_raster_state state = {.left = 0, .top = 8, .right = 320, .bottom = 240};
            gex_gpu_direct_state result = {0};
            tile_capture capture = {0};
            assert(gex_dreamcast_title_draw_direct_tiles(&capture, roots[copy], roots[copy],
                       &state, NULL, capture_tile, &result, error, sizeof error) == 1);
            const uint8_t *p = expected[copy];
            assert(capture.calls == 1 && capture.material.page == (final_page & 0x19f) &&
                   capture.material.clut == 0x456 && capture.material.opcode == p[7] &&
                   capture.material.rgb == 0x112233 && result.page_valid &&
                   result.page_command_valid && result.page_command == (final_page & 0x1ff));
            int x = (int16_t)(p[8] | (uint16_t)p[9] << 8);
            int y = (int16_t)(p[10] | (uint16_t)p[11] << 8);
            for (unsigned i = 0; i < 4; ++i)
                assert(capture.q[i].x == x + ((i & 1) ? 4 : 0) &&
                       capture.q[i].y == y - 8 + ((i & 2) ? 3 : 0) &&
                       capture.q[i].u == p[12] + ((i & 1) ? 4 : 0) &&
                       capture.q[i].v == p[13] + ((i & 2) ? 3 : 0));
        }
        gex_dreamcast_title_reset();
    }

    setup_direct_flipped(1, 0, 0);
    gex_gpu_cel_draw draw = {IMAGE, PAGES, 0, 0, 0, 0, 0, 0, 0, 0};
    put16(0x4a2b20, 0x123);
    assert(gex_dreamcast_map_cel_direct(&io, &draw, roots) == 1);
    for (unsigned copy = 0; copy < 2; ++copy) {
        gex_raster_state inherited = {.page = copy ? 0x081 : 0x102,
                                      .left = 0, .top = 8, .right = 320, .bottom = 240};
        gex_gpu_direct_state inherited_result = {0};
        tile_capture capture = {0};
        assert(gex_dreamcast_title_draw_direct_tiles(&capture, roots[copy], roots[copy],
                   &inherited, NULL, capture_tile, &inherited_result, error, sizeof error) == 1);
        assert(capture.calls == 1 && capture.material.page == inherited.page &&
               !inherited_result.page_valid && !inherited_result.page_command_valid);
    }
    gex_dreamcast_title_reset();

    setup_direct_flipped(1, 0, 0);
    put16(0x4a2b20, 0);
    put16(IMAGE + 28, 44);
    memory[IMAGE + 30] = 4; memory[IMAGE + 31] = 3;
    put16(PAGES + 8, 0x103);
    assert(gex_dreamcast_map_cel_direct(&io, &draw, roots) == 1);
    gex_raster_state state = {.left = 0, .top = 8, .right = 320, .bottom = 240};
    gex_gpu_direct_state result = {0};
    tile_capture captures[2] = {0};
    assert(gex_dreamcast_title_draw_direct_tiles(captures, roots[0], roots[0], &state,
               NULL, capture_tile_sequence, &result, error, sizeof error) == 1);
    assert(captures[0].calls == 2 && captures[0].material.page == 0x103 &&
           captures[1].material.page == 0x103 && result.page_command_valid &&
           result.page_command == 0x123 && get16(0x4a2b20) == 0x123);
    gex_dreamcast_title_reset();

    setup_direct_flipped(1, 0, 0);
    put16(0x4a2b20, 0x123);
    put16(IMAGE + 28, 44);
    memory[IMAGE + 30] = 4; memory[IMAGE + 31] = 3;
    put16(PAGES + 8, 0x105);
    assert(gex_dreamcast_map_cel_direct(&io, &draw, roots) == 1);
    state.page = 0x081;
    result = (gex_gpu_direct_state){0};
    memset(captures, 0, sizeof captures);
    assert(gex_dreamcast_title_draw_direct_tiles(captures, roots[0], roots[0], &state,
               NULL, capture_tile_sequence, &result, error, sizeof error) == 1);
    assert(captures[0].calls == 2 && captures[0].material.page == 0x081 &&
           captures[1].material.page == 0x105 && result.page_valid &&
           result.page == 0x125 && result.page_command_valid &&
           result.page_command == 0x125);
    gex_dreamcast_title_reset();
}

static void check_fixed(void) {
    memset(memory, 0, sizeof memory);
    allocations = writes = 0;
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    put32(0x4a2ae0, ARENA); put32(0x4a2adc, ARENA + 1024);
    put32(0x4a2ae4, ARENA); put32(0x4a2b18, 0x46bc64);
    put32(0x4a2b14, 0x46bc60); put16(0x4a2b20, 0xffff);
    memory[IMAGE + 16] = 0x40;
    put16(IMAGE + 20, 36);
    memory[IMAGE + 22] = 4; memory[IMAGE + 23] = 3;
    put16(IMAGE + 24, 2); put16(IMAGE + 26, 3);
    put16(PAGES, 0x103); put16(PAGES + 2, 0x1234);
    put32(ARENA, 0xdeadbeef); put32(ARENA + 48, 0xabcdef01);
    gex_gpu_cel_draw draw = {IMAGE, PAGES, 0, 0, 0x112233,
                             100, 200, 5, 7, 0x456};
    gex_gpu_cel_result result;
    assert(gex_dreamcast_map_unflipped_cel(&io, &draw, &result) == 1);
    assert(allocations == 2 && get32(0x4a2ae4) == ARENA + 64);
    assert(get32(0x46bc64) == ARENA + 40 && get32(0x46bc60) == ARENA + 52);
    assert(get32(ARENA + 40) == ARENA && get32(ARENA + 52) == ARENA + 20);
    assert(get32(ARENA + 44) == 0xe1000323 && get32(ARENA + 56) == 0xe1000323);
    assert(get32(ARENA + 48) == 0xabcdef01 && get32(ARENA + 60) == 0xabcdef01);
    assert(get32(ARENA + 20) == 0xdeadbeef && get32(ARENA + 4) == 0x64112233);
    assert(get16(ARENA + 8) == 102 && get16(ARENA + 10) == 203);
    assert(get16(ARENA + 28) == 97 && get16(ARENA + 30) == 196);
    assert(get16(ARENA + 12) == 0x1234 && get16(ARENA + 14) == 0x456);
    assert(result.eax == ARENA + 20 && result.ecx == ARENA + 52 &&
           result.edx == 0x46bc60);
    assert(get16(0x4a2b20) == 0x123);

    assert(gex_dreamcast_map_unflipped_cel(&io, &draw, &result) == 1);
    assert(allocations == 3 && get32(0x4a2ae4) == ARENA + 104);
    assert(get32(ARENA) == ARENA + 64 && get32(ARENA + 20) == ARENA + 84);
    assert(result.eax == ARENA && result.ecx == ARENA + 20 && result.edx == ARENA + 84);
    unsigned before_writes = writes, before_allocations = allocations;
    draw.flags = 0x80000000u;
    assert(gex_dreamcast_map_unflipped_cel(&io, &draw, &result) == 0);
    draw.flags = 0; memory[IMAGE + 16] = 0;
    put16(PAGES + 16, 0x103); put16(PAGES + 18, 0x1234);
    assert(gex_dreamcast_map_unflipped_cel(&io, &draw, &result) == 1);
    assert(get32(0x4a2ae4) == ARENA + 144);
    assert(get16(ARENA + 116) == 0x1234);
    before_writes = writes; before_allocations = allocations;
    put16(IMAGE + 28, 44);
    memory[IMAGE + 30] = 2; memory[IMAGE + 31] = 2;
    assert(gex_dreamcast_map_unflipped_cel(&io, &draw, &result) == 0);
    assert(writes == before_writes && allocations == before_allocations);

    memory[IMAGE + 16] = 0x40;
    put16(PAGES + 8, 0x103); put16(PAGES + 10, 0x5678);
    draw.tint = 1;
    put16(0x4a2b20, 0x103);
    put32(0x4a2b18, 0x46bc64); put32(0x4a2b14, 0x46bc60);
    put32(0x4a2adc, ARENA + 160); put32(0x4a2ae4, ARENA + 120);
    assert(gex_dreamcast_map_unflipped_cel(&io, &draw, &result) == 1);
    assert(get32(0x4a2ae4) == ARENA + 40);
    assert(get32(0x46bc64) == ARENA + 120 && get32(ARENA + 120) == ARENA);
    assert(get32(0x46bc60) == ARENA + 140 && get32(ARENA + 140) == ARENA + 20);
    assert(get16(ARENA + 12) == 0x5678);

    put32(0x4a2adc, ARENA + 72); put32(0x4a2ae4, ARENA);
    put32(0x4a2b18, 0x46bc64); put32(0x4a2b14, 0x46bc60);
    before_writes = writes; before_allocations = allocations;
    assert(gex_dreamcast_map_unflipped_cel(&io, &draw, &result) == 0);
    assert(writes == before_writes && allocations == before_allocations);
}

static void check_scaled(void) {
    memset(memory, 0, sizeof memory);
    allocations = writes = 0;
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    put32(0x4a2ae0, ARENA); put32(0x4a2adc, ARENA + 1024);
    put32(0x4a2ae4, ARENA); put32(0x4a2b18, 0x46bc64);
    put32(0x4a2b14, 0x46bc60);
    memory[IMAGE + 16] = 0x40;
    put16(IMAGE + 20, 36);
    memory[IMAGE + 22] = 4; memory[IMAGE + 23] = 3;
    put16(IMAGE + 24, (uint16_t)-2); put16(IMAGE + 26, 3);
    put16(PAGES, 0x103); put16(PAGES + 2, 0x1234);
    put32(ARENA, 0xdeadbeef); put16(ARENA + 30, 0xabcd);
    const gex_gpu_scaled_cel_draw draw = {IMAGE, IMAGE + 20, PAGES, 0, 0x2c112233,
                                          10, -12, 4, 3, 100, 200, 5, 7, 0x456};
    uint32_t last_atlas = 0;
    assert(gex_dreamcast_map_scaled_cel(&io, &draw, &last_atlas) == 1);
    assert(last_atlas == PAGES + 8 && allocations == 1);
    assert(get32(0x4a2ae4) == ARENA + 80 && get32(0x46bc64) == ARENA);
    assert(get32(0x46bc60) == ARENA + 40 && get32(ARENA + 40) == 0xdeadbeef);
    assert(get32(ARENA + 4) == 0x2c112233 && get16(ARENA + 14) == 0x456);
    assert(get16(ARENA + 8) == 95 && get16(ARENA + 16) == 105);
    assert(get16(ARENA + 10) == 188 && get16(ARENA + 26) == 176);
    assert(get16(ARENA + 48) == 90 && get16(ARENA + 50) == 181);
    assert(get16(ARENA + 12) == 0x1234 && get16(ARENA + 20) == 0x1237);
    assert(memory[ARENA + 29] == 0x14 && get16(ARENA + 22) == 0x123);
    assert(get16(ARENA + 30) == 0xabcd && get16(ARENA + 70) == 0xabcd);
    assert(get16(0x4a2b20) == 0x123);
    unsigned before_writes = writes, before_allocations = allocations;
    gex_gpu_scaled_cel_draw invalid = draw;
    invalid.divisor_x = 0;
    assert(gex_dreamcast_map_scaled_cel(&io, &invalid, &last_atlas) == 0);
    assert(writes == before_writes && allocations == before_allocations);

    memory[IMAGE + 16] = 1;
    put16(IMAGE + 28, 44);
    memory[IMAGE + 30] = 2; memory[IMAGE + 31] = 2;
    put32(PAGES + 4, PAGES + 32);
    put16(PAGES + 16, 0x104); put16(PAGES + 18, 0x0605);
    put16(PAGES + 48, 0x105); put16(PAGES + 50, 0x0807);
    put32(0x4a2ae4, ARENA + 1000);
    put32(0x4a2b18, 0x46bc64); put32(0x4a2b14, 0x46bc60);
    assert(gex_dreamcast_map_scaled_cel(&io, &draw, &last_atlas) == 1);
    assert(last_atlas == PAGES + 32 && get32(0x4a2ae4) == ARENA + 160);
    assert(get32(ARENA) == ARENA + 80 && get32(ARENA + 40) == ARENA + 120);
    assert(get16(ARENA + 22) == 0x124 && get16(ARENA + 102) == 0x125);
    assert(get16(ARENA + 92) == 0x0807 && get16(ARENA + 100) == 0x0808);
}

static void setup_direct_scaled(int linked, unsigned uv) {
    setup_direct_flipped(1, uv, uv);
    memory[IMAGE + 16] = linked ? 1 : 0x41;
    put16(IMAGE + 24, (uint16_t)-2);
    put16(IMAGE + 26, 3);
    put16(IMAGE + 28, 44);
    memory[IMAGE + 30] = 2; memory[IMAGE + 31] = 2;
    put16(IMAGE + 32, 5); put16(IMAGE + 34, (uint16_t)-4);
    if (linked) {
        put32(PAGES + 4, PAGES + 32);
        put16(PAGES + 16, 0x104); put16(PAGES + 18, (uint16_t)(uv * 0x101));
        put16(PAGES + 48, 0x105); put16(PAGES + 50, 0xfffe);
    } else {
        put16(PAGES + 8, 0x105); put16(PAGES + 10, 0xfffe);
    }
}

static void check_direct_scaled(void) {
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    const uint32_t roots[2] = {0x28000, 0x28010};
    const unsigned positions[] = {0, 252, 253};
    char error[128] = {0};
    for (unsigned linked = 0; linked < 2; ++linked)
        for (unsigned pos = 0; pos < 3; ++pos) {
            uint8_t expected[2][2][40];
            uint32_t legacy_last = 0, direct_last = 0;
            gex_gpu_scaled_cel_draw draw = {IMAGE, IMAGE + 20, PAGES, 0, 0x2e112233,
                                             10, -12, -4, 3, 100, 200, 5, 7, 0x456};
            setup_direct_scaled(linked, positions[pos]);
            assert(gex_dreamcast_map_scaled_cel(&io, &draw, &legacy_last) == 1);
            for (unsigned part = 0; part < 2; ++part)
                for (unsigned copy = 0; copy < 2; ++copy)
                    memcpy(expected[part][copy], memory + ARENA + part * 80 + copy * 40, 40);
            uint16_t final_page = get16(0x4a2b20);

            setup_direct_scaled(linked, positions[pos]);
            assert(gex_dreamcast_map_scaled_cel_direct(&io, &draw, roots, &direct_last) == 1);
            assert(direct_last == legacy_last && !allocations && writes == 1 &&
                   get32(0x4a2ae4) == ARENA && get16(0x4a2b20) == final_page);
            for (unsigned copy = 0; copy < 2; ++copy) {
                gex_raster_state state = {.left = 0, .top = 8, .right = 320, .bottom = 240};
                gex_gpu_direct_state result = {0};
                tile_capture captures[2] = {0};
                assert(gex_dreamcast_title_draw_direct_tiles(captures, roots[copy], roots[copy],
                           &state, NULL, capture_tile_sequence, &result, error, sizeof error) == 1);
                assert(captures[0].calls == 2 && result.page_valid && result.page == final_page &&
                       !result.page_command_valid);
                for (unsigned part = 0; part < 2; ++part) {
                    const uint8_t *p = expected[part][copy];
                    tile_capture *capture = &captures[part];
                    assert(capture->material.page == (p[22] | (unsigned)p[23] << 8) &&
                           capture->material.clut == 0x456 && capture->material.opcode == p[7] &&
                           capture->material.rgb == 0x112233);
                    for (unsigned i = 0; i < 4; ++i) {
                        unsigned at = 8 + i * 8;
                        int x = (int16_t)(p[at] | (uint16_t)p[at + 1] << 8);
                        int y = (int16_t)(p[at + 2] | (uint16_t)p[at + 3] << 8);
                        unsigned u = p[at + 4], v = p[at + 5];
                        if ((i & 1) && !u) u = 256;
                        if ((i & 2) && !v) v = 256;
                        assert(capture->q[i].x == x && capture->q[i].y == y - 8 &&
                               capture->q[i].u == u && capture->q[i].v == v);
                    }
                }
            }
            gex_dreamcast_title_reset();
        }
    setup_direct_scaled(0, 0);
    gex_gpu_scaled_cel_draw invalid = {IMAGE, IMAGE + 20, PAGES, 0, 0x2c112233,
                                       10, -12, 4, 3, 100, 200, 5, 7, 0x456};
    uint32_t last = 0xdeadbeef;
    invalid.divisor_x = 0;
    assert(!gex_dreamcast_map_scaled_cel_direct(&io, &invalid, roots, &last));
    invalid.divisor_x = -1;
    invalid.span_x = 0x40000000;
    assert(!gex_dreamcast_map_scaled_cel_direct(&io, &invalid, roots, &last));
    invalid.divisor_x = 4;
    invalid.span_x = 10;
    memory[IMAGE + 30] = 0;
    assert(!gex_dreamcast_map_scaled_cel_direct(&io, &invalid, roots, &last));
    memory[IMAGE + 30] = 2;
    invalid.rgb = 0x64112233;
    assert(!gex_dreamcast_map_scaled_cel_direct(&io, &invalid, roots, &last));
    assert(!writes && !allocations && last == 0xdeadbeef &&
           !gex_dreamcast_title_has_direct(roots[0], roots[0]));
}

int main(void) {
    check_direct_world_backdrop();
    check_direct_chunk();
    check_direct_root_index();
    check_direct_indexed();
    memset(memory, 0, sizeof memory);
    allocations = writes = 0;
    const gex_gpu_guest_io io = {NULL, read_guest, write_guest, allocated, NULL};
    put32(0x4a2ae8, TABLE);
    put32(0x460e08, PAGES);
    put32(0x4a2ae0, ARENA);
    put32(0x4a2adc, ARENA + 1024);
    put32(0x4a2ae4, ARENA);
    put32(0x46bc74, 0x46bc64);
    put32(0x46bc70, 0x46bc60);
    put32(0x46bc6c, 0x46bc58);
    put32(0x46bc68, 0x46bc5c);
    memory[0x4a2af8] = 0x11; memory[0x4a2af9] = 0x22; memory[0x4a2afa] = 0x33;
    put16(0x4a2a96, 2); put16(0x4a2a94, (uint16_t)-3);
    put32(TABLE, GROUP);
    put16(CHUNK + 8, 1);
    put16(GROUP + 16, 1);
    put32(DEFS + 12, IMAGE);
    memory[IMAGE + 16] = 2;
    put16(IMAGE + 20, 1);
    memory[IMAGE + 22] = 24; memory[IMAGE + 23] = 20;
    put16(IMAGE + 24, 4); put16(IMAGE + 26, 6);
    put16(PAGES, 0x103); put16(PAGES + 2, 0x1234);
    put32(ARENA, 0xdeadbeef);
    put32(ARENA + 48, 0xabcdef01);
    assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 1);
    assert(allocations == 1 && get32(0x4a2ae4) == ARENA + 64);
    assert(get32(0x46bc64) == ARENA + 40 && get32(0x46bc60) == ARENA + 52);
    assert(get32(ARENA + 40) == ARENA && get32(ARENA + 52) == ARENA + 20);
    assert(get32(ARENA) == 0xdeadbeef && get32(ARENA + 20) == 0xdeadbeef);
    assert(get32(ARENA + 48) == 0xabcdef01 && get32(ARENA + 60) == 0xabcdef01);
    assert(get32(ARENA + 44) == 0xe1000303 && get32(ARENA + 56) == 0xe1000303);
    assert(get32(ARENA + 4) == 0x64112233 && get32(ARENA + 24) == 0x64112233);
    assert(get32(ARENA + 8) == 0x00060004 && get32(ARENA + 28) == 0x00030006);
    assert(get32(ARENA + 12) == 0x1234 && get32(ARENA + 16) == 0x00140018);
    assert(get32(0x46bc74) == ARENA && get32(0x46bc70) == ARENA + 20);

    memory[IMAGE + 16] = 0;
    unsigned before_writes = writes, before_allocations = allocations;
    assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 0);
    assert(writes == before_writes && allocations == before_allocations);
    memory[IMAGE + 16] = 2;
    assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, INT32_MIN, 0, 320, 240) == 0);
    put16(0x4a2a96, 257);
    assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 0);
    put16(0x4a2a96, 2);
    assert(writes == before_writes && allocations == before_allocations);

    put16(GROUP + 24, 0x2001);
    put32(DEFS + 20, 0x8080);
    put32(0x4a2ae4, ARENA + 1016);
    put32(0x46bc74, 0x46bc64); put32(0x46bc70, 0x46bc60);
    put32(0x46bc6c, 0x46bc58); put32(0x46bc68, 0x46bc5c);
    assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 1);
    assert(get32(0x4a2ae4) == ARENA + 128);
    assert(get32(0x46bc64) == ARENA + 40 && get32(0x46bc60) == ARENA + 52);
    assert(get32(0x46bc58) == ARENA + 104 && get32(0x46bc5c) == ARENA + 116);
    assert(get32(ARENA + 4) == 0x66112233 && get32(ARENA + 68) == 0x66112233);

    put16(GROUP + 24, 0);
    put32(0x4a2ae4, ARENA);
    put32(0x46bc74, ARENA + 8);
    before_writes = writes; before_allocations = allocations;
    assert(gex_dreamcast_map_chunk(&io, CHUNK, DEFS, 0, 0, 320, 240) == 0);
    assert(writes == before_writes && allocations == before_allocations);
    check_chunk_retirement();
    check_chunk_snapshot_retirement();
    check_chunk_flips();
    check_flipped();
    check_direct_flipped();
    check_direct_unflipped();
    check_fixed();
    check_scaled();
    check_direct_scaled();
    return 0;
}
