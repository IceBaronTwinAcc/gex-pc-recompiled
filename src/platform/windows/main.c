#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native32.h"
#include "identity.h"
#include "renderer.h"
#include "graphics_guest.h"

static char command[128];
static int render_width, render_height;

static void command_line(void) {
    g_eax = (uint32_t)(uintptr_t)command;
    g_esp += 4;
}

static void module_handle(void) {
    const char *name = (const char *)(uintptr_t)MEM32(g_esp + 4);
    g_eax = (!name || !_stricmp(name, "GEX.exe")) ? 0x00400000u :
        (uint32_t)(uintptr_t)GetModuleHandleA(name);
    g_esp += 8;
}

static void module_filename(void) {
    HMODULE module = (HMODULE)(uintptr_t)MEM32(g_esp + 4);
    char *buffer = (char *)(uintptr_t)MEM32(g_esp + 8);
    DWORD size = MEM32(g_esp + 12);
    if ((uintptr_t)module == 0x00400000u || !module) {
        char path[MAX_PATH];
        DWORD length = GetFullPathNameA("GEX.exe", sizeof path, path, NULL);
        if (!length || length >= sizeof path) g_eax = 0;
        else {
            if (size) {
                size_t n = length < size ? length : size - 1;
                memcpy(buffer, path, n);
                buffer[n] = 0;
            }
            g_eax = length < size ? length : size;
            if (length >= size) SetLastError(ERROR_INSUFFICIENT_BUFFER);
        }
    } else g_eax = GetModuleFileNameA(module, buffer, size);
    g_esp += 16;
}

static native32_shim_t shims[] = {
    {"GetCommandLineA", command_line, 0},
    {"GetModuleHandleA", module_handle, 0},
    {"GetModuleFileNameA", module_filename, 0},
    {"DirectDrawCreate", gex_graphics_forbidden, 0},
};

recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}

static DWORD WINAPI run_game(void *unused) {
    (void)unused;
    native32_init();
    if (gex_renderer_init(gex_renderer_backend_get(), 0) || gex_renderer_resize(320, 240)) {
        fprintf(stderr, "[renderer] %s\n", gex_renderer_error());
        return 1;
    }
    if (render_width && gex_graphics_set_render_size(render_width, render_height)) return 1;
    if (!native32_map("GEX.exe", 0x00400000u)) return 1;
    snprintf(command, sizeof command, "GEX.exe %s", (const char *)(uintptr_t)0x00454FD0u);
    if (native32_bind(0x00400000u, shims, sizeof shims / sizeof shims[0])) return 2;
    native32_call_guest(0x00449BD0u, 0, NULL);
    gex_graphics_shutdown();
    gex_renderer_shutdown();
    return g_eax;
}

int main(int argc, char **argv) {
    const char *data = "Gex";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--render-size")) {
            if (i + 1 == argc || gex_renderer_parse_size(argv[++i], &render_width, &render_height)) {
                fprintf(stderr, "--render-size requires WIDTHxHEIGHT: 1..8192 per axis, at most 16777216 pixels\n");
                return 64;
            }
        }
        else {
            fprintf(stderr, "Usage: %s [--data GOG-directory] [--render-size WIDTHxHEIGHT]\n", argv[0]);
            return !strcmp(argv[i], "--help") ? 0 : 64;
        }
    }
    char path[MAX_PATH];
    DWORD length = GetFullPathNameA(data, sizeof path, path, NULL);
    if (!length || length >= sizeof path - 9) {
        fprintf(stderr, "Cannot resolve game-data directory\n");
        return 1;
    }
    strcat(path, "\\GEX.exe");
    if (gex_verify_executable(path)) return 1;
    native32_hold_image(path, 0x00400000u);
    if (!SetCurrentDirectoryA(data)) {
        fprintf(stderr, "Cannot enter %s (Windows error %lu)\n", data, GetLastError());
        return 1;
    }
    HANDLE thread = CreateThread(NULL, 8u << 20, run_game, NULL, 0, NULL);
    if (!thread) return 1;
    WaitForSingleObject(thread, INFINITE);
    DWORD code;
    if (!GetExitCodeThread(thread, &code)) code = 1;
    CloseHandle(thread);
    return (int)code;
}
