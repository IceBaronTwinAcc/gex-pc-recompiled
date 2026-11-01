#include <string.h>
#include "gameplay.h"

void gex_gameplay_record(gex_gameplay_state *state, uint32_t level, uint64_t hash,
                        uint32_t player, int32_t x, int32_t y) {
    if (level != 3) {
        memset(state, 0, sizeof *state);
        return;
    }
    if (state->frames && hash != state->hash) ++state->changes;
    state->hash = hash;
    ++state->frames;
    if (!player) {
        state->player = 0;
        return;
    }
    if (player != state->player) {
        state->player = player;
        state->min_x = state->max_x = x;
        state->min_y = state->max_y = y;
    } else {
        if (x < state->min_x) state->min_x = x;
        if (x > state->max_x) state->max_x = x;
        if (y < state->min_y) state->min_y = y;
        if (y > state->max_y) state->max_y = y;
    }
}

int gex_gameplay_passed(const gex_gameplay_state *state, uint32_t level, uint32_t player) {
    return level == 3 && player && player == state->player &&
           state->frames >= 120 && state->changes >= 60 &&
           (int64_t)state->max_x - state->min_x >= 16 * 65536 &&
           (int64_t)state->max_y - state->min_y >= 8 * 65536;
}
