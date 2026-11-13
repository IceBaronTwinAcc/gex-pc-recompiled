#include "platform/dreamcast/map_draw.h"
#include "platform/dreamcast/title_draw.h"
#undef NDEBUG
#include <assert.h>
#include <string.h>

enum { SIZE = 0x4b0000, CHUNK = 0x10000, TABLE = 0x11000, GROUP = 0x12000,
       DEFS = 0x13000, IMAGE = 0x14000, PAGES = 0x15000, ARENA = 0x30000 };
static uint8_t memory[SIZE];
static unsigned allocations, writes;
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

int main(void) {
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
    check_fixed();
    check_scaled();
    return 0;
}
