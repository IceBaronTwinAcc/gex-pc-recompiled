#ifndef GEX_AUTOPLAY_H
#define GEX_AUTOPLAY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t step;
    uint32_t ready_at;
    unsigned ready_frame;
    uint32_t pressed_level, pressed_key;
    unsigned pressed_frame;
    int waiting;
    int motion_started;
} gex_autoplay_state;

typedef struct {
    uint32_t key, scan;
    int down;
} gex_autoplay_event;

int gex_autoplay_next(gex_autoplay_state *state, uint32_t elapsed_ms,
                      uint32_t level, unsigned scene_frames, gex_autoplay_event *event);

#endif
