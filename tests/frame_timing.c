#include <stdint.h>
#include <stdio.h>
#include "host.h"
#include "win32hle.h"

void gex_guest_wait(void);

static int failures;
static uint32_t now, wake_step, overshoot, waits[8];
static unsigned wait_count, clock_count;

#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

static void clock_read(void) {
    ++clock_count;
    RET(now, 0);
}

static void sleep_until(void) {
    uint32_t ms = A32(0);
    CHECK(ms > 0 && ms <= 33);
    if (wait_count < 8) waits[wait_count] = ms;
    if (++wait_count > 8) hle_fatal("frame limiter did not block until its deadline");
    now += (wake_step && ms > wake_step ? wake_step : ms) + overshoot;
    RETV(1);
}

static void run(uint32_t previous, uint32_t tick, uint32_t step,
                uint32_t late, uint32_t result, uint32_t deadline, unsigned sleeps) {
    MEM32(0x4517E8) = previous;
    now = tick;
    wake_step = step;
    overshoot = late;
    wait_count = clock_count = 0;
    g_ebx = 0x12345678;
    g_ebp = 0x23456789;
    g_esi = 0x3456789A;
    g_edi = 0x456789AB;
    uint32_t stack = g_esp;
    CHECK(hle_call_guest(0x405120, 0, NULL) == result);
    CHECK(g_esp == stack);
    CHECK(g_ebx == 0x12345678 && g_ebp == 0x23456789);
    CHECK(g_esi == 0x3456789A && g_edi == 0x456789AB);
    CHECK(MEM32(0x4517E8) == deadline);
    CHECK(wait_count == sleeps && clock_count == sleeps + 1);
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    recomp_host_init();
    if (recomp_host_boot_permissive(argv[1])) return 1;
    const win32hle_shim shims[] = {
        {"FrameTestClock", clock_read}, {"FrameTestSleep", sleep_until}, {NULL, NULL}
    };
    win32hle_register(shims);
    mach_enter();
    MEM32(0x4A5688) = hle_resolve("FrameTestClock");
    MEM32(0x4A546C) = hle_resolve("FrameTestSleep");
    run(0, 1000, 0, 0, 1, 1000, 0);
    run(1000, 1000, 0, 0, 1, 1033, 1);
    CHECK(waits[0] == 33 && now == 1033);
    run(1000, 1010, 10, 0, 1, 1033, 3);
    CHECK(waits[0] == 23 && waits[1] == 13 && waits[2] == 3);
    run(1000, 1000, 0, 5, 1, 1033, 1);
    CHECK(now == 1038);
    run(1000, 1033, 0, 0, 1, 1033, 0);
    run(1000, 1065, 0, 0, 1, 1033, 0);
    run(1000, 1066, 0, 0, 0, 1033, 0);
    run(1000, 1365, 0, 0, 0, 1033, 0);
    run(1000, 1366, 0, 0, 1, 1366, 0);
    run(UINT32_MAX - 100, UINT32_MAX - 90, 0, 0, 1, UINT32_MAX - 67, 1);
    run(UINT32_MAX - 20, UINT32_MAX - 10, 0, 0, 1, 12, 1);
    CHECK(waits[0] == 23 && now == 12);
    uint32_t stack = g_esp;
    g_eax = 19; g_ecx = 23; g_edx = 29;
    g_flag_a = 31; g_flag_b = 37; g_flag_k = 0; g_flag_cf = 1;
    gex_guest_wait();
    CHECK(g_esp == stack && g_eax == 19 && g_ecx == 23 && g_edx == 29);
    CHECK(g_ebx == 0x12345678 && g_ebp == 0x23456789);
    CHECK(g_esi == 0x3456789A && g_edi == 0x456789AB);
    CHECK(g_flag_a == 31 && g_flag_b == 37 && g_flag_k == 0 && g_flag_cf == 1);
    mach_leave();
    return failures ? 1 : 0;
}
