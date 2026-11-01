#include <stdio.h>
#include "renderer.h"

static int open_renderer(int headless, char *error, size_t size) {
    if (!headless) {
        snprintf(error, size, "the memory-only renderer requires --headless");
        return -1;
    }
    return 0;
}

static int frame(const gex_render_frame *frame, char *error, size_t size) {
    (void)frame; (void)error; (void)size;
    return 0;
}

static int pump(char *error, size_t size) { (void)error; (void)size; return 0; }
static void shutdown(void) {}

const gex_renderer_backend *gex_renderer_backend_get(void) {
    static const gex_renderer_backend backend = {open_renderer, frame, frame, pump, shutdown};
    return &backend;
}
