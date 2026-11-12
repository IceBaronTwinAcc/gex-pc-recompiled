#include "title_plan.h"

uint32_t gex_title_packet_rgb(uint32_t tint, uint32_t fade, const uint8_t fade_rgb[3],
                              const uint32_t colors[32]) {
    if (tint & 0x8000u)
        return (tint & 0xc0u) == 0xc0u ? fade : (fade & 0x00fefefeu) >> 1;
    uint32_t color = colors[(tint >> 8) & 31u];
    if (fade == 0x00808080u) return color;
    unsigned intensity = color & 255u;
    return (((fade_rgb[0] * intensity & 0x7f80u) >> 7) |
            ((fade_rgb[1] * intensity & 0x7f80u) << 1) |
            ((fade_rgb[2] * intensity & 0x7f80u) << 9));
}

static int read32(gex_title_read read, void *context, uint32_t base,
                  unsigned offset, uint32_t *value) {
    uint8_t bytes[4];
    if (base > UINT32_MAX - offset || read(context, base + offset, bytes, sizeof bytes))
        return -1;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
             (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return 0;
}

int gex_title_plan(gex_title_read read, void *context, uint32_t object, uint32_t cel,
                   uint32_t camera_x, uint32_t camera_y, uint32_t current_frame,
                   uint16_t screen_x, uint16_t screen_y, int view_width, int view_height,
                   gex_title_part *parts, size_t capacity, size_t *count) {
    if (!read || !object || !count || (capacity && !parts) ||
        view_width <= 0 || view_width > 32767 || view_height <= 0 || view_height > 32767)
        return GEX_TITLE_PLAN_INVALID;
    *count = 0;
    if (!cel) return GEX_TITLE_PLAN_OK;
    uint32_t list, tint, palette;
    gex_title_layout_input layout = {0};
    gex_title_wrap_input wrap = {0};
    if (read32(read, context, cel, 0x18, &list) || !list ||
        read32(read, context, object, 0x78, &layout.object_x) ||
        read32(read, context, object, 0x7c, &layout.object_y) ||
        read32(read, context, object, 0x6c, &layout.object_flags) ||
        read32(read, context, object, 0xbc, &tint) ||
        read32(read, context, object, 0xc0, &palette) ||
        read32(read, context, object, 0xe0, &wrap.object_flags) ||
        read32(read, context, object, 0x1f4, &wrap.object_frame) ||
        read32(read, context, object, 0x1f8, &wrap.previous_x) ||
        read32(read, context, object, 0x1fc, &wrap.previous_y))
        return GEX_TITLE_PLAN_INVALID;
    layout.camera_x = camera_x;
    layout.camera_y = camera_y;
    layout.view_width = view_width;
    layout.view_height = view_height;
    wrap.x = layout.object_x; wrap.y = layout.object_y;
    wrap.current_frame = current_frame;
    wrap.screen_x = screen_x; wrap.screen_y = screen_y;
    int wrap_x, wrap_y;
    gex_title_layout_wrap(&wrap, &wrap_x, &wrap_y);

    size_t written = 0;
    for (unsigned i = 0; i <= 512; ++i) {
        uint32_t part, image, first_tile, part_tint, part_palette;
        if (read32(read, context, list, i * 4, &part)) return GEX_TITLE_PLAN_INVALID;
        if (!part) {
            *count = written;
            return GEX_TITLE_PLAN_OK;
        }
        if (i == 512) return GEX_TITLE_PLAN_CAPACITY;
        if (read32(read, context, part, 0, &layout.part_position) ||
            read32(read, context, part, 4, &layout.part_flags) ||
            read32(read, context, part, 8, &image) || !image ||
            read32(read, context, image, 20, &first_tile) ||
            read32(read, context, image, 0, &layout.image_width) ||
            read32(read, context, image, 4, &layout.image_height) ||
            read32(read, context, image, 8, &layout.image_origin_x) ||
            read32(read, context, image, 12, &layout.image_origin_y))
            return GEX_TITLE_PLAN_INVALID;
        if (!(first_tile & 0xffff)) continue;
        gex_title_layout_output position;
        if (!gex_title_layout_part(&layout, &position)) continue;
        if (written == capacity) return GEX_TITLE_PLAN_CAPACITY;
        if ((!tint && read32(read, context, part, 16, &part_tint)) ||
            (!palette && read32(read, context, part, 12, &part_palette)))
            return GEX_TITLE_PLAN_INVALID;
        parts[written++] = (gex_title_part){image, palette ? palette : part_palette,
                                            tint ? tint : part_tint, position.flags,
                                            position.x, position.y, wrap_x, wrap_y};
    }
    return GEX_TITLE_PLAN_INVALID;
}
