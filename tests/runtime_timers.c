#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "host.h"
#include "win32hle.h"
#include "platform/runtime.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;
static atomic_uint callbacks, self_killed;
static uint32_t kill_timer;
static int failures;

#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

static void callback(void) {
    atomic_fetch_add(&callbacks, 1);
    if (A32(2) == 1) {
        uint32_t args[] = {A32(0)};
        if (!hle_call_guest(kill_timer, 1, args)) atomic_fetch_add(&self_killed, 1);
    }
    RET(0, 5);
}

static void wait_us(unsigned microseconds) {
    hle_block_begin();
    gex_runtime_sleep_us(microseconds);
    hle_block_end();
}

static void await(atomic_uint *counter, unsigned minimum) {
    uint64_t deadline = gex_runtime_time_us() + 1000000;
    while (atomic_load(counter) < minimum && gex_runtime_time_us() < deadline)
        wait_us(1000);
    CHECK(atomic_load(counter) >= minimum);
}

int main(void) {
    recomp_host_init();
    const win32hle_shim shims[] = {{"TimerTestCallback", callback}, {NULL, NULL}};
    win32hle_register(shims);
    mach_enter();
    uint32_t set_timer = hle_resolve("timeSetEvent");
    kill_timer = hle_resolve("timeKillEvent");
    uint32_t time_get_time = hle_resolve("timeGetTime");
    CHECK(set_timer && kill_timer && time_get_time);
    uint64_t before = gex_runtime_time_us();
    uint32_t guest_before = hle_call_guest(time_get_time, 0, NULL);
    wait_us(5000);
    CHECK(gex_runtime_time_us() - before >= 5000);
    CHECK((uint32_t)(hle_call_guest(time_get_time, 0, NULL) - guest_before) >= 5);

    uint32_t args[] = {2, 1, hle_resolve("TimerTestCallback"), 0, 1};
    uint32_t timer = hle_call_guest(set_timer, 5, args);
    CHECK(timer);
    await(&callbacks, 3);
    CHECK(!hle_call_guest(kill_timer, 1, &timer));
    unsigned stopped = atomic_load(&callbacks);
    wait_us(20000);
    CHECK(atomic_load(&callbacks) == stopped);
    CHECK(hle_call_guest(kill_timer, 1, &timer) == 97);

    args[3] = 1;
    timer = hle_call_guest(set_timer, 5, args);
    CHECK(timer);
    await(&self_killed, 1);
    stopped = atomic_load(&callbacks);
    wait_us(20000);
    CHECK(atomic_load(&callbacks) == stopped);

    args[3] = 0;
    args[4] = 0;
    timer = hle_call_guest(set_timer, 5, args);
    CHECK(timer);
    await(&callbacks, stopped + 1);
    wait_us(20000);
    CHECK(atomic_load(&callbacks) == stopped + 1);
    mach_leave();
    return failures ? 1 : 0;
}
