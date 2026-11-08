#include <emscripten/html5.h>
#include "platform/wasm/fullscreen.h"

int gex_wasm_set_fullscreen(int enabled) {
    EMSCRIPTEN_RESULT result;
    if (enabled) {
        /* The page's fullscreen CSS owns layout; SDL's software surface stays windowed. */
        result = emscripten_request_fullscreen("#canvas", EM_TRUE);
    } else {
        result = emscripten_exit_fullscreen();
    }
    return result == EMSCRIPTEN_RESULT_SUCCESS || result == EMSCRIPTEN_RESULT_DEFERRED ?
        0 : result;
}
