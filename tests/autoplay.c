#include <stdio.h>
#include "autoplay.h"

static int failures;
static int next(gex_autoplay_state *state, uint32_t time, uint32_t level,
                gex_autoplay_event *event) {
    return gex_autoplay_next(state, time, level, time / 10, event);
}
#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

int main(void) {
    gex_autoplay_state state = {0};
    gex_autoplay_event event;
    CHECK(!next(&state, 0, 0, &event));
    CHECK(!next(&state, 10000, 0, &event));
    CHECK(!next(&state, 10000, 63, &event));
    CHECK(!next(&state, 11999, 63, &event));
    CHECK(next(&state, 12000, 63, &event));
    CHECK(event.key == 'X' && event.down);
    CHECK(!next(&state, 12000, 55, &event));
    CHECK(next(&state, 12300, 55, &event));
    CHECK(event.key == 'X' && !event.down);
    CHECK(!next(&state, 12300, 55, &event));
    CHECK(!next(&state, 14000, 63, &event));
    CHECK(!next(&state, 15000, 55, &event));
    CHECK(!next(&state, 17999, 55, &event));
    CHECK(next(&state, 18000, 55, &event));
    CHECK(event.key == 'X' && event.down);
    CHECK(!next(&state, 18000, 49, &event));
    CHECK(next(&state, 18300, 49, &event));
    CHECK(!event.down);
    CHECK(!next(&state, 100000, 49, &event));
    CHECK(state.step == 4);
    CHECK(!state.motion_started);

    state = (gex_autoplay_state){.step = 8};
    CHECK(!next(&state, 0, 49, &event));
    CHECK(!gex_autoplay_next(&state, 5000, 49, 59, &event));
    CHECK(gex_autoplay_next(&state, 5000, 49, 60, &event));
    CHECK(event.key == 0x28 && event.down);

    state = (gex_autoplay_state){.step = 6};
    CHECK(!gex_autoplay_next(&state, 0, 55, 10, &event));
    CHECK(!gex_autoplay_next(&state, 5000, 55, 39, &event));
    CHECK(gex_autoplay_next(&state, 5000, 55, 40, &event));
    CHECK(event.key == 'X' && event.down);

    state = (gex_autoplay_state){.step = 4};
    CHECK(!gex_autoplay_next(&state, 0, 55, 70, &event));
    CHECK(!gex_autoplay_next(&state, 30000, 55, 99, &event));
    CHECK(gex_autoplay_next(&state, 30000, 55, 100, &event));
    CHECK(event.key == 0x27 && event.down);
    CHECK(!gex_autoplay_next(&state, 30000, 55, 100, &event));
    CHECK(!gex_autoplay_next(&state, 35000, 55, 129, &event));
    CHECK(gex_autoplay_next(&state, 35000, 55, 130, &event));
    CHECK(event.key == 0x27 && !event.down);

    state = (gex_autoplay_state){.step = 10};
    CHECK(!gex_autoplay_next(&state, 0, 49, 70, &event));
    CHECK(!gex_autoplay_next(&state, 30000, 49, 99, &event));
    CHECK(gex_autoplay_next(&state, 30000, 49, 100, &event));
    CHECK(event.key == 'X' && event.down);

    state = (gex_autoplay_state){0};
    CHECK(!gex_autoplay_next(&state, 0, 63, 0, &event));
    CHECK(!gex_autoplay_next(&state, 2000, 63, 59, &event));
    CHECK(gex_autoplay_next(&state, 2000, 63, 60, &event));
    CHECK(event.down);
    CHECK(!gex_autoplay_next(&state, 2000, 63, 60, &event));
    CHECK(!gex_autoplay_next(&state, 2300, 63, 61, &event));
    CHECK(!gex_autoplay_next(&state, 5000, 63, 61, &event));
    CHECK(gex_autoplay_next(&state, 5000, 63, 62, &event));
    CHECK(!event.down);

    state = (gex_autoplay_state){0};
    unsigned pressed[256] = {0}, count = 0;
    for (uint32_t time = 0; time <= 45000; time += 10) {
        uint32_t level = state.step < 2 ? 63 : state.step < 7 ? 55 :
                         state.step < 12 ? 49 : 3;
        if (!next(&state, time, level, &event)) continue;
        CHECK(event.key < 256);
        CHECK(pressed[event.key] != (unsigned)event.down);
        pressed[event.key] = (unsigned)event.down;
        if (event.key == 0x1B) CHECK(time >= 30000);
        ++count;
    }
    CHECK(count == 18);
    CHECK(state.motion_started);
    for (unsigned i = 0; i < 256; ++i) CHECK(!pressed[i]);
    CHECK(!next(&state, UINT32_MAX, 3, &event));
    return failures ? 1 : 0;
}
