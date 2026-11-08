#include <kos.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned fail_allocation;
static void *page_allocate(size_t alignment, size_t bytes) {
    if (fail_allocation && !--fail_allocation) return NULL;
    return memalign(alignment, bytes);
}
#define memalign page_allocate
#include "../src/platform/dreamcast/memory.c"
#undef memalign

KOS_INIT_FLAGS(INIT_DEFAULT);
static unsigned failures;
#define EXPECT(expr) do { if (!(expr)) { printf("VM FAIL line %d: %s\n", __LINE__, #expr); ++failures; } } while (0)
void hle_fatal(const char *format, ...) {
    va_list args;
    va_start(args, format); vfprintf(stderr, format, args); va_end(args);
    abort();
}

int main(void) {
    EXPECT(gex_flat_vm_reserve(IMAGE_BASE, IMAGE_SIZE) == IMAGE_BASE);
    volatile uint32_t *image_words = (void *)(uintptr_t)IMAGE_BASE;
    image_words[0] = 0x12345678u;
    int before = mallinfo().uordblks;
    uint32_t base = gex_flat_vm_reserve_only(0, 4u << 20);
    EXPECT(base && !(base & (PAGE_SIZE - 1)));
    int reserved = mallinfo().uordblks;
    EXPECT(reserved - before < 4096);
    EXPECT(gex_flat_vm_commit(base, PAGE_SIZE) == base);
    int committed = mallinfo().uordblks;
    EXPECT(committed - reserved >= PAGE_SIZE && committed - reserved < PAGE_SIZE + 4096);
    volatile uint32_t *words = (void *)(uintptr_t)base;
    EXPECT(words[0] == 0 && words[PAGE_SIZE / 4 - 1] == 0);
    words[0] = 0x76543210u;
    EXPECT(gex_flat_vm_commit(base, PAGE_SIZE) == base && words[0] == 0x76543210u);
    EXPECT(mallinfo().uordblks == committed);
    EXPECT(!gex_flat_vm_commit(base + (4u << 20) - 4096, 8192));

    fail_allocation = 2;
    EXPECT(!gex_flat_vm_commit(base + PAGE_SIZE, PAGE_SIZE * 2));
    EXPECT(!reservations[reservation_of(base)].pages[1].memory);
    EXPECT(!reservations[reservation_of(base)].pages[2].memory);
    EXPECT(mallinfo().uordblks == committed && words[0] == 0x76543210u);
    EXPECT(image_words[0] == 0x12345678u);

    EXPECT(gex_flat_vm_commit(base, 4u << 20) == base);
    for (unsigned n = 0; n < 64; ++n) {
        volatile uint32_t *page = (void *)(uintptr_t)(base + n * PAGE_SIZE);
        page[0] = n + 1;
        page[PAGE_SIZE / 4 - 1] = n + 65;
    }
    for (unsigned pass = 0; pass < 3; ++pass)
        for (unsigned n = 64; n-- > 0;) {
            volatile uint32_t *page = (void *)(uintptr_t)(base + n * PAGE_SIZE);
            EXPECT(page[0] == n + 1 && page[PAGE_SIZE / 4 - 1] == n + 65);
        }
    EXPECT(image_words[0] == 0x12345678u);
    int full = mallinfo().uordblks;
    gex_flat_vm_zero(base + PAGE_SIZE, PAGE_SIZE);
    EXPECT(full - mallinfo().uordblks >= PAGE_SIZE);
    EXPECT(gex_flat_vm_commit(base + PAGE_SIZE, PAGE_SIZE) == base + PAGE_SIZE);
    EXPECT(words[PAGE_SIZE / 4] == 0 && words[PAGE_SIZE / 2 - 1] == 0);
    EXPECT(words[0] == 1 && words[PAGE_SIZE / 2] == 3);
    words[1024] = 99;
    gex_flat_vm_zero(base + 4096, 4096);
    EXPECT(words[1024] == 0 && words[0] == 1);
    EXPECT(image_words[0] == 0x12345678u);

    gex_flat_vm_release(base, 4u << 20);
    EXPECT(mallinfo().uordblks - before < 4096);
    uint32_t reused = gex_flat_vm_reserve_only(0, 4u << 20);
    EXPECT(reused == base);
    EXPECT(gex_flat_vm_commit(reused, PAGE_SIZE) == reused);
    EXPECT(words[0] == 0 && image_words[0] == 0x12345678u);
    gex_flat_vm_release(reused, 4u << 20);
    uint32_t stack = gex_flat_vm_reserve(0, 131072);
    EXPECT(stack && !(stack & 4095));
    gex_flat_vm_release(stack, 131072);
    gex_flat_vm_release(IMAGE_BASE, IMAGE_SIZE);
    EXPECT(!mmu_enabled());
    printf("Dreamcast guest VM: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
