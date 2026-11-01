#include <stdio.h>
#include <SDL2/SDL.h>
#include "win32hle.h"
#include "platform/input.h"

static SDL_GameController *controller;
static SDL_threadID owner;
static int headless_mode;
static uint32_t last_scan;
static const SDL_Scancode scancodes[GEX_KEY_COUNT] = {
    SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_ESCAPE
};

static int keyboard_event(int scancode, int down) {
    for (unsigned i = 0; i < GEX_KEY_COUNT; ++i)
        if (scancode == scancodes[i]) {
            gex_gamepad_key((enum gex_input_key)i, down);
            return 1;
        }
    return 0;
}

int gex_host_input_init(int headless) {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)) {
        fprintf(stderr, "[gamepad] cannot initialize controllers: %s\n", SDL_GetError());
        return -1;
    }
    owner = SDL_ThreadID();
    headless_mode = headless;
    last_scan = SDL_GetTicks() - 1000;
    hle_screen_key_hook = keyboard_event;
    return 0;
}

int gex_host_input_poll(gex_input_state *state) {
    if (SDL_ThreadID() != owner) return 0;
    SDL_GameControllerUpdate();
    int disconnected = 0;
    if (controller && !SDL_GameControllerGetAttached(controller)) {
        fprintf(stderr, "[gamepad] disconnected\n");
        SDL_GameControllerClose(controller);
        controller = NULL;
        disconnected = 1;
        last_scan = SDL_GetTicks() - 1000;
    }
    uint32_t now = SDL_GetTicks();
    if (!controller && now - last_scan >= 1000) {
        last_scan = now;
        int count = SDL_NumJoysticks();
        for (int i = 0; i < count; ++i) {
            if (!SDL_IsGameController(i)) continue;
            controller = SDL_GameControllerOpen(i);
            if (controller) {
                fprintf(stderr, "[gamepad] connected: %s\n", SDL_GameControllerName(controller));
                break;
            }
            fprintf(stderr, "[gamepad] cannot open controller %d: %s\n", i, SDL_GetError());
        }
    }
    *state = (gex_input_state){.focused = headless_mode || SDL_GetKeyboardFocus(),
                               .connected = controller != NULL, .reset = disconnected};
    if (controller) {
        state->x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
        state->y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
        static const struct { SDL_GameControllerButton button; uint32_t bit; } buttons[] = {
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, GEX_PAD_LEFT},
            {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, GEX_PAD_RIGHT},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, GEX_PAD_UP},
            {SDL_CONTROLLER_BUTTON_DPAD_DOWN, GEX_PAD_DOWN},
            {SDL_CONTROLLER_BUTTON_A, GEX_PAD_A}, {SDL_CONTROLLER_BUTTON_B, GEX_PAD_B},
            {SDL_CONTROLLER_BUTTON_X, GEX_PAD_X}, {SDL_CONTROLLER_BUTTON_Y, GEX_PAD_Y},
            {SDL_CONTROLLER_BUTTON_START, GEX_PAD_START}, {SDL_CONTROLLER_BUTTON_BACK, GEX_PAD_BACK},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, GEX_PAD_L},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, GEX_PAD_R}
        };
        for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; ++i)
            if (SDL_GameControllerGetButton(controller, buttons[i].button))
                state->buttons |= buttons[i].bit;
    }
    return 1;
}

void gex_host_input_key(int down, uint32_t key, uint32_t scan) {
    hle_input_key(down, key, scan);
}

void gex_host_input_shutdown(void) {
    if (controller) SDL_GameControllerClose(controller);
    controller = NULL;
    hle_screen_key_hook = NULL;
    owner = 0;
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}
