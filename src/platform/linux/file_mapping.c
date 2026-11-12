#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/mman.h>
#include "platform/runtime.h"

int gex_vm_map_file(void *address, size_t bytes, int file, uint64_t offset,
                    int writable, int copy) {
    return mmap(address, bytes, PROT_READ | (writable ? PROT_WRITE : 0),
                MAP_FIXED | (copy ? MAP_PRIVATE : MAP_SHARED), file, (off_t)offset) == MAP_FAILED ? -1 : 0;
}
