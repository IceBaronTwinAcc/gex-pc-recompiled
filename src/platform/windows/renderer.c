#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "recomp_types.h"
#include "renderer.h"

static int open_renderer(int headless, char *error, size_t size) {
    if (headless) {
        snprintf(error, size, "the native Win32 presenter does not support headless mode");
        return -1;
    }
    return 0;
}

static int pump(char *error, size_t size) { (void)error; (void)size; return 0; }

static int configure(const gex_render_frame *frame, char *error, size_t size) {
    (void)frame; (void)error; (void)size;
    return 0;
}

static int present(const gex_render_frame *frame, char *error, size_t size) {
    HWND window = (HWND)(uintptr_t)MEM32(0x4875A0);
    RECT rect;
    HDC dc = GetDC(window);
    if (!dc || !GetClientRect(window, &rect)) {
        if (dc) ReleaseDC(window, dc);
        snprintf(error, size, "cannot acquire Win32 presentation target (%lu)", GetLastError());
        return -1;
    }
    struct {
        BITMAPINFOHEADER header;
        DWORD masks[3];
    } info = {0};
    info.header.biSize = sizeof info.header;
    info.header.biWidth = frame->width;
    info.header.biHeight = -frame->height;
    info.header.biPlanes = 1;
    info.header.biBitCount = 16;
    info.header.biCompression = BI_BITFIELDS;
    info.masks[0] = 0xF800; info.masks[1] = 0x07E0; info.masks[2] = 0x001F;
    int rows = StretchDIBits(dc, 0, 0, rect.right, rect.bottom, 0, 0,
        frame->width, frame->height, frame->pixels, (const BITMAPINFO *)&info,
        DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(window, dc);
    if (!rows || rows == GDI_ERROR) {
        snprintf(error, size, "Win32 framebuffer presentation failed (%lu)", GetLastError());
        return -1;
    }
    return 0;
}

static void shutdown(void) {}

const gex_renderer_backend *gex_renderer_backend_get(void) {
    static const gex_renderer_backend backend = {open_renderer, configure, present, pump, shutdown};
    return &backend;
}
