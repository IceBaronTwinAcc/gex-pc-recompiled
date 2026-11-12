#include "platform/bgfx/present_policy.h"
#include "packet_gpu.h"

void gex_bgfx_window_size(SDL_Window *window, int initial_width, int initial_height,
                          int *width, int *height) {
    (void)window;
    *width = initial_width; *height = initial_height;
}
void gex_bgfx_before_window_close(void) {}
void gex_bgfx_after_window_close(void) { gex_packet_gpu_shutdown_bgfx(); }
