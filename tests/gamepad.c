#include <stdint.h>
#include <stdio.h>
#include <SDL2/SDL.h>
#include "host.h"
#include "win32hle.h"
#include "gamepad.h"

static SDL_bool virtual_controller(int index) {
    return SDL_JoystickIsVirtual(index) && SDL_IsGameController(index);
}
#define SDL_IsGameController virtual_controller
#include "../src/platform/desktop/input.c"
#undef SDL_IsGameController

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;
static int failures;
static uint32_t async_key;
static int configured_menu;
static int menu_context(void) { return configured_menu; }

#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__, #expression, SDL_GetError()); ++failures; } \
} while (0)

static int capture_key_test(int down, uint32_t vk, uint32_t scan) {
    if (down) {
        CHECK(vk == 'Q' && scan == 0x10);
        gex_binding_key_filter = NULL;
    }
    return 1;
}

static unsigned key(uint32_t vk) {
    uint32_t args[] = {vk};
    return hle_call_guest(async_key, 1, args) & 0xFFFF;
}

static int attach(void) {
    int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
        SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    if (index < 0) return index;
    char guid[33], mapping[1024];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof guid);
    snprintf(mapping, sizeof mapping,
        "%s,Gex test controller,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
        "leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,"
        "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,", guid);
    CHECK(SDL_GameControllerAddMapping(mapping) >= 0);
    return index;
}

int main(void) {
    recomp_host_init();
    if (hle_screen_open("headless controller test", 0, 1)) return 1;
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK)) return 1;
    if (gex_gamepad_init(1)) return 1;
    async_key = hle_resolve("GetAsyncKeyState");
    mach_enter();
    gex_gamepad_pump();
    CHECK(!key('X'));
    int index = attach();
    if (index < 0) { fprintf(stderr, "virtual joystick: %s\n", SDL_GetError()); return 1; }
    SDL_Joystick *joystick = SDL_JoystickOpen(index);
    if (!joystick) return 1;
    SDL_Delay(1001);
    gex_gamepad_pump();
    static const struct { int button; uint32_t vk; } buttons[] = {
        {SDL_CONTROLLER_BUTTON_A, 'X'}, {SDL_CONTROLLER_BUTTON_B, 'C'},
        {SDL_CONTROLLER_BUTTON_X, 'Z'}, {SDL_CONTROLLER_BUTTON_Y, 'V'},
        {SDL_CONTROLLER_BUTTON_START, 'X'}, {SDL_CONTROLLER_BUTTON_BACK, 0x1B},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 'V'}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 'V'},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x25}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x27},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x26}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x28}
    };
    for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; ++i) {
        CHECK(!SDL_JoystickSetVirtualButton(joystick, buttons[i].button, 1));
        gex_gamepad_pump();
        CHECK(key(buttons[i].vk) == 0x8001);
        gex_gamepad_pump();
        CHECK(key(buttons[i].vk) == 0x8000);
        CHECK(!SDL_JoystickSetVirtualButton(joystick, buttons[i].button, 0));
        gex_gamepad_pump();
        CHECK(!key(buttons[i].vk));
    }
    CHECK(!SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 8000));
    gex_gamepad_pump();
    CHECK(!key(0x27));
    CHECK(!SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 20000));
    gex_gamepad_pump();
    CHECK(key(0x27) == 0x8001);
    CHECK(!SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 12000));
    gex_gamepad_pump();
    CHECK(key(0x27) == 0x8000);
    CHECK(!SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, -20000));
    gex_gamepad_pump();
    CHECK(!key(0x27) && key(0x25) == 0x8001);
    CHECK(!SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 0));
    CHECK(!SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTY, -20000));
    gex_gamepad_pump();
    CHECK(!key(0x25) && key(0x26) == 0x8001);
    CHECK(!SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTY, 0));
    CHECK(hle_screen_key_hook(SDL_SCANCODE_X, 1));
    CHECK(key('X') == 0x8001);
    CHECK(!SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 1));
    gex_gamepad_pump();
    CHECK(key('X') == 0x8000);
    CHECK(hle_screen_key_hook(SDL_SCANCODE_X, 0));
    CHECK(key('X') == 0x8000);
    CHECK(!SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 0));
    gex_gamepad_pump();
    CHECK(!key('X'));
    CHECK(hle_screen_key_hook(SDL_SCANCODE_X, 1));
    CHECK(!SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 1));
    gex_gamepad_pump();
    CHECK(key('X') == 0x8001);
    CHECK(!SDL_JoystickDetachVirtual(index));
    gex_gamepad_pump();
    CHECK(key('X') == 0x8000);
    CHECK(hle_screen_key_hook(SDL_SCANCODE_X, 0));
    CHECK(!key('X'));
    SDL_JoystickClose(joystick);
    index = attach();
    CHECK(index >= 0);
    joystick = SDL_JoystickOpen(index);
    CHECK(joystick);
    SDL_Delay(1001);
    gex_gamepad_pump();
    CHECK(!SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_X, 1));
    gex_gamepad_pump();
    CHECK(key('Z') == 0x8001);
    CHECK(hle_screen_key_hook(SDL_SCANCODE_X, 1));
    CHECK(key('X') == 0x8001);
    headless_mode = 0;
    gex_gamepad_pump();
    CHECK(!key('Z') && !key('X'));
    headless_mode = 1;
    gex_gamepad_pump();
    CHECK(key('Z') == 0x8001);
    CHECK(!SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 1));
    CHECK(!SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_START, 1));
    gex_gamepad_pump();
    CHECK(key('X') == 0x8001);
    CHECK(!key(0x13));
    CHECK(!SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 0));
    gex_gamepad_pump();
    CHECK(key('X') == 0x8000 && !key(0x13));
    CHECK(hle_screen_key_hook(SDL_SCANCODE_X, 1));
    CHECK(!SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_START, 0));
    gex_gamepad_pump();
    CHECK(key('X') == 0x8000 && !key(0x13));
    CHECK(hle_screen_key_hook(SDL_SCANCODE_X, 0));
    CHECK(!key('X') && !key(0x13));
    CHECK(!hle_screen_key_hook(SDL_SCANCODE_F1, 1));
    gex_preferences_reset();
    gex_controls_menu_hook = menu_context;
    CHECK(!gex_controls_bind(GEX_ACTION_JUMP, 0, 'Q', 0x10));
    CHECK(hle_screen_key_hook(SDL_SCANCODE_Q, 1));
    CHECK(key('X') == 0x8001);
    CHECK(hle_screen_key_hook(SDL_SCANCODE_Q, 0));
    CHECK(!key('X'));
    CHECK(hle_screen_key_hook(SDL_SCANCODE_X, 1));
    CHECK(!key('X')); /* The old Jump key must not leak through. */
    CHECK(hle_screen_key_hook(SDL_SCANCODE_X, 0));
    configured_menu = 1;
    CHECK(hle_screen_key_hook(SDL_SCANCODE_RETURN, 1));
    CHECK(key('X') == 0x8001);
    gex_binding_key_filter = capture_key_test;
    CHECK(hle_screen_key_hook(SDL_SCANCODE_RETURN, 0));
    CHECK(!key('X')); /* Release the capture-triggering key normally. */
    CHECK(hle_screen_key_hook(SDL_SCANCODE_Q, 1));
    CHECK(!gex_binding_key_filter);
    CHECK(!key('X'));
    CHECK(hle_screen_key_hook(SDL_SCANCODE_Q, 0));
    CHECK(hle_screen_key_hook(SDL_SCANCODE_RETURN, 1));
    CHECK(key('X') == 0x8001);
    CHECK(hle_screen_key_hook(SDL_SCANCODE_RETURN, 0));
    gex_gamepad_shutdown();
    CHECK(!key('Z') && !hle_screen_key_hook);
    CHECK(!SDL_JoystickDetachVirtual(index));
    SDL_JoystickClose(joystick);
    mach_leave();
    SDL_Quit();
    return failures ? 1 : 0;
}
