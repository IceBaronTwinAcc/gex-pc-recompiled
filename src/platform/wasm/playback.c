#include <stdio.h>
#include "platform/audio.h"
#include "platform/runtime.h"
#include "platform/wasm/playback.h"
#include "win32hle.h"

extern int gex_web_audio_playback_ready(void);

int gex_wasm_prepare_playback(void) {
    if (!gex_audio_open()) {
        fprintf(stderr, "[audio] cannot prepare browser playback\n");
        return -1;
    }
    int ready;
    while ((ready = gex_web_audio_playback_ready()) == 0) {
        hle_screen_pump();
        gex_runtime_sleep_us(10000);
    }
    if (ready < 0) {
        fprintf(stderr, "[audio] browser playback context is unavailable\n");
        gex_audio_close();
        return -1;
    }
    return 0;
}
