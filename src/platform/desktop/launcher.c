#include "platform/host.h"

int gex_host_renderer_preference(int renderer, int explicit_renderer) {
    (void)explicit_renderer;
    return renderer;
}

void (*gex_host_game_exit_callback(void))(int code) { return NULL; }
