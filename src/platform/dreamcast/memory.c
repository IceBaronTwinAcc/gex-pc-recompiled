#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arch/mmu.h>
#include <arch/cache.h>
#include "win32hle.h"
#include "platform/runtime.h"

static void *image;
static uint32_t image_size;
#define IMAGE_BASE 0x400000u
#define IMAGE_SIZE 0x100000u
#define PAGE_SIZE 0x10000u

static void release_image(void) {
    if (!image) return;
    mmu_shutdown_basic();
    free(image);
    image = NULL;
    image_size = 0;
}

uint32_t gex_flat_vm_reserve(uint32_t address, uint32_t bytes) {
    if (!bytes) return 0;
    if (address && (address != IMAGE_BASE || bytes > IMAGE_SIZE || image || mmu_enabled())) {
        fprintf(stderr, "[dreamcast] unsupported fixed reservation %08lX + %lu\n",
                (unsigned long)address, (unsigned long)bytes);
        return 0;
    }
    size_t reserved = address ? (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1) : bytes;
    void *memory = memalign(address ? PAGE_SIZE : 32, reserved);
    if (!memory) {
        fprintf(stderr, "[dreamcast] guest allocation failed: %lu bytes\n", (unsigned long)bytes);
        return 0;
    }
    memset(memory, 0, reserved);
    if (!address) return (uint32_t)(uintptr_t)memory;
    image = memory;
    image_size = bytes;
    dcache_flush_range((uintptr_t)image, reserved);
    mmu_init_basic();
    for (size_t offset = 0; offset < reserved; offset += PAGE_SIZE) {
        uintptr_t physical = ((uintptr_t)image + offset) & 0x1FFFFFFFu;
        if (mmu_page_map_static(address + offset, physical, PAGE_SIZE_64K,
                                MMU_KERNEL_RDWR, true)) {
            release_image();
            return 0;
        }
    }
    atexit(release_image);
    fprintf(stderr, "[dreamcast] MMU guest image %08lX -> %p (%lu bytes)\n",
            (unsigned long)address, image, (unsigned long)bytes);
    return address;
}

void gex_flat_vm_release(uint32_t address, uint32_t bytes) {
    (void)bytes;
    if (address == IMAGE_BASE && image) release_image();
    else free((void *)(uintptr_t)address);
}

void gex_flat_vm_zero(uint32_t address, uint32_t bytes) {
    if (address >= IMAGE_BASE && address - IMAGE_BASE < image_size &&
        bytes > image_size - (address - IMAGE_BASE))
        hle_fatal("image decommit exceeds reservation");
    memset(hle_ptr(address), 0, bytes);
}

void *gex_vm_reserve(void *hint, uint64_t bytes, int accessible, int fixed) {
    (void)accessible;
    if (fixed || hint || bytes > SIZE_MAX) {
        fprintf(stderr, "[dreamcast] host fixed virtual memory is unavailable\n");
        return NULL;
    }
    return calloc(1, (size_t)bytes);
}
int gex_vm_replace(void *address, uint64_t bytes, int accessible) {
    (void)accessible;
    if (bytes > SIZE_MAX) return -1;
    memset(address, 0, (size_t)bytes);
    return 0;
}
void gex_vm_release(void *address, uint64_t bytes) { (void)bytes; free(address); }
void gex_vm_discard(void *address, uint64_t bytes) {
    if (bytes > SIZE_MAX) hle_fatal("host discard exceeds address space");
    memset(address, 0, (size_t)bytes);
}
size_t gex_vm_page_size(void) { return 4096; }
int gex_vm_map_file(void *address, size_t bytes, int file, uint64_t offset,
                    int writable, int copy) {
    (void)address; (void)bytes; (void)file; (void)offset; (void)writable; (void)copy;
    fprintf(stderr, "[dreamcast] file-backed virtual memory is unavailable\n");
    return -1;
}
