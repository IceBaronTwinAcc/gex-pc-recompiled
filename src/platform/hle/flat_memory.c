#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include "platform/runtime.h"
#ifdef __EMSCRIPTEN__
/* WebAssembly: one linear memory, and a guest VA is an address in it. There
 * is no mmap at a fixed address and no protection, so a fixed reservation
 * (the PE image at its base) is simply a range below HLE_FIXED_END, where
 * the toolchain puts nothing (the program is linked with GLOBAL_BASE there;
 * everything of its own, data, stack and heap, is above), tracked here so
 * two cannot overlap; a reservation anywhere is a 64 KB-aligned block from
 * malloc. Both are zeroed. */
#ifndef HLE_FIXED_END
#define HLE_FIXED_END 0x01000000u
#endif
#define WASM_GRAN 0x10000u
#define WASM_MAX_RES 1024
static gex_mutex g_res_lock = GEX_MUTEX_INIT;
static struct { uint32_t lo, hi; int fixed; } g_res[WASM_MAX_RES];
static int g_nres;


uint32_t gex_flat_vm_reserve(uint32_t va, uint32_t size) {
    if (!size) return 0;
    uint32_t r = 0;
    gex_mutex_lock(&g_res_lock);
    if (g_nres >= WASM_MAX_RES) { gex_mutex_unlock(&g_res_lock); return 0; }
    if (va) {
        uint64_t lo = va & ~(uint64_t)(WASM_GRAN - 1), hi = ((uint64_t)va + size + WASM_GRAN - 1) & ~(uint64_t)(WASM_GRAN - 1);
        int ok = lo >= WASM_GRAN && hi <= HLE_FIXED_END;
        for (int i = 0; ok && i < g_nres; i++) if (lo < g_res[i].hi && hi > g_res[i].lo) ok = 0;
        if (ok) {
            memset((void *)(uintptr_t)lo, 0, (size_t)(hi - lo));
            g_res[g_nres].lo = (uint32_t)lo, g_res[g_nres].hi = (uint32_t)hi, g_res[g_nres].fixed = 1, g_nres++;
            r = (uint32_t)lo;
        }
    } else {
        size_t n = ((size_t)size + WASM_GRAN - 1) & ~(size_t)(WASM_GRAN - 1);
        void *p = NULL;
        if (posix_memalign(&p, WASM_GRAN, n) == 0 && p) {
            memset(p, 0, n);
            g_res[g_nres].lo = (uint32_t)(uintptr_t)p, g_res[g_nres].hi = (uint32_t)((uintptr_t)p + n), g_res[g_nres].fixed = 0, g_nres++;
            r = (uint32_t)(uintptr_t)p;
        }
    }
    gex_mutex_unlock(&g_res_lock);
    return r;
}
void gex_flat_vm_release(uint32_t va, uint32_t size) {
    (void)size;
    gex_mutex_lock(&g_res_lock);
    for (int i = 0; i < g_nres; i++) {
        if (g_res[i].lo != va) continue;
        if (g_res[i].fixed) memset((void *)(uintptr_t)va, 0, g_res[i].hi - g_res[i].lo);
        else free((void *)(uintptr_t)va);
        g_res[i] = g_res[--g_nres];
        break;
    }
    gex_mutex_unlock(&g_res_lock);
}
void gex_flat_vm_zero(uint32_t va, uint32_t size) { memset((void *)(uintptr_t)va, 0, size); }

#else
uint32_t gex_flat_vm_reserve(uint32_t address, uint32_t size) {
    return (uint32_t)(uintptr_t)gex_vm_reserve(address ? (void *)(uintptr_t)(address & ~0xFFFFu) : NULL, size, 1, address != 0);
}
void gex_flat_vm_release(uint32_t address, uint32_t size) { gex_vm_release((void *)(uintptr_t)address, size); }
void gex_flat_vm_zero(uint32_t address, uint32_t size) { gex_vm_discard((void *)(uintptr_t)address, size); }
#endif
