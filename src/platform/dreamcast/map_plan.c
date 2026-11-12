#include "map_plan.h"
#include <limits.h>

static int read16(gex_title_read read, void *context, uint32_t address, uint16_t *value) {
    uint8_t bytes[2];
    if (address > UINT32_MAX - 1 || read(context, address, bytes, sizeof bytes)) return -1;
    *value = bytes[0] | (uint16_t)bytes[1] << 8;
    return 0;
}

static int read32(gex_title_read read, void *context, uint32_t address, uint32_t *value) {
    uint8_t bytes[4];
    if (address > UINT32_MAX - 3 || read(context, address, bytes, sizeof bytes)) return -1;
    *value = bytes[0] | (uint32_t)bytes[1] << 8 |
             (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return 0;
}

int gex_dreamcast_map_plan(gex_title_read read, void *context,
                           uint32_t chunk, uint32_t table, uint32_t definitions,
                           int32_t x, int32_t y, int16_t screen_x, int16_t screen_y,
                           int width, int height, gex_map_tile tiles[GEX_MAP_TILE_LIMIT],
                           size_t *count) {
    if (!read || !chunk || !table || !definitions || !tiles || !count ||
        chunk > UINT32_MAX - 135 || width <= 0 || width > 32767 ||
        height <= 0 || height > 32767) return -1;
    *count = 0;
    int64_t margin_x = (screen_x < 0 ? -(int64_t)screen_x : screen_x) * 131072;
    int64_t margin_y = (screen_y < 0 ? -(int64_t)screen_y : screen_y) * 131072;
    for (unsigned row = 0; row < 8; ++row) {
        int64_t py = (int64_t)y + row * 0x200000;
        if (py + 0x1f0000 < -margin_y || py >= ((int64_t)height << 16) + margin_y)
            continue;
        for (unsigned col = 0; col < 8; ++col) {
            int64_t px = (int64_t)x + col * 0x200000;
            if (px + 0x1f0000 < -margin_x || px >= ((int64_t)width << 16) + margin_x)
                continue;
            if (px < INT32_MIN || px > INT32_MAX || py < INT32_MIN || py > INT32_MAX)
                return -1;
            uint16_t cell;
            if (read16(read, context, chunk + 8 + (row * 8 + col) * 2, &cell))
                return -1;
            if (!cell) continue;
            uint32_t group;
            uint32_t group_offset = ((uint32_t)cell >> 3) & 0xfffc;
            if (table > UINT32_MAX - group_offset ||
                read32(read, context, table + group_offset, &group) || !group)
                return -1;
            uint32_t cell_offset = (cell & 31u) * 16;
            if (group > UINT32_MAX - cell_offset - 9) return -1;
            for (unsigned layer = 0; layer < 2; ++layer) {
                uint16_t spec, first;
                uint32_t image, origin;
                if (read16(read, context, group + cell_offset + layer * 8, &spec))
                    return -1;
                uint32_t offset = (spec & 0xfffu) * 12;
                if (definitions > UINT32_MAX - offset - 11) return -1;
                uint32_t descriptor = definitions + offset;
                if (read32(read, context, descriptor, &image)) return -1;
                if (!image) continue;
                if (image > UINT32_MAX - 21 ||
                    read16(read, context, image + 20, &first) ||
                    read32(read, context, image + 8, &origin))
                    return -1;
                if (!first || (origin & 0x80000000u)) continue;
                tiles[(*count)++] = (gex_map_tile){descriptor, image, spec,
                                                  (int32_t)px, (int32_t)py};
            }
        }
    }
    return 0;
}
