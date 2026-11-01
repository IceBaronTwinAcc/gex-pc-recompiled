#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "win32hle.h"

ptrdiff_t g_mem_base;

int main(void) {
    uint8_t memory[128] = {0};
    const uint32_t base = 0x400000;
    g_mem_base = (ptrdiff_t)((uintptr_t)memory - base);
    assert(ADDR(base) == (uintptr_t)memory);
    MEM16(base + 1) = 0x1234;
    MEM32(base + 3) = 0x89ABCDEF;
    MEM64(base + 7) = UINT64_C(0xFEDCBA9876543210);
    MEMF(base + 17) = 1.25f;
    MEMD(base + 23) = 3.5;
    assert(MEM16(base + 1) == 0x1234);
    assert(MEM32(base + 3) == 0x89ABCDEF);
    assert(MEM64(base + 7) == UINT64_C(0xFEDCBA9876543210));
    assert(MEMF(base + 17) == 1.25f);
    assert(MEMD(base + 23) == 3.5);
    assert(memory[1] == 0x34 && memory[2] == 0x12);
    return 0;
}
