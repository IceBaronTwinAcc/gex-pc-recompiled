#include <stdio.h>
#include "platform/desktop/audio_limiter.h"
#if defined(__has_include)
#if __has_include(<alext.h>)
#include <alext.h>
#endif
#endif

int gex_openal_limiter_attributes(ALCdevice *device, ALCint *attributes, int *count) {
#ifdef ALC_SOFT_output_limiter
    int limiter = alcIsExtensionPresent(device, "ALC_SOFT_output_limiter");
    if (limiter) {
        attributes[(*count)++] = ALC_OUTPUT_LIMITER_SOFT;
        attributes[(*count)++] = ALC_TRUE;
    } else fprintf(stderr, "[audio] OpenAL output limiter unavailable; overlapping voices may clip\n");
    return limiter;
#else
    (void)device; (void)attributes; (void)count;
    fprintf(stderr, "[audio] OpenAL headers lack output limiter support; overlapping voices may clip\n");
    return 0;
#endif
}

int gex_openal_limiter_enable(ALCdevice *device, int configured) {
#ifdef ALC_SOFT_output_limiter
    if (configured) {
        ALCint enabled = ALC_FALSE;
        alcGetIntegerv(device, ALC_OUTPUT_LIMITER_SOFT, 1, &enabled);
        if (alcGetError(device) != ALC_NO_ERROR || !enabled) {
            fprintf(stderr, "[audio] OpenAL failed to enable the output limiter\n");
            return 0;
        }
        fprintf(stderr, "[audio] OpenAL output limiter enabled\n");
    }
#else
    (void)device; (void)configured;
#endif
    return 1;
}
