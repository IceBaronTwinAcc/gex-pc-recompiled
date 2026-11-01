#include <stdio.h>
#include <string.h>
#include "renderer.h"

int main(void) {
    gex_render_frame frame;
    if (gex_renderer_init(gex_renderer_backend_get(), 0) != -1) return 1;
    if (!strstr(gex_renderer_error(), "--headless")) return 1;
    if (gex_renderer_init(gex_renderer_backend_get(), 1) ||
        gex_renderer_resize(2, 1)) return 1;
    const uint8_t input[] = {31, 0, 0, 0x7C};
    const uint8_t expected[] = {0, 0xF8, 31, 0};
    if (gex_renderer_upload_bgr555(input, 2, 1, 4) ||
        gex_renderer_frame(&frame) || memcmp(frame.pixels, expected, 4) ||
        gex_renderer_pump()) return 1;
    gex_renderer_shutdown();
    return 0;
}
