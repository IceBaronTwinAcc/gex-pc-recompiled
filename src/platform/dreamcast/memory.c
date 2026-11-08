#include <kos.h>
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
#define VM_BASE 0x01000000u
#define VM_END 0x08000000u
#define VM_MAX_BYTES 0x01000000u
#define VM_REGIONS 32

typedef struct {
    void *memory;
    mmupage_t mapping;
} guest_page;
static struct {
    uint32_t address, bytes;
    guest_page *pages;
} reservations[VM_REGIONS];
static mmucontext_t *context;
static mmu_mapfunc_t previous_mapper;

static int reservation_of(uint32_t address) {
    for (int i = 0; i < VM_REGIONS; ++i)
        if (reservations[i].bytes && address >= reservations[i].address &&
            address - reservations[i].address < reservations[i].bytes) return i;
    return -1;
}
static mmupage_t *map_reserved(mmucontext_t *table, int virtual_page) {
    uint32_t address = (uint32_t)virtual_page << 12;
    int i = reservation_of(address);
    if (i < 0) return previous_mapper ? previous_mapper(table, virtual_page) : NULL;
    guest_page *page = &reservations[i].pages[(address - reservations[i].address) / PAGE_SIZE];
    return page->mapping.valid ? &page->mapping : NULL;
}
static void map_image(void) {
    size_t reserved = (image_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (size_t offset = 0; offset < reserved; offset += PAGE_SIZE) {
        uintptr_t physical = ((uintptr_t)image + offset) & 0x1FFFFFFFu;
        if (mmu_page_map_static(IMAGE_BASE + offset, physical, PAGE_SIZE_64K,
                                MMU_KERNEL_RDWR, true))
            hle_fatal("cannot map guest image page");
    }
}
static void invalidate_mappings(void) {
    irq_disable_scoped();
    mmu_shutdown_basic();
    mmu_init_basic();
    map_image();
    mmu_switch_context(context);
}
static void discard_pages(int i, const uint32_t *selected) {
    unsigned count = reservations[i].bytes / PAGE_SIZE;
    for (unsigned n = 0; n < count; ++n) {
        guest_page *page = &reservations[i].pages[n];
        if (!page->memory || (selected && !(selected[n / 32] & (1u << (n % 32))))) continue;
        dcache_flush_range((uintptr_t)page->memory, PAGE_SIZE);
        page->mapping.valid = 0;
    }
    invalidate_mappings();
    for (unsigned n = 0; n < count; ++n) {
        guest_page *page = &reservations[i].pages[n];
        if (!page->memory || (selected && !(selected[n / 32] & (1u << (n % 32))))) continue;
        free(page->memory);
        memset(page, 0, sizeof *page);
    }
}

static void release_image(void) {
    if (!image) return;
    for (int i = 0; i < VM_REGIONS; ++i) {
        if (!reservations[i].bytes) continue;
        discard_pages(i, NULL);
        free(reservations[i].pages);
        memset(&reservations[i], 0, sizeof reservations[i]);
    }
    mmu_map_set_callback(previous_mapper);
    mmu_use_table(NULL);
    mmu_shutdown();
    mmu_context_destroy(context);
    context = NULL;
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
    void *memory = memalign(address ? PAGE_SIZE : 4096, reserved);
    if (!memory) {
        fprintf(stderr, "[dreamcast] guest allocation failed: %lu bytes\n", (unsigned long)bytes);
        return 0;
    }
    memset(memory, 0, reserved);
    if (!address) return (uint32_t)(uintptr_t)memory;
    context = mmu_context_create(0);
    if (!context) {
        free(memory);
        fprintf(stderr, "[dreamcast] cannot allocate guest page context\n");
        return 0;
    }
    image = memory;
    image_size = bytes;
    dcache_flush_range((uintptr_t)image, reserved);
    mmu_init();
    previous_mapper = mmu_map_set_callback(map_reserved);
    mmu_use_table(context);
    mmu_switch_context(context);
    map_image();
    atexit(release_image);
    fprintf(stderr, "[dreamcast] MMU guest image %08lX -> %p (%lu bytes)\n",
            (unsigned long)address, image, (unsigned long)bytes);
    return address;
}

uint32_t gex_flat_vm_reserve_only(uint32_t address, uint32_t bytes) {
    if (!context || address || !bytes || bytes > VM_MAX_BYTES) {
        fprintf(stderr, "[dreamcast] unsupported guest reservation %08lX + %lu\n",
                (unsigned long)address, (unsigned long)bytes);
        return 0;
    }
    uint32_t reserved = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t base = VM_BASE;
    int slot = -1;
    for (int i = 0; i < VM_REGIONS; ++i)
        if (!reservations[i].bytes) { slot = i; break; }
    for (;;) {
        uint32_t next = base;
        if (reserved > VM_END - base) break;
        for (int i = 0; i < VM_REGIONS; ++i) {
            if (reservations[i].bytes && base < reservations[i].address + reservations[i].bytes &&
                base + reserved > reservations[i].address)
                next = reservations[i].address + reservations[i].bytes > next ?
                    reservations[i].address + reservations[i].bytes : next;
        }
        if (next == base && slot >= 0) {
            guest_page *pages = calloc(reserved / PAGE_SIZE, sizeof *pages);
            if (!pages) break;
            reservations[slot].address = base;
            reservations[slot].bytes = reserved;
            reservations[slot].pages = pages;
            fprintf(stderr, "[dreamcast] reserved guest range %08lX (%lu bytes, uncommitted)\n",
                    (unsigned long)base, (unsigned long)reserved);
            return base;
        }
        if (next == base) break;
        base = next;
    }
    fprintf(stderr, "[dreamcast] cannot reserve %lu guest bytes\n", (unsigned long)bytes);
    return 0;
}
uint32_t gex_flat_vm_commit(uint32_t address, uint32_t bytes) {
    int i = reservation_of(address);
    if (i < 0) return address;
    uint32_t offset = address - reservations[i].address;
    if (!bytes || bytes > reservations[i].bytes - offset) {
        fprintf(stderr, "[dreamcast] guest commit exceeds reservation\n");
        return 0;
    }
    uint32_t added[VM_MAX_BYTES / PAGE_SIZE / 32] = {0};
    unsigned first = offset / PAGE_SIZE, end = (offset + bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (unsigned n = first; n < end; ++n) {
        guest_page *page = &reservations[i].pages[n];
        if (page->memory) continue;
        page->memory = memalign(PAGE_SIZE, PAGE_SIZE);
        if (!page->memory) {
            discard_pages(i, added);
            fprintf(stderr, "[dreamcast] cannot commit guest page at %08lX\n",
                    (unsigned long)(reservations[i].address + n * PAGE_SIZE));
            return 0;
        }
        added[n / 32] |= 1u << (n % 32);
        memset(page->memory, 0, PAGE_SIZE);
        dcache_flush_range((uintptr_t)page->memory, PAGE_SIZE);
        uint32_t physical = (uintptr_t)page->memory & 0x1FFFFFFFu;
        page->mapping.physical = physical >> 12;
        page->mapping.prkey = MMU_KERNEL_RDWR;
        page->mapping.cache = page->mapping.dirty = page->mapping.valid = 1;
        page->mapping.pteh = reservations[i].address + n * PAGE_SIZE;
        /* SH4 PTEL: valid, 64 KiB, kernel read/write, write-back cached, dirty. */
        page->mapping.ptel = physical | 0x1ACu;
    }
    return address;
}
void gex_flat_vm_release(uint32_t address, uint32_t bytes) {
    (void)bytes;
    int i = reservation_of(address);
    if (i >= 0) {
        if (address != reservations[i].address) hle_fatal("guest release is not a reservation base");
        discard_pages(i, NULL);
        free(reservations[i].pages);
        memset(&reservations[i], 0, sizeof reservations[i]);
    } else if (address == IMAGE_BASE && image) release_image();
    else free((void *)(uintptr_t)address);
}

void gex_flat_vm_zero(uint32_t address, uint32_t bytes) {
    int i = reservation_of(address);
    if (i >= 0) {
        uint32_t offset = address - reservations[i].address;
        if (bytes > reservations[i].bytes - offset) hle_fatal("guest decommit exceeds reservation");
        uint32_t removed[VM_MAX_BYTES / PAGE_SIZE / 32] = {0};
        while (bytes) {
            unsigned n = offset / PAGE_SIZE;
            uint32_t within = offset % PAGE_SIZE, take = PAGE_SIZE - within;
            if (take > bytes) take = bytes;
            if (!within && take == PAGE_SIZE) removed[n / 32] |= 1u << (n % 32);
            else if (reservations[i].pages[n].memory)
                memset((uint8_t *)reservations[i].pages[n].memory + within, 0, take);
            bytes -= take; offset += take;
        }
        discard_pages(i, removed);
        return;
    }
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
