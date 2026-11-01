#ifndef GEX_GAMEPLAY_H
#define GEX_GAMEPLAY_H

#include <stdint.h>

typedef struct {
    unsigned frames, changes;
    uint64_t hash;
    uint32_t player;
    int32_t min_x, max_x, min_y, max_y;
} gex_gameplay_state;

void gex_gameplay_record(gex_gameplay_state *state, uint32_t level, uint64_t hash,
                        uint32_t player, int32_t x, int32_t y);
int gex_gameplay_passed(const gex_gameplay_state *state, uint32_t level, uint32_t player);

#endif
