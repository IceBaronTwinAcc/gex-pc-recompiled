#include "win32hle.h"
#include "platform/host.h"

void *gex_host_guest_alloc(uint32_t bytes, uint32_t *address) {
    void *memory = hle_alloc(bytes);
    *address = memory ? hle_va(memory) : 0;
    return memory;
}

void gex_host_guest_free(void *memory) { hle_free(memory); }
