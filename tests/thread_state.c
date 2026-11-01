#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "host.h"
#include "win32hle.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int phase, failures;
static const char outer_import[] = "outer";
static const char inner_import[] = "inner";

static void fill(unsigned seed) {
    g_eax = seed; g_ecx = seed + 1; g_edx = seed + 2;
    g_ebx = seed + 3; g_esi = seed + 4; g_edi = seed + 5; g_ebp = seed + 6;
    g_cur_func = seed + 7;
    g_gs_base = seed + 8;
    g_flag_k = seed + 9; g_flag_a = seed + 10;
    g_flag_b = seed + 11; g_flag_cf = seed + 12;
    g_fp_top = seed % 8; g_fpu_cw = (uint16_t)(0x027F + seed);
    g_mxcsr = 0x1F80 + seed;
    g_seg_cs = (uint16_t)seed; g_seg_ds = (uint16_t)(seed + 1);
    g_seg_es = (uint16_t)(seed + 2); g_seg_fs = (uint16_t)(seed + 3);
    g_seg_gs = (uint16_t)(seed + 4); g_seg_ss = (uint16_t)(seed + 5);
    g_cur_import = seed == 100 ? outer_import : inner_import;
    for (unsigned i = 0; i < 8; ++i) {
        g_st[i] = seed + i + 0.5;
        g_mm[i] = ((uint64_t)seed << 32) + i;
        for (unsigned j = 0; j < 4; ++j) g_xmm[i].u32[j] = seed + i * 4 + j;
    }
}

#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

static void expect_outer(uint32_t stack, uint32_t tib) {
    CHECK(g_eax == 100 && g_ecx == 101 && g_edx == 102);
    CHECK(g_ebx == 103 && g_esi == 104 && g_edi == 105 && g_ebp == 106);
    CHECK(g_esp == stack && g_fs_base == tib && g_gs_base == 108);
    CHECK(g_cur_func == 107 && g_cur_import == outer_import);
    CHECK(g_flag_k == 109 && g_flag_a == 110 && g_flag_b == 111 && g_flag_cf == 112);
    CHECK(g_fp_top == 4 && g_fpu_cw == 0x027F + 100 && g_mxcsr == 0x1F80 + 100);
    CHECK(g_seg_cs == 100 && g_seg_ds == 101 && g_seg_es == 102);
    CHECK(g_seg_fs == 103 && g_seg_gs == 104 && g_seg_ss == 105);
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(g_st[i] == 100 + i + 0.5);
        CHECK(g_mm[i] == ((uint64_t)100 << 32) + i);
        for (unsigned j = 0; j < 4; ++j) CHECK(g_xmm[i].u32[j] == 100 + i * 4 + j);
    }
}

static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&gate);
    while (!phase) pthread_cond_wait(&ready, &gate);
    pthread_mutex_unlock(&gate);
    mach_enter();
    CHECK(g_mxcsr == 0x1F80 && g_fpu_cw == 0x027F);
    CHECK(g_flag_k == 0 && g_flag_a == 0 && g_flag_b == 0 && g_flag_cf == 0);
    fill(200);
    mach_leave();
    pthread_mutex_lock(&gate);
    phase = 2;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&gate);
    return NULL;
}

static void callback(void) {
    fill(200);
    RET(0x12345678, 0);
}

int main(void) {
    recomp_host_init();
    const win32hle_shim shims[] = {{"StateCallback", callback}, {NULL, NULL}};
    win32hle_register(shims);
    mach_enter();
    uint32_t stack = g_esp, tib = g_fs_base;
    fill(100);
    CHECK(hle_call_guest(hle_resolve("StateCallback"), 0, NULL) == 0x12345678);
    expect_outer(stack, tib);
    fill(100);
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL)) return 1;
    hle_block_begin();
    pthread_mutex_lock(&gate);
    phase = 1;
    pthread_cond_signal(&ready);
    while (phase != 2) pthread_cond_wait(&ready, &gate);
    pthread_mutex_unlock(&gate);
    if (pthread_join(thread, NULL)) return 1;
    hle_block_end();
    expect_outer(stack, tib);
    mach_leave();
    mach_enter();
    expect_outer(stack, tib);
    mach_leave();
    return failures ? 1 : 0;
}
