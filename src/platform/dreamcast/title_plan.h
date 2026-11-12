#ifndef GEX_DREAMCAST_TITLE_PLAN_H
#define GEX_DREAMCAST_TITLE_PLAN_H

#include "title_layout.h"
#include <stddef.h>
#include <stdint.h>

typedef int (*gex_title_read)(void *context, uint32_t address, void *out, size_t size);

typedef struct {
    uint32_t image, palette, tint, flags;
    int x, y, wrap_x, wrap_y;
} gex_title_part;

enum {
    GEX_TITLE_PLAN_OK,
    GEX_TITLE_PLAN_INVALID,
    GEX_TITLE_PLAN_CAPACITY
};

/* The caller selects the CEL before planning and must not submit any output
 * unless planning succeeds. read returns zero only for a complete read. */
int gex_title_plan(gex_title_read read, void *context, uint32_t object, uint32_t cel,
                   uint32_t camera_x, uint32_t camera_y, uint32_t current_frame,
                   uint16_t screen_x, uint16_t screen_y, int view_width, int view_height,
                   gex_title_part *parts, size_t capacity, size_t *count);

/* fade_rgb contains the original fade bytes in packet RGB order. */
uint32_t gex_title_packet_rgb(uint32_t tint, uint32_t fade, const uint8_t fade_rgb[3],
                              const uint32_t colors[32]);

#endif
