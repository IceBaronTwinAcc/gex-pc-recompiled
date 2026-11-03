#ifndef GEX_PLATFORM_INPUT_H
#define GEX_PLATFORM_INPUT_H

#include <stdint.h>

enum gex_input_key {
    GEX_KEY_LEFT, GEX_KEY_RIGHT, GEX_KEY_UP, GEX_KEY_DOWN,
    GEX_KEY_X, GEX_KEY_Z, GEX_KEY_C, GEX_KEY_V, GEX_KEY_ESCAPE,
    GEX_KEY_COUNT
};

enum gex_input_button {
    GEX_PAD_LEFT = 1u << 0, GEX_PAD_RIGHT = 1u << 1,
    GEX_PAD_UP = 1u << 2, GEX_PAD_DOWN = 1u << 3,
    GEX_PAD_A = 1u << 4, GEX_PAD_B = 1u << 5,
    GEX_PAD_X = 1u << 6, GEX_PAD_Y = 1u << 7,
    GEX_PAD_START = 1u << 8, GEX_PAD_BACK = 1u << 9,
    GEX_PAD_L = 1u << 10, GEX_PAD_R = 1u << 11
};

typedef struct {
    uint32_t buttons;
    int16_t x, y;
    int focused, connected;
    int reset; /* Release old pad state before consuming a replacement pad. */
} gex_input_state;

int gex_host_input_init(int headless);
/* Returns 0 on a non-owner thread without changing input state. */
int gex_host_input_poll(gex_input_state *state);
void gex_host_input_shutdown(void);
void gex_host_input_key(int down, uint32_t key, uint32_t scan);
void gex_gamepad_key(enum gex_input_key key, int down);

/* Owner-thread modal UI capture; return nonzero to consume an event.
 * UINT32_MAX announces a window-close request so a modal loop can unwind. */
extern int (*gex_input_key_filter)(int down, uint32_t key, uint32_t scan);
/* Framebuffer coordinates; down is 1/0 for buttons, -1 for motion. */
extern int (*gex_input_pointer_filter)(int x, int y, int down);

#endif
