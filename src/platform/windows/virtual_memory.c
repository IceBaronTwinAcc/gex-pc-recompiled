#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <io.h>
#include <stdio.h>
#include "platform/runtime.h"
#include "win32hle.h"

typedef void *(WINAPI *allocate_fn)(HANDLE, void *, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef void *(WINAPI *map_fn)(HANDLE, HANDLE, void *, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL (WINAPI *unmap_fn)(HANDLE, void *, ULONG);
static allocate_fn allocate;
static map_fn map;
static unmap_fn unmap;
static gex_once once = GEX_ONCE_INIT;
static int failed(const char *operation) {
    fprintf(stderr, "[memory] %s: Windows error %lu\n", operation, GetLastError());
    return -1;
}

static void initialize(void) {
    HMODULE kernel = GetModuleHandleA("kernelbase.dll");
    allocate = (allocate_fn)GetProcAddress(kernel, "VirtualAlloc2");
    map = (map_fn)GetProcAddress(kernel, "MapViewOfFile3");
    unmap = (unmap_fn)GetProcAddress(kernel, "UnmapViewOfFile2");
    if (!allocate || !map || !unmap)
        hle_fatal("the MinGW memory backend requires Windows 10 version 1803 or newer");
}

void *gex_vm_reserve(void *hint, uint64_t bytes, int accessible, int fixed) {
    gex_call_once(&once, initialize);
    if (!bytes || bytes > SIZE_MAX) return NULL;
    void *memory = allocate(GetCurrentProcess(), hint, (size_t)bytes,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (!memory && hint && !fixed)
        memory = allocate(GetCurrentProcess(), NULL, (size_t)bytes,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (!memory) return NULL;
    if ((fixed && memory != hint) ||
        (accessible && gex_vm_replace(memory, bytes, 1))) {
        gex_vm_release(memory, bytes);
        return NULL;
    }
    return memory;
}

/* Isolate exactly this subrange before replacing a placeholder. */
static int isolate(void *address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(address, &info, sizeof info)) return -1;
    if (info.BaseAddress != address &&
        !VirtualFree(address, (uint8_t *)info.BaseAddress + info.RegionSize -
                     (uint8_t *)address, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        return -1;
    if (!VirtualQuery(address, &info, sizeof info)) return -1;
    if (info.RegionSize != bytes &&
        !VirtualFree(address, bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        return -1;
    return 0;
}

static uint8_t *allocation_end(const MEMORY_BASIC_INFORMATION *info) {
    uint8_t *end = (uint8_t *)info->BaseAddress + info->RegionSize;
    MEMORY_BASIC_INFORMATION following;
    while (VirtualQuery(end, &following, sizeof following) &&
           following.AllocationBase == info->AllocationBase)
        end = (uint8_t *)following.BaseAddress + following.RegionSize;
    return end;
}

int gex_vm_replace(void *address, uint64_t bytes, int accessible) {
    if (!bytes || bytes > SIZE_MAX) return -1;
    uint8_t *at = address, *end = at + (size_t)bytes;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(at, &info, sizeof info) || info.State == MEM_FREE) return -1;
        uint8_t *limit = (uint8_t *)info.BaseAddress + info.RegionSize;
        if (limit > end) limit = end;
        size_t length = (size_t)(limit - at);
        if (info.Type == MEM_MAPPED) {
            if (!unmap(GetCurrentProcess(), info.AllocationBase, MEM_PRESERVE_PLACEHOLDER))
                return -1;
            continue;
        }
        if (info.AllocationProtect != PAGE_NOACCESS) {
            uint8_t *whole_end = allocation_end(&info);
            if (!accessible && at == info.AllocationBase && whole_end <= end) {
                if (!VirtualFree(at, whole_end - at, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
                    return -1;
                limit = whole_end;
            } else {
                if (!VirtualFree(at, length, MEM_DECOMMIT)) return -1;
                if (accessible && !VirtualAlloc(at, length, MEM_COMMIT, PAGE_READWRITE))
                    return -1;
            }
        } else if (accessible) {
            if (isolate(at, length) ||
                !allocate(GetCurrentProcess(), at, length,
                    MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                    PAGE_READWRITE, NULL, 0)) return -1;
        }
        at = limit;
    }
    return 0;
}

void gex_vm_release(void *address, uint64_t bytes) {
    uint8_t *at = address, *end = at + (size_t)bytes;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(at, &info, sizeof info) || info.State == MEM_FREE)
            hle_fatal("cannot inspect host reservation (%lu)", GetLastError());
        uint8_t *next = allocation_end(&info);
        if (info.Type == MEM_MAPPED) {
            if (!unmap(GetCurrentProcess(), info.AllocationBase, 0))
                hle_fatal("cannot release host file view (%lu)", GetLastError());
        } else if (!VirtualFree(info.AllocationBase, 0, MEM_RELEASE))
            hle_fatal("cannot release host reservation (%lu)", GetLastError());
        at = next;
    }
}
void gex_vm_discard(void *address, uint64_t bytes) {
    if (gex_vm_replace(address, bytes, 1))
        hle_fatal("cannot discard host pages (%lu)", GetLastError());
}
size_t gex_vm_page_size(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
}
int gex_vm_map_file(void *address, size_t bytes, int file, uint64_t offset,
                    int writable, int copy) {
    size_t page = gex_vm_page_size();
    if (!bytes || bytes > SIZE_MAX - (page - 1)) return -1;
    size_t rounded = (bytes + page - 1) & ~(page - 1);
    uint8_t *at = address, *end = at + rounded;
    unsigned placeholders = 0;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(at, &info, sizeof info) || info.State == MEM_FREE)
            return failed("inspect file mapping reservation");
        uint8_t *next = allocation_end(&info);
        if (info.AllocationProtect != PAGE_NOACCESS) {
            if (info.AllocationBase != at || info.Type != MEM_PRIVATE ||
                !VirtualFree(at, next - at, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
                return failed("restore file mapping placeholder");
        }
        if (next > end) next = end;
        if (isolate(at, next - at)) return failed("split file mapping placeholder");
        ++placeholders;
        at = next;
    }
    if (placeholders > 1 &&
        !VirtualFree(address, rounded, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
        return failed("coalesce file mapping placeholders");
    HANDLE mapping = CreateFileMappingA((HANDLE)_get_osfhandle(file), NULL,
        copy ? PAGE_WRITECOPY : writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, NULL);
    if (!mapping) return failed("create host file mapping");
    void *view = map(mapping, GetCurrentProcess(), address, offset, bytes,
        MEM_REPLACE_PLACEHOLDER,
        copy ? PAGE_WRITECOPY : writable ? PAGE_READWRITE : PAGE_READONLY, NULL, 0);
    int result = view == address ? 0 : failed("replace placeholder with file view");
    CloseHandle(mapping);
    return result;
}
