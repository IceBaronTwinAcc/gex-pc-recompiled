#include "platform/runtime.h"
uint32_t gex_flat_vm_reserve(uint32_t address, uint32_t size) {
    return (uint32_t)(uintptr_t)gex_vm_reserve(address ? (void *)(uintptr_t)(address & ~0xFFFFu) : NULL, size, 1, address != 0);
}
void gex_flat_vm_release(uint32_t address, uint32_t size) { gex_vm_release((void *)(uintptr_t)address, size); }
void gex_flat_vm_zero(uint32_t address, uint32_t size) { gex_vm_discard((void *)(uintptr_t)address, size); }
uint32_t gex_flat_vm_reserve_only(uint32_t address, uint32_t bytes) {
    return gex_flat_vm_reserve(address, bytes);
}
uint32_t gex_flat_vm_commit(uint32_t address, uint32_t bytes) {
    (void)bytes;
    return address;
}
