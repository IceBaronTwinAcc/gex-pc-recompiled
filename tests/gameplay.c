#include <limits.h>
#include <stdio.h>
#include "gameplay.h"

static int failures;
#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

int main(void) {
    gex_gameplay_state state = {0};
    CHECK(!gex_gameplay_passed(&state, 3, 0x1000));
    for (unsigned i = 0; i < 120; ++i)
        gex_gameplay_record(&state, 3, i < 61 ? i : 60, 0x1000,
                            i ? 16 * 65536 : 0, i ? 8 * 65536 : 0);
    CHECK(state.frames == 120 && state.changes == 60);
    CHECK(gex_gameplay_passed(&state, 3, 0x1000));
    CHECK(!gex_gameplay_passed(&state, 55, 0x1000));
    CHECK(!gex_gameplay_passed(&state, 3, 0));
    CHECK(!gex_gameplay_passed(&state, 3, 0x2000));
    gex_gameplay_state boundary = state;
    --state.frames;
    CHECK(!gex_gameplay_passed(&state, 3, 0x1000));
    state = boundary; --state.changes;
    CHECK(!gex_gameplay_passed(&state, 3, 0x1000));
    state = boundary; --state.max_x;
    CHECK(!gex_gameplay_passed(&state, 3, 0x1000));
    state = boundary; --state.max_y;
    CHECK(!gex_gameplay_passed(&state, 3, 0x1000));
    state = boundary;
    gex_gameplay_record(&state, 3, 60, 0x2000, INT32_MIN, INT32_MIN);
    CHECK(!gex_gameplay_passed(&state, 3, 0x2000));
    gex_gameplay_record(&state, 3, 60, 0x2000, INT32_MAX, INT32_MAX);
    CHECK(gex_gameplay_passed(&state, 3, 0x2000));
    gex_gameplay_record(&state, 3, 60, 0, 0, 0);
    CHECK(!gex_gameplay_passed(&state, 3, 0x2000));
    state = boundary;
    gex_gameplay_record(&state, 49, 61, 0x1000, 0, 0);
    CHECK(!state.frames && !state.changes && !state.player);
    CHECK(!gex_gameplay_passed(&state, 3, 0x1000));
    return failures ? 1 : 0;
}
