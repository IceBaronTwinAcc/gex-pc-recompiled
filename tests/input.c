#include <stdio.h>
#include "win32hle.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;

static int failures;
static uint32_t async_key;

static void expect(uint32_t key, uint32_t expected, const char *description) {
    uint32_t actual = hle_call_guest(async_key, 1, &key);
    if (actual != expected) {
        fprintf(stderr, "%s: expected 0x%X, got 0x%X\n", description, expected, actual);
        ++failures;
    }
}

int main(void) {
    win32hle_register(win32hle_user32);
    async_key = hle_resolve("GetAsyncKeyState");
    if (!async_key) return 1;
    mach_enter();
    expect(0x27, 0, "initial state");
    hle_input_key(1, 0x27, 0x4D);
    expect(0x27, 0x8001, "press includes held and pressed bits");
    expect(0x27, 0x8000, "query consumes pressed bit");
    hle_input_key(1, 0x27, 0x4D);
    expect(0x27, 0x8000, "repeat is not a new press");
    hle_input_key(0, 0x27, 0x4D);
    expect(0x27, 0, "released state");
    hle_input_key(1, 0x27, 0x4D);
    hle_input_key(0, 0x27, 0x4D);
    expect('X', 0, "queries are per key");
    expect(0x27, 1, "quick tap survives release");
    expect(0x27, 0, "tap consumed once");
    hle_input_key_state('X', 1);
    hle_input_key_state('X', 0);
    expect('X', 1, "state-only input records press");
    hle_input_key(1, 0xA0, 0x2A);
    expect(0x10, 0x8001, "left shift updates generic shift");
    hle_input_key(1, 0xA1, 0x36);
    hle_input_key(0, 0xA0, 0x2A);
    expect(0x10, 0x8000, "right shift remains held");
    hle_input_key(0, 0xA1, 0x36);
    expect(0x10, 0, "both shifts released");
    hle_input_mouse(0x201, 0, 0);
    hle_input_mouse(0x202, 0, 0);
    expect(1, 1, "mouse tap records press");
    expect(256, 0, "out-of-range virtual key");
    mach_leave();
    return failures ? 1 : 0;
}
