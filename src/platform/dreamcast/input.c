#include <kos.h>
#include "win32hle.h"
#include "platform/input.h"

static kthread_t *owner;
static maple_device_t *previous;

int gex_host_input_init(int headless) {
    (void)headless;
    owner = thd_get_current();
    previous = NULL;
    return 0;
}
int gex_host_input_poll(gex_input_state *state) {
    if (!owner || owner != thd_get_current()) return 0;
    maple_device_t *device = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *pad = device ? maple_dev_status(device) : NULL;
    *state = (gex_input_state){.focused = 1, .connected = pad != NULL,
                               .reset = device != previous};
    previous = device;
    if (!pad) return 1;
    state->x = (int16_t)(pad->joyx * 256);
    state->y = (int16_t)(pad->joyy * 256);
    static const struct { unsigned native, portable; } buttons[] = {
        {CONT_DPAD_LEFT, GEX_PAD_LEFT}, {CONT_DPAD_RIGHT, GEX_PAD_RIGHT},
        {CONT_DPAD_UP, GEX_PAD_UP}, {CONT_DPAD_DOWN, GEX_PAD_DOWN},
        {CONT_A, GEX_PAD_A}, {CONT_B, GEX_PAD_B},
        {CONT_X, GEX_PAD_X}, {CONT_Y, GEX_PAD_Y}, {CONT_START, GEX_PAD_START}
    };
    for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; ++i)
        if (pad->buttons & buttons[i].native) state->buttons |= buttons[i].portable;
    if (pad->ltrig > 128) state->buttons |= GEX_PAD_L;
    if (pad->rtrig > 128) state->buttons |= GEX_PAD_R;
    if ((pad->buttons & (CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START)) ==
        (CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START))
        state->buttons = GEX_PAD_BACK;
    return 1;
}
void gex_host_input_key(int down, uint32_t key, uint32_t scan) { hle_input_key(down, key, scan); }
void gex_host_input_shutdown(void) { owner = NULL; previous = NULL; }
