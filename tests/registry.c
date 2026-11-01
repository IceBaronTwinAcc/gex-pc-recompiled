#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform/filesystem.h"
#include "host.h"
#include "win32hle.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;

int main(int argc, char **argv) {
    if (argc != 3) return 1;
    recomp_host_init();
    if (hle_reg_load(argv[2])) return 2;
    mach_enter();
    uint32_t path = hle_guest_strdup("Software\\GexRecompTest");
    uint32_t name = hle_guest_strdup("TestValue");
    uint32_t slot = hle_va(hle_alloc(4));
    uint32_t create[] = {0x80000001u, path, slot};
    if (hle_call_guest(hle_resolve("RegCreateKeyA"), 3, create)) return 1;
    uint32_t key = MEM32(slot);
    uint32_t data = hle_va(hle_alloc(16));
    uint32_t type = hle_va(hle_alloc(4));
    uint32_t size = hle_va(hle_alloc(4));
    if (!strcmp(argv[1], "write") || !strcmp(argv[1], "fail-write") ||
        !strcmp(argv[1], "fail-replace")) {
        if (!strcmp(argv[1], "fail-replace") && gex_directory_create(argv[2])) return 1;
        const uint8_t bytes[] = {0, 0x7F, 0x80, 0xFF};
        memcpy(hle_ptr(data), bytes, sizeof bytes);
        uint32_t set[] = {key, name, 0, 3, data, sizeof bytes};
        uint32_t error = hle_call_guest(hle_resolve("RegSetValueExA"), 6, set);
        if (strncmp(argv[1], "fail-", 5) == 0) return error == 5 ? 0 : 1;
        if (error) return 1;
        hle_reg_set_dword(0x80000001u, "Software\\GexRecompTest", "Number", 0x87654321u);
        hle_reg_set_string(0x80000001u, "Software\\GexRecompTest", "Label", "Gex test");
        if (hle_reg_flush()) return 1;
    } else if (!strcmp(argv[1], "read")) {
        MEM32(size) = 16;
        uint32_t query[] = {key, name, 0, type, data, size};
        if (hle_call_guest(hle_resolve("RegQueryValueExA"), 6, query)) return 1;
        const uint8_t bytes[] = {0, 0x7F, 0x80, 0xFF};
        if (MEM32(type) != 3 || MEM32(size) != sizeof bytes ||
            memcmp(hle_ptr(data), bytes, sizeof bytes)) return 1;
        query[1] = hle_guest_strdup("Number"); MEM32(size) = 16;
        if (hle_call_guest(hle_resolve("RegQueryValueExA"), 6, query) ||
            MEM32(type) != 4 || MEM32(data) != 0x87654321u) return 1;
        query[1] = hle_guest_strdup("Label"); MEM32(size) = 16;
        if (hle_call_guest(hle_resolve("RegQueryValueExA"), 6, query) ||
            MEM32(type) != 1 || strcmp(hle_ptr(data), "Gex test")) return 1;
    } else if (strcmp(argv[1], "load")) return 1;
    mach_leave();
    return 0;
}
