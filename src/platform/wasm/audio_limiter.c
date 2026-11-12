#include <stdio.h>
#include "platform/desktop/audio_limiter.h"

extern int gex_web_audio_enable_limiter(void);

int gex_openal_limiter_attributes(ALCdevice *device, ALCint *attributes, int *count) {
    (void)device; (void)attributes; (void)count;
    return 0;
}
int gex_openal_limiter_enable(ALCdevice *device, int configured) {
    (void)device; (void)configured;
    if (!gex_web_audio_enable_limiter()) {
        fprintf(stderr, "[audio] cannot initialize the browser output limiter\n");
        return 0;
    }
    fprintf(stderr, "[audio] browser output limiter enabled\n");
    return 1;
}
