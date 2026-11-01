#include "autoplay.h"

#define ANY_LEVEL UINT32_MAX

static const struct {
    uint32_t level, delay;
    gex_autoplay_event event;
    unsigned minimum_frames;
    unsigned minimum_updates;
} steps[] = {
    {63, 2000, {'X', 0x2D, 1}, 60}, {ANY_LEVEL, 300, {'X', 0x2D, 0}},
    {55, 3000, {'X', 0x2D, 1}, 60}, {ANY_LEVEL, 300, {'X', 0x2D, 0}},
    {55, 3000, {0x27, 0x4D, 1}, 0, 30},
    {55, 1000, {0x27, 0x4D, 0}, 0, 30},
    {55, 0, {'X', 0x2D, 1}, 0, 30}, {ANY_LEVEL, 300, {'X', 0x2D, 0}},
    {49, 5000, {0x28, 0x50, 1}, 60}, {49, 300, {0x28, 0x50, 0}, 0, 9},
    {49, 1700, {'X', 0x2D, 1}, 0, 30}, {ANY_LEVEL, 300, {'X', 0x2D, 0}},
    {3, 3000, {0x27, 0x4D, 1}},
    {3, 1000, {'X', 0x2D, 1}}, {ANY_LEVEL, 300, {'X', 0x2D, 0}},
    {ANY_LEVEL, 2700, {0x27, 0x4D, 0}},
    {ANY_LEVEL, 5000, {0x1B, 0x01, 1}}, {ANY_LEVEL, 300, {0x1B, 0x01, 0}},
};

int gex_autoplay_next(gex_autoplay_state *state, uint32_t elapsed_ms,
                      uint32_t level, unsigned scene_frames, gex_autoplay_event *event) {
    if (state->step >= sizeof steps / sizeof steps[0]) return 0;
    if (steps[state->step].level != ANY_LEVEL && steps[state->step].level != level) {
        state->waiting = 0;
        return 0;
    }
    if (!state->waiting) {
        state->ready_at = elapsed_ms;
        state->ready_frame = scene_frames;
        state->waiting = 1;
    }
    if (elapsed_ms - state->ready_at < steps[state->step].delay) return 0;
    if (scene_frames - state->ready_frame < steps[state->step].minimum_updates) return 0;
    if (scene_frames < steps[state->step].minimum_frames) return 0;
    if (steps[state->step].event.key == 0x1B && elapsed_ms < 30000) return 0;
    if (!steps[state->step].event.down &&
        state->pressed_key == steps[state->step].event.key &&
        state->pressed_level == level && scene_frames - state->pressed_frame < 2)
        return 0;
    *event = steps[state->step].event;
    if (event->down) {
        state->pressed_level = level;
        state->pressed_key = event->key;
        state->pressed_frame = scene_frames;
    }
    if (steps[state->step].level == 3 && event->key == 0x27 && event->down)
        state->motion_started = 1;
    ++state->step;
    state->waiting = 0;
    return 1;
}
