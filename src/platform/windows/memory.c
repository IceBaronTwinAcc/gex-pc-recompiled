#include <stdlib.h>
#include "platform/host.h"

void *gex_host_guest_alloc(uint32_t bytes, uint32_t *address) {
    void *memory = calloc(1, bytes);
    *address = (uint32_t)(uintptr_t)memory;
    return memory;
}

void gex_host_guest_free(void *memory) { free(memory); }
