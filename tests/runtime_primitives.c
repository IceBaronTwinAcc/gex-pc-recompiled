#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "win32hle.h"
#include "platform/runtime.h"
#include "platform/filesystem.h"
#include "platform/font.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;
static int failures;
#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

static gex_mutex counter_lock = GEX_MUTEX_INIT;
static gex_once once = GEX_ONCE_INIT;
static unsigned counter;
static atomic_uint initialized, finished, guest_started;
static uint32_t exit_thread;

static void initialize(void) { atomic_fetch_add(&initialized, 1); }
static void *worker(void *unused) {
    (void)unused;
    gex_call_once(&once, initialize);
    for (unsigned i = 0; i < 1000; ++i) {
        gex_mutex_lock(&counter_lock);
        ++counter;
        gex_mutex_unlock(&counter_lock);
    }
    atomic_fetch_add(&finished, 1);
    return NULL;
}

static void test_host_primitives(void) {
    CHECK(gex_fs_path_absolute("/game/data"));
    CHECK(gex_fs_path_absolute("C:\\game\\data"));
    CHECK(gex_fs_path_absolute("D:/game/data"));
    CHECK(!gex_fs_path_absolute("data/GEX.exe"));
    CHECK(!gex_fs_path_absolute("C:relative"));
    CHECK(!gex_fs_path_absolute(""));
    for (unsigned i = 0; i < 4; ++i) CHECK(!gex_runtime_start_thread(worker, NULL));
    uint64_t deadline = gex_runtime_time_us() + 2000000;
    while (atomic_load(&finished) < 4 && gex_runtime_time_us() < deadline)
        gex_runtime_sleep_us(1000);
    CHECK(atomic_load(&finished) == 4 && counter == 4000);
    CHECK(atomic_load(&initialized) == 1);
    size_t page = gex_vm_page_size();
    uint8_t *memory = gex_vm_reserve(NULL, page * 3, 1, 0);
    CHECK(memory);
    if (memory) {
        memset(memory, 0xA5, page * 3);
        CHECK(!gex_vm_reserve(memory, page, 1, 1));
        CHECK(memory[0] == 0xA5);
        CHECK(!gex_vm_replace(memory + page, page, 1));
        CHECK(memory[page] == 0 && memory[page - 1] == 0xA5 && memory[page * 2] == 0xA5);
        gex_vm_release(memory, page * 3);
    }
    struct tm calendar;
    gex_runtime_calendar(946684800, 0, &calendar);
    CHECK(calendar.tm_year == 100 && calendar.tm_mon == 0 && calendar.tm_mday == 1);
    CHECK(gex_runtime_utc_seconds(&calendar) == 946684800);
}

static void guest_entry(void) {
    atomic_fetch_add(&guest_started, 1);
    CHECK(A32(0) == 42);
    RET(0xBEEF, 1);
}

static void guest_exit(void) {
    uint32_t code = 123;
    hle_call_guest(exit_thread, 1, &code);
    CHECK(0);
    RET(0, 1);
}

static uint32_t call(const char *name, unsigned count, const uint32_t *args) {
    uint32_t function = hle_resolve(name);
    CHECK(function);
    return hle_call_guest(function, count, args);
}

static void test_shared_dib_sections(void) {
    uint32_t *header = hle_alloc(40);
    uint32_t *bits = hle_alloc(4);
    CHECK(header && bits);
    if (!header || !bits) return;
    header[0] = 40;
    header[1] = 8;
    header[2] = (uint32_t)-8;
    header[3] = 1 | (16u << 16);
    uint32_t create[] = {UINT32_MAX, 0, 4, 0, 132, 0};
    uint32_t mapping = call("CreateFileMappingA", 6, create);
    CHECK(mapping);
    uint32_t args[] = {0, hle_va(header), 0, hle_va(bits), mapping, 0};
    uint32_t first = call("CreateDIBSection", 6, args), first_bits = *bits;
    args[5] = 4;
    uint32_t second = call("CreateDIBSection", 6, args), second_bits = *bits;
    CHECK(first && second && first != second && second_bits == first_bits + 4);
    if (first_bits && second_bits) {
        MEM16(first_bits + 4) = 0x1234;
        CHECK(MEM16(second_bits) == 0x1234);
    }
    args[5] = 8;
    CHECK(!call("CreateDIBSection", 6, args) && !*bits);
    CHECK(call("CloseHandle", 1, &mapping));
    CHECK(call("DeleteObject", 1, &first));
    if (second_bits) CHECK(MEM16(second_bits) == 0x1234);
    CHECK(call("DeleteObject", 1, &second));
    hle_free(header);
    hle_free(bits);
}

static void test_guest_threads(void) {
    uint32_t slot = hle_va(hle_alloc(4));
    uint32_t args[] = {0, 0, hle_resolve("PrimitiveThread"), 42, 4, slot};
    uint32_t thread = call("CreateThread", 6, args);
    CHECK(thread && MEM32(slot));
    uint32_t wait[] = {thread, 0};
    CHECK(call("WaitForSingleObject", 2, wait) == 258);
    CHECK(!atomic_load(&guest_started));
    CHECK(call("ResumeThread", 1, &thread) == 1);
    wait[1] = 1000;
    CHECK(!call("WaitForSingleObject", 2, wait));
    uint32_t code[] = {thread, slot};
    CHECK(call("GetExitCodeThread", 2, code) && MEM32(slot) == 0xBEEF);
    CHECK(atomic_load(&guest_started) == 1);
    CHECK(call("CloseHandle", 1, &thread));

    args[2] = hle_resolve("PrimitiveExit");
    args[4] = 0;
    thread = call("CreateThread", 6, args);
    wait[0] = thread;
    CHECK(thread && !call("WaitForSingleObject", 2, wait));
    code[0] = thread;
    CHECK(call("GetExitCodeThread", 2, code) && MEM32(slot) == 123);
    CHECK(call("CloseHandle", 1, &thread));
    hle_free(hle_ptr(slot));
}

static void test_guest_events_and_atomics(void) {
    uint32_t create[] = {0, 0, 1, 0};
    uint32_t event = call("CreateEventA", 4, create);
    uint32_t wait[] = {event, 0};
    CHECK(event && !call("WaitForSingleObject", 2, wait));
    CHECK(call("WaitForSingleObject", 2, wait) == 258);
    CHECK(call("SetEvent", 1, &event));
    CHECK(!call("WaitForSingleObject", 2, wait));
    CHECK(call("CloseHandle", 1, &event));
    uint32_t slot = hle_va(hle_alloc(4));
    MEM32(slot) = UINT32_MAX;
    CHECK(call("InterlockedIncrement", 1, &slot) == 0 && MEM32(slot) == 0);
    CHECK(call("InterlockedDecrement", 1, &slot) == UINT32_MAX);
    uint32_t args[] = {slot, 5, 1};
    CHECK(call("InterlockedExchange", 2, args) == UINT32_MAX && MEM32(slot) == 5);
    CHECK(call("InterlockedExchangeAdd", 2, args) == 5 && MEM32(slot) == 10);
    CHECK(call("InterlockedCompareExchange", 3, args) == 10 && MEM32(slot) == 10);
    args[2] = 10;
    CHECK(call("InterlockedCompareExchange", 3, args) == 10 && MEM32(slot) == 5);
    hle_free(hle_ptr(slot));
}

static void test_files(const char *directory) {
    hle_set_drive('T', directory);
    uint32_t name = hle_guest_strdup("T:\\MiXeD.bin");
    uint32_t args[] = {name, 0xC0000000u, 0, 0, 2, 0, 0};
    uint32_t file = call("CreateFileA", 7, args);
    CHECK(file != UINT32_MAX);
    uint32_t data = hle_va(hle_alloc(8192)), count = hle_va(hle_alloc(4));
    memset(hle_ptr(data), 0x5A, 8192);
    uint32_t io[] = {file, data, 8192, count, 0};
    CHECK(call("WriteFile", 5, io) && MEM32(count) == 8192);
    uint32_t timestamp = hle_va(hle_alloc(8));
    uint64_t filetime = (946684800ull + 11644473600ull) * 10000000ull;
    MEM32(timestamp) = (uint32_t)filetime;
    MEM32(timestamp + 4) = (uint32_t)(filetime >> 32);
    uint32_t set_time[] = {file, 0, timestamp, timestamp};
    CHECK(call("SetFileTime", 4, set_time));
    uint32_t get_time[] = {file, 0, 0, timestamp};
    CHECK(call("GetFileTime", 4, get_time));
    CHECK(MEM32(timestamp) == (uint32_t)filetime &&
          MEM32(timestamp + 4) == (uint32_t)(filetime >> 32));
    hle_free(hle_ptr(timestamp));
    uint32_t mapping_args[] = {file, 0, 4, 0, 0, 0};
    uint32_t mapping = call("CreateFileMappingA", 6, mapping_args);
    uint32_t view_args[] = {mapping, 2, 0, 0, 8192};
    uint32_t view = call("MapViewOfFile", 5, view_args);
    CHECK(mapping && view);
    if (view) {
        CHECK(MEM8(view + 4096) == 0x5A);
        MEM8(view) = 0x31;
        CHECK(call("UnmapViewOfFile", 1, &view));
    }
    CHECK(call("CloseHandle", 1, &mapping));
    CHECK(call("CloseHandle", 1, &file));
    hle_free(hle_ptr(name));
    name = hle_guest_strdup("T:\\mixed.BIN");
    args[0] = name; args[1] = 0x80000000u; args[4] = 3;
    file = call("CreateFileA", 7, args);
    io[0] = file;
    CHECK(call("ReadFile", 5, io) && MEM32(count) == 8192 && MEM8(data) == 0x31);
    CHECK(call("CloseHandle", 1, &file));
    hle_free(hle_ptr(name));
    name = hle_guest_strdup("T:\\*.BIN");
    uint32_t find_data = hle_va(hle_alloc(320)), find_args[] = {name, find_data};
    uint32_t find = call("FindFirstFileA", 2, find_args);
    CHECK(find != UINT32_MAX && !strcmp(hle_ptr(find_data + 44), "MiXeD.bin"));
    CHECK(call("FindClose", 1, &find));
    hle_free(hle_ptr(name));
    hle_free(hle_ptr(data)); hle_free(hle_ptr(count)); hle_free(hle_ptr(find_data));

    char path[1024];
    snprintf(path, sizeof path, "%s/MiXeD.bin", directory);
    gex_file_info info;
    CHECK(!gex_path_stat(path, &info) && info.size == 8192 && !info.directory);
    CHECK(!gex_file_remove(path));
    CHECK(gex_path_stat(path, &info) == -1 && gex_fs_error() == GEX_IO_MISSING);
}

static void test_guest_memory(void) {
    uint32_t memory = hle_vm_reserve(0, 32768);
    CHECK(memory);
    if (!memory) return;
    memset(hle_ptr(memory), 0xA5, 32768);
    hle_vm_zero(memory + 123, 16384);
    CHECK(MEM8(memory + 122) == 0xA5 && MEM8(memory + 16507) == 0xA5);
    for (uint32_t i = 123; i < 16507; ++i) CHECK(!MEM8(memory + i));
    CHECK(!hle_vm_reserve(memory, 32768));
    hle_vm_release(memory, 32768);
    CHECK(hle_vm_reserve(memory, 32768) == memory && !MEM8(memory));
    hle_vm_release(memory, 32768);
    uint8_t *block = hle_heap_alloc(32, 1);
    CHECK(block);
    if (!block) return;
    memset(block, 0x31, 32);
    block = hle_heap_realloc(block, 8192, 1);
    CHECK(block);
    if (!block) return;
    CHECK(block[0] == 0x31 && block[31] == 0x31 && block[32] == 0 && block[8191] == 0);
    hle_heap_free(block);
}

static void test_file_views(const char *directory) {
    char path[1024];
    snprintf(path, sizeof path, "%s/views.bin", directory);
    int file = gex_file_open(path, GEX_FILE_READ | GEX_FILE_WRITE | GEX_FILE_CREATE |
                                  GEX_FILE_EXCLUSIVE);
    CHECK(file >= 0);
    if (file < 0) return;
    CHECK(!gex_file_truncate(file, 131072));
    uint32_t whole = hle_vm_reserve(0, 131072);
    CHECK(whole);
    if (whole) {
        hle_vm_release(whole, 131072);
        CHECK(hle_vm_reserve(whole, 65536) == whole);
        CHECK(hle_vm_reserve(whole + 65536, 65536) == whole + 65536);
        hle_vm_release(whole, 65536);
        hle_vm_release(whole + 65536, 65536);
        CHECK(hle_vm_reserve(whole, 131072) == whole);
        int mapped = !gex_vm_map_file(hle_ptr(whole), 131072, file, 0, 1, 0);
        CHECK(mapped);
        if (mapped) {
            MEM8(whole) = 0x31;
            MEM8(whole + 65536) = 0x42;
        }
        hle_vm_release(whole, 131072);
        CHECK(hle_vm_reserve(whole, 65536) == whole);
        mapped = !gex_vm_map_file(hle_ptr(whole), 5003, file, 0, 1, 1);
        CHECK(mapped);
        if (mapped) {
            CHECK(MEM8(whole) == 0x31);
            MEM8(whole) = 0x55;
        }
        hle_vm_release(whole, 65536);
        unsigned char value = 0;
        CHECK(gex_file_seek(file, 0, SEEK_SET) == 0);
        CHECK(gex_file_read(file, &value, 1) == 1 && value == 0x31);
        CHECK(hle_vm_reserve(whole, 65536) == whole);
        mapped = !gex_vm_map_file(hle_ptr(whole), 4096, file, 65536, 0, 0);
        CHECK(mapped);
        if (mapped) CHECK(MEM8(whole) == 0x42);
        hle_vm_release(whole, 65536);
    }
    CHECK(!gex_file_close(file));
    hle_path_changed();
    char *canonical = gex_fs_canonical_path(directory);
    CHECK(canonical);
    if (canonical) {
        char guest[1024], host[1024];
        hle_guest_path(canonical, guest, sizeof guest);
        CHECK(hle_host_path(guest, host, sizeof host));
        CHECK(!gex_string_compare_case(canonical, host));
        size_t used = strlen(guest);
        CHECK(snprintf(guest + used, sizeof guest - used, "\\unused\\..\\views.bin") <
              (int)(sizeof guest - used));
        CHECK(hle_host_path(guest, host, sizeof host));
        gex_file_info info;
        CHECK(!gex_path_stat(host, &info) && info.size == 131072);
        free(canonical);
    }
    CHECK(!gex_file_remove(path));
}

static void test_font(void) {
    CHECK(gex_font_initialize());
    const char *path = gex_font_locate("Liberation Sans", 0, 0);
    CHECK(path);
    if (!path) return;
    gex_font *font = gex_font_open(path, NULL, 0, 14);
    CHECK(font);
    if (!font) return;
    gex_font_configure(font, 1, 0, 0);
    for (int bold = 0; bold <= 1; ++bold) {
        int width, height;
        uint8_t *pixels = gex_font_coverage(font, "Gex", 3, 1, bold, &width, &height);
        CHECK(pixels && width > 0 && height > 0);
        unsigned ink = 0;
        if (pixels) for (size_t i = 0; i < (size_t)width * height; ++i) ink |= pixels[i];
        CHECK(ink);
        free(pixels);
    }
    gex_font_close(font);
}

int main(void) {
    const char *temporary = getenv("TMPDIR");
    if (!temporary) temporary = getenv("TEMP");
    if (!temporary) temporary = ".";
    char directory[1024];
    if (snprintf(directory, sizeof directory, "%s/gex-runtime-XXXXXX", temporary) >=
        (int)sizeof directory) return 1;
    if (!mkdtemp(directory)) return 1;
    test_host_primitives();
    recomp_host_init();
    CHECK(!hle_resolve("WSAStartup") && !hle_resolve("socket"));
    const win32hle_shim shims[] = {
        {"PrimitiveThread", guest_entry}, {"PrimitiveExit", guest_exit}, {NULL, NULL}
    };
    win32hle_register(shims);
    exit_thread = hle_resolve("ExitThread");
    mach_enter();
    test_guest_threads();
    test_guest_events_and_atomics();
    test_guest_memory();
    test_shared_dib_sections();
    test_files(directory);
    test_file_views(directory);
    mach_leave();
    test_font();
    CHECK(!gex_directory_remove(directory));
    return failures ? 1 : 0;
}
