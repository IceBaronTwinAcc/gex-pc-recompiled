#include <string.h>
#include "gamepad.h"
#include "platform/input.h"

static int axis_x, axis_y;
static const struct { uint32_t key, scan; } bindings[GEX_KEY_COUNT] = {
    {0x25, 0x4B}, {0x27, 0x4D}, {0x26, 0x48}, {0x28, 0x50},
    {'X', 0x2D}, {'Z', 0x2C}, {'C', 0x2E}, {'V', 0x2F}, {0x1B, 0x01}
};
static uint8_t keyboard[GEX_KEY_COUNT], pad[GEX_KEY_COUNT], sent[GEX_KEY_COUNT];

static void publish(void) {
    for (unsigned i = 0; i < GEX_KEY_COUNT; ++i) {
        int down = keyboard[i] || pad[i];
        if (down != sent[i]) {
            gex_host_input_key(down, bindings[i].key, bindings[i].scan);
            sent[i] = (uint8_t)down;
        }
    }
}

void gex_gamepad_key(enum gex_input_key key, int down) {
    if ((unsigned)key >= GEX_KEY_COUNT) return;
    keyboard[key] = down != 0;
    publish();
}

static int direction(int value, int previous) {
    if (value < -16000 || (previous < 0 && value < -10000)) return -1;
    if (value > 16000 || (previous > 0 && value > 10000)) return 1;
    return 0;
}

int gex_gamepad_init(int headless) { return gex_host_input_init(headless); }

void gex_gamepad_pump(void) {
    gex_input_state state;
    if (!gex_host_input_poll(&state)) return;
    memset(pad, 0, sizeof pad);
    if (state.reset) {
        axis_x = axis_y = 0;
        publish();
    }
    if (!state.connected) axis_x = axis_y = 0;
    if (!state.focused) {
        memset(keyboard, 0, sizeof keyboard);
        axis_x = axis_y = 0;
    } else if (state.connected) {
        axis_x = direction(state.x, axis_x);
        axis_y = direction(state.y, axis_y);
        uint32_t buttons = state.buttons;
        pad[0] = axis_x < 0 || (buttons & GEX_PAD_LEFT);
        pad[1] = axis_x > 0 || (buttons & GEX_PAD_RIGHT);
        pad[2] = axis_y < 0 || (buttons & GEX_PAD_UP);
        pad[3] = axis_y > 0 || (buttons & GEX_PAD_DOWN);
        pad[4] = (buttons & (GEX_PAD_A | GEX_PAD_START)) != 0;
        pad[5] = (buttons & GEX_PAD_X) != 0;
        pad[6] = (buttons & GEX_PAD_B) != 0;
        pad[7] = (buttons & (GEX_PAD_Y | GEX_PAD_L | GEX_PAD_R)) != 0;
        pad[8] = (buttons & GEX_PAD_BACK) != 0;
    }
    publish();
}

void gex_gamepad_shutdown(void) {
    memset(keyboard, 0, sizeof keyboard);
    memset(pad, 0, sizeof pad);
    publish();
    axis_x = axis_y = 0;
    gex_host_input_shutdown();
}
