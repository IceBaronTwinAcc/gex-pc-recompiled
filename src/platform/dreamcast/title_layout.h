#ifndef GEX_DREAMCAST_TITLE_LAYOUT_H
#define GEX_DREAMCAST_TITLE_LAYOUT_H

#include <stdint.h>

typedef struct {
    uint32_t object_x, object_y, camera_x, camera_y, object_flags;
    uint32_t part_position, part_flags;
    uint32_t image_width, image_height, image_origin_x, image_origin_y;
    int view_width, view_height;
} gex_title_layout_input;

typedef struct {
    int x, y;
    uint32_t flags;
} gex_title_layout_output;

typedef struct {
    uint32_t x, y, previous_x, previous_y;
    uint32_t object_flags, object_frame, current_frame;
    uint16_t screen_x, screen_y;
} gex_title_wrap_input;

/* Returns 1 for a visible part and 0 if the original CEL culling rejects it. */
int gex_title_layout_part(const gex_title_layout_input *input,
                          gex_title_layout_output *output);
void gex_title_layout_wrap(const gex_title_wrap_input *input, int *x, int *y);

#endif
