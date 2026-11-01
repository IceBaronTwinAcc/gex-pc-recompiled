#include <stdio.h>
#include "gamepad.h"
#include "platform/input.h"

static gex_input_state snapshot;
static int poll_ready = 1, failures;
static unsigned held[256], presses[256];
static unsigned event_count;

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
        {GEX_PAD_A, 'X'}, {GEX_PAD_START, 'X'}, {GEX_PAD_B, 'C'},
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
        unsigned old_events = event_count;
        gex_gamepad_pump();
        CHECK(event_count == old_events);
        snapshot.buttons = 0;
        gex_gamepad_pump();
        CHECK(!held[mapping[i].key]);
    }
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
    return failures ? 1 : 0;
}
