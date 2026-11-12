#include "platform/dreamcast/map_plan.h"
#undef NDEBUG
#include <assert.h>
#include <string.h>

enum { CHUNK = 0x100, TABLE = 0x200, GROUP = 0x400,
       DEFINITIONS = 0x1000, IMAGE = 0x2000, MEMORY_SIZE = 0x4000 };
static uint8_t memory[MEMORY_SIZE];

static void put16(unsigned at, uint16_t value) {
    memory[at] = (uint8_t)value;
    memory[at + 1] = (uint8_t)(value >> 8);
}

static void put32(unsigned at, uint32_t value) {
    put16(at, (uint16_t)value);
    put16(at + 2, (uint16_t)(value >> 16));
}

static int read_guest(void *context, uint32_t address, void *out, size_t size) {
    (void)context;
    if (address > MEMORY_SIZE || size > MEMORY_SIZE - address) return -1;
    memcpy(out, memory + address, size);
    return 0;
}

static size_t plan(int32_t x, int32_t y, int16_t screen_x, int16_t screen_y,
                   gex_map_tile *tiles) {
    size_t count = 0;
    assert(!gex_dreamcast_map_plan(read_guest, NULL, CHUNK, TABLE, DEFINITIONS,
                                   x, y, screen_x, screen_y, 320, 240, tiles, &count));
    return count;
}

static int floor32(int64_t value) {
    const int64_t unit = 0x200000;
    return value >= 0 ? (int)(value / unit) : (int)(-((-value + unit - 1) / unit));
}

static void check_clipping(void) {
    memset(memory, 0, sizeof memory);
    put32(TABLE, GROUP);
    for (unsigned i = 0; i < 64; ++i) put16(CHUNK + 8 + i * 2, 1);
    put16(GROUP + 16, 1);
    put32(DEFINITIONS + 12, IMAGE);
    put16(IMAGE + 20, 1);
    uint32_t random = 1;
    for (unsigned trial = 0; trial < 4000; ++trial) {
        int32_t position[2], initial[2];
        int16_t screen[2];
        int extent[2] = {320, 240}, start[2] = {0, 0}, count[2] = {8, 8};
        for (unsigned axis = 0; axis < 2; ++axis) {
            random = random * 1664525u + 1013904223u;
            position[axis] = ((int32_t)(random % 500) - 200) * 65536 +
                             (int32_t)((random >> 8) & 0xffff);
            initial[axis] = position[axis];
            random = random * 1664525u + 1013904223u;
            screen[axis] = (int16_t)((int)(random % 17) - 8);
            int margin = (screen[axis] < 0 ? -screen[axis] : screen[axis]) * 131072;
            if (position[axis] >= margin + extent[axis] * 65536) {
                count[axis] = 0;
                continue;
            }
            if (position[axis] < -margin) {
                int skipped = -floor32((int64_t)margin + position[axis] + 0x1f0000);
                start[axis] += skipped;
                count[axis] -= skipped;
                position[axis] += skipped * 0x200000;
            }
            int edge = margin + (extent[axis] - 256) * 65536;
            if (position[axis] >= edge)
                count[axis] -= floor32((int64_t)position[axis] - edge);
        }
        gex_map_tile tiles[GEX_MAP_TILE_LIMIT];
        size_t actual = plan(initial[0], initial[1], screen[0], screen[1], tiles);
        size_t expected = (size_t)count[0] * count[1];
        assert(actual == expected);
        for (size_t i = 0; i < actual; ++i) {
            int col = start[0] + (int)(i % count[0]);
            int row = start[1] + (int)(i / count[0]);
            assert(tiles[i].x == initial[0] + col * 0x200000);
            assert(tiles[i].y == initial[1] + row * 0x200000);
        }
    }
}

int main(void) {
    gex_map_tile tiles[GEX_MAP_TILE_LIMIT];
    put32(TABLE, GROUP);
    put16(CHUNK + 8, 1);
    put16(CHUNK + 10, 2);
    put16(CHUNK + 24, 1);
    put16(GROUP + 16, 1);
    put16(GROUP + 24, 2);
    put16(GROUP + 32, 3);
    for (unsigned i = 1; i <= 3; ++i) {
        put32(DEFINITIONS + i * 12, IMAGE + i * 32);
        put16(IMAGE + i * 32 + 20, 1);
    }

    assert(plan(0, 0, 0, 0, tiles) == 5);
    assert(tiles[0].spec == 1 && tiles[1].spec == 2 && tiles[2].spec == 3);
    assert(tiles[2].x == 0x200000 && tiles[2].y == 0);
    assert(tiles[3].spec == 1 && tiles[3].y == 0x200000);
    assert(tiles[0].descriptor == DEFINITIONS + 12 && tiles[0].image == IMAGE + 32);

    assert(plan(-31 * 65536, 0, 0, 0, tiles) == 5);
    assert(plan(-31 * 65536 - 1, 0, 0, 0, tiles) == 1);
    assert(tiles[0].spec == 3);
    assert(plan(-32 * 65536, 0, 1, 0, tiles) == 5);
    assert(plan(320 * 65536, 0, 0, 0, tiles) == 0);
    assert(plan(0, 240 * 65536, 0, 0, tiles) == 0);

    put32(IMAGE + 32 + 8, 0x80000000u);
    put16(IMAGE + 64 + 20, 0);
    assert(plan(0, 0, 0, 0, tiles) == 1 && tiles[0].spec == 3);
    put32(TABLE, MEMORY_SIZE - 1);
    size_t count;
    assert(gex_dreamcast_map_plan(read_guest, NULL, CHUNK, TABLE, DEFINITIONS,
                                  0, 0, 0, 0, 320, 240, tiles, &count) == -1);
    check_clipping();
    return 0;
}
