#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"
#include "win32hle.h"
#include "platform/runtime.h"

void gex_guest_wait(void) {
    hle_block_begin();
    gex_runtime_sleep_us(1000);
    hle_block_end();
}

recomp_func_t gex_lookup_import(uint32_t va) {
    recomp_func_t fn = recomp_lookup_import(va);
    if (!fn && va != RECOMP_RETADDR) {
        fprintf(stderr, "[gex] unresolved guest transfer 0x%08X -> 0x%08X\n", g_cur_func, va);
        abort();
    }
    return fn;
}
