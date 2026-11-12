#ifndef GEX_DREAMCAST_MAP_PLAN_H
#define GEX_DREAMCAST_MAP_PLAN_H

#include "title_plan.h"

typedef struct {
    uint32_t descriptor, image;
    uint16_t spec;
    int32_t x, y;
} gex_map_tile;

enum { GEX_MAP_TILE_LIMIT = 128 };

/* The plan contains only drawable tiles, in the original row/layer order.
 * Output is valid only on success. Positions retain 16.16 precision. */
int gex_dreamcast_map_plan(gex_title_read read, void *context,
                           uint32_t chunk, uint32_t table, uint32_t definitions,
                           int32_t x, int32_t y, int16_t screen_x, int16_t screen_y,
                           int width, int height, gex_map_tile tiles[GEX_MAP_TILE_LIMIT],
                           size_t *count);

#endif
