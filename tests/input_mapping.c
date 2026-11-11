#include <stdio.h>
#include "gamepad.h"
#include "platform/input.h"
#include "preferences.h"

static gex_input_state snapshot;
static int poll_ready = 1, failures;
static unsigned held[256], presses[256];
static unsigned event_count;
static int in_menu;
static int menu_context(void) { return in_menu; }
static int capture_button(uint32_t buttons) { return buttons != 0; }

int gex_host_input_init(int headless) { (void)headless; return 0; }
int gex_host_input_poll(gex_input_state *state) {
    *state = snapshot;
    return poll_ready;
}
void gex_host_input_shutdown(void) {}
void gex_host_input_key(int down, uint32_t key, uint32_t scan) {
    (void)scan;
    held[key] = down;
    if (down) ++presses[key];
    ++event_count;
}

#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

int main(void) {
    CHECK(!gex_gamepad_init(1));
    snapshot.focused = snapshot.connected = 1;
    static const struct { uint32_t button, key; } mapping[] = {
        {GEX_PAD_A, 'X'}, {GEX_PAD_START, 0x13}, {GEX_PAD_B, 'C'},
        {GEX_PAD_X, 'Z'}, {GEX_PAD_Y, 'V'}, {GEX_PAD_L, 'V'},
        {GEX_PAD_R, 'V'}, {GEX_PAD_BACK, 0x1B},
        {GEX_PAD_LEFT, 0x25}, {GEX_PAD_RIGHT, 0x27},
        {GEX_PAD_UP, 0x26}, {GEX_PAD_DOWN, 0x28}
    };
    for (unsigned i = 0; i < sizeof mapping / sizeof mapping[0]; ++i) {
        unsigned old_presses = presses[mapping[i].key];
        snapshot.buttons = mapping[i].button;
        gex_gamepad_pump();
        CHECK(held[mapping[i].key] && presses[mapping[i].key] == old_presses + 1);
        if (mapping[i].button == GEX_PAD_START) CHECK(!held['X']);
        unsigned old_events = event_count;
        gex_gamepad_pump();
        CHECK(event_count == old_events);
        snapshot.buttons = 0;
        gex_gamepad_pump();
        CHECK(!held[mapping[i].key]);
    }
    snapshot.buttons = GEX_PAD_A | GEX_PAD_START;
    gex_gamepad_pump();
    CHECK(held['X'] && held[0x13] && !held[0x1B]);
    snapshot.buttons = GEX_PAD_START;
    gex_gamepad_pump();
    CHECK(!held['X'] && held[0x13]);
    snapshot.buttons = 0;
    gex_gamepad_pump();
    CHECK(!held['X'] && !held[0x13]);
    snapshot.x = 16000;
    gex_gamepad_pump();
    CHECK(!held[0x27]);
    snapshot.x = 16001;
    gex_gamepad_pump();
    CHECK(held[0x27]);
    snapshot.x = 12000;
    gex_gamepad_pump();
    CHECK(held[0x27]);
    snapshot.buttons = GEX_PAD_A;
    gex_gamepad_pump();
    CHECK(held['X']);
    unsigned old_presses = presses['X'];
    snapshot.reset = 1;
    gex_gamepad_pump();
    CHECK(!held[0x27] && held['X'] && presses['X'] == old_presses + 1);
    snapshot.reset = 0;
    gex_gamepad_key(GEX_KEY_X, 1);
    old_presses = presses['X'];
    snapshot.connected = 0;
    gex_gamepad_pump();
    CHECK(held['X'] && presses['X'] == old_presses);
    gex_gamepad_key(GEX_KEY_X, 0);
    CHECK(!held['X']);
    gex_gamepad_key(GEX_KEY_Z, 1);
    snapshot.focused = 0;
    gex_gamepad_pump();
    CHECK(!held['Z']);
    snapshot.focused = snapshot.connected = 1;
    gex_gamepad_pump();
    CHECK(held['X']);
    poll_ready = 0;
    snapshot.buttons = 0;
    gex_gamepad_pump();
    CHECK(held['X']);
    gex_gamepad_shutdown();
    CHECK(!held['X']);
    gex_preferences_reset();
    gex_controls_menu_hook = menu_context;
    poll_ready = 1;
    snapshot = (gex_input_state){.focused = 1, .connected = 1};
    CHECK(!gex_gamepad_init(1));
    snapshot.buttons = GEX_PAD_START;
    gex_gamepad_pump();
    CHECK(held[0x13] && !held[0x1B] && !held['X']);
    snapshot.buttons = 0;
    gex_gamepad_pump();
    CHECK(gex_controls_keyboard(0x1B) == 1u << GEX_KEY_PAUSE);
    CHECK(!gex_controls_bind(GEX_ACTION_PAUSE, 1, GEX_PAD_R, 0));
    CHECK(gex_controls_gamepad(GEX_PAD_START) == 0);
    CHECK(gex_controls_gamepad(GEX_PAD_R) == 1u << GEX_KEY_PAUSE);
    CHECK(!gex_controls_bind(GEX_ACTION_PAUSE, 1, GEX_PAD_START, 0));
    CHECK(!gex_controls_bind(GEX_ACTION_LASH, 1, GEX_PAD_L, 0));
    snapshot.buttons = GEX_PAD_L;
    gex_gamepad_pump();
    CHECK(held['C'] && !held['V']);
    snapshot.buttons = 0;
    gex_gamepad_pump();
    CHECK(!gex_controls_bind(GEX_ACTION_HURL, 0, 'Q', 0x10));
    CHECK(gex_controls_keyboard('Q') == 1u << GEX_KEY_C);
    CHECK(gex_controls_bind(GEX_ACTION_LASH, 0, 'X', 0x2D) == -1);
    CHECK(gex_controls_bind(GEX_ACTION_JUMP, 1, GEX_PAD_Y, 0) == -1);
    CHECK(gex_controls_bind(GEX_ACTION_CONFIRM, 0, 0x1B, 1) == -1);
    CHECK(gex_controls_bind(GEX_ACTION_BACK, 0, 0x0D, 0x1C) == -1);
    CHECK(gex_controls_bind(GEX_ACTION_BACK, 0, 0x20, 0x39) == -1);
    in_menu = 1;
    CHECK(gex_controls_keyboard(0x1B) == 1u << GEX_KEY_ESCAPE);
    CHECK(gex_controls_gamepad(GEX_PAD_B) == 1u << GEX_KEY_ESCAPE);
    snapshot.buttons = GEX_PAD_A;
    gex_gamepad_pump();
    CHECK(!held['X']); /* The opening press is blocked across context changes. */
    snapshot.buttons = 0;
    gex_gamepad_pump();
    snapshot.buttons = GEX_PAD_A;
    gex_gamepad_pump();
    CHECK(held['X']);
    snapshot.buttons = 0;
    gex_gamepad_pump();
    snapshot.buttons = GEX_PAD_START;
    gex_gamepad_pump();
    CHECK(held[0x13] && !held[0x1B]);
    snapshot.buttons = 0;
    gex_gamepad_pump();
    CHECK(!held[0x13]);
    CHECK(!gex_controls_bind(GEX_ACTION_CONFIRM, 1, GEX_PAD_START, 0));
    CHECK(gex_controls_gamepad(GEX_PAD_A) == 0);
    CHECK(gex_controls_gamepad(GEX_PAD_START) == 1u << GEX_KEY_X);
    gex_binding_pad_filter = capture_button;
    snapshot.buttons = GEX_PAD_START;
    gex_gamepad_pump();
    CHECK(!held['X']);
    gex_binding_pad_filter = NULL;
    gex_gamepad_pump();
    CHECK(!held['X']); /* Captured buttons stay consumed until released. */
    snapshot.buttons = 0;
    gex_gamepad_pump();
    snapshot.buttons = GEX_PAD_START;
    gex_gamepad_pump();
    CHECK(held['X']);
    int width, height;
    CHECK(!gex_preferences_default_size(1920, 1080, &width, &height));
    CHECK(width == 427 && height == 240);
    CHECK(!gex_preferences_default_size(1280, 1024, &width, &height));
    CHECK(width == 300 && height == 240);
    CHECK(gex_preferences_default_size(0, 1080, &width, &height) == -1);
    gex_gamepad_shutdown();
    return failures ? 1 : 0;
}
