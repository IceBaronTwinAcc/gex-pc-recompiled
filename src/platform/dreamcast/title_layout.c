#include "title_layout.h"
#include <stdint.h>

static int64_t signed32(uint32_t value) {
    return value & 0x80000000u ? (int64_t)value - INT64_C(0x100000000) : value;
}

static int fixed_floor(uint32_t value) {
    int64_t v = signed32(value);
    return v >= 0 ? (int)(v / 65536) : (int)(-((-v + 65535) / 65536));
}

static int floor_div(int64_t value, int divisor) {
    return value >= 0 ? (int)(value / divisor) :
                        (int)(-((-value + divisor - 1) / divisor));
}

static int signed16(unsigned value) {
    value &= 0xffffu;
    return value & 0x8000u ? (int)value - 65536 : (int)value;
}

void gex_title_layout_wrap(const gex_title_wrap_input *input, int *x, int *y) {
    *x = *y = 0;
    if ((input->object_flags & 0x01000000u) ||
        input->object_frame - input->current_frame != UINT32_MAX)
        return;
    uint32_t dx = input->x - input->previous_x;
    uint32_t dy = input->y - input->previous_y;
    if (signed32(dx) < 0) dx += 65536u;
    if (signed32(dy) < 0) dy += 65536u;
    int sx = signed16((unsigned)floor_div(signed32(dx), 131072) - input->screen_x);
    int sy = signed16((unsigned)floor_div(signed32(dy), 131072) - input->screen_y);
    int distance = (sx < 0 ? -sx : sx) + (sy < 0 ? -sy : sy);
    if (distance <= 100) {
        *x = sx;
        *y = sy;
    }
}

int gex_title_layout_part(const gex_title_layout_input *input,
                          gex_title_layout_output *output) {
    uint32_t width = input->image_width, height = input->image_height;
    uint32_t xpart = input->part_position & 0xffff0000u;
    uint32_t ypart = input->part_position << 16;
    uint32_t xorigin = input->part_flags & 0x80000000u ?
        width - input->image_origin_x : input->image_origin_x;
    uint32_t yorigin = input->part_flags & 0x40000000u ?
        height - input->image_origin_y : input->image_origin_y;
    uint32_t x = input->object_x - input->camera_x;
    uint32_t y = input->object_y - input->camera_y;
    x = input->object_flags & 0x80000000u ?
        x - xorigin - xpart : x + xpart + xorigin;
    y = input->object_flags & 0x40000000u ?
        y - yorigin - ypart : y + ypart + yorigin;
    uint32_t flags = input->object_flags ^ input->part_flags;
    uint32_t right = (uint32_t)input->view_width << 16;
    uint32_t bottom = (uint32_t)input->view_height << 16;
    if (flags & 0x80000000u) {
        if (signed32(x) < 0 || signed32(x - width) >= signed32(right)) return 0;
    } else {
        if (signed32(x + width) < 0 || signed32(x) >= signed32(right)) return 0;
    }
    if (flags & 0x40000000u) {
        if (signed32(y) < 0 || signed32(y - height) >= signed32(bottom)) return 0;
    } else {
        if (signed32(y + height) < 0 || signed32(y) >= signed32(bottom)) return 0;
    }
    output->x = fixed_floor(x);
    output->y = fixed_floor(y);
    output->flags = flags;
    return 1;
}
