#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "platform/runtime.h"
#include "win32hle.h"

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

void *gex_vm_reserve(void *hint, uint64_t bytes, int accessible, int fixed) {
    if (bytes > SIZE_MAX) return NULL;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | (accessible ? 0 : MAP_NORESERVE);
#ifdef MAP_FIXED_NOREPLACE
    if (fixed) flags |= MAP_FIXED_NOREPLACE;
#endif
    void *memory = mmap(hint, (size_t)bytes, accessible ? PROT_READ | PROT_WRITE : PROT_NONE,
                         flags, -1, 0);
    if (memory == MAP_FAILED) return NULL;
    if (fixed && memory != hint) {
        gex_vm_release(memory, bytes);
        return NULL;
    }
    return memory;
}

int gex_vm_replace(void *address, uint64_t bytes, int accessible) {
    if (bytes > SIZE_MAX) return -1;
    void *memory = mmap(address, (size_t)bytes, accessible ? PROT_READ | PROT_WRITE : PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED |
                        (accessible ? 0 : MAP_NORESERVE), -1, 0);
    return memory == address ? 0 : -1;
}

void gex_vm_release(void *address, uint64_t bytes) {
    if (bytes > SIZE_MAX || munmap(address, (size_t)bytes))
        hle_fatal("release host reservation: %s", strerror(errno));
}

void gex_vm_discard(void *address, uint64_t bytes) {
    if (bytes > SIZE_MAX || madvise(address, (size_t)bytes, MADV_DONTNEED))
        hle_fatal("discard host pages: %s", strerror(errno));
}

size_t gex_vm_page_size(void) {
    long size = sysconf(_SC_PAGESIZE);
    if (size <= 0) hle_fatal("cannot determine host page size");
    return (size_t)size;
}
