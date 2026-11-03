#include "packet_gpu.h"
#include "renderer.h"
#if GEX_ENABLE_GLDC_RENDERER
#include "platform/gl/context.h"
#endif
#if GEX_PROFILE_RENDERER
#include <stdio.h>
#endif

const gex_packet_gpu *gex_packet_gpu_platform_get(void);
static int use_gldc;
static int use_bgfx;

const gex_packet_gpu *gex_packet_gpu_get(void) {
#if GEX_ENABLE_BGFX_RENDERER
    if (use_bgfx)
        return gex_packet_gpu_bgfx();
#endif
#if GEX_ENABLE_GLDC_RENDERER
    if (use_gldc)
        return gex_packet_gpu_gldc();
#endif
    return gex_packet_gpu_platform_get();
}

int gex_packet_gpu_select_bgfx(void) {
#if GEX_ENABLE_BGFX_RENDERER
    use_bgfx = 1;
    use_gldc = 0;
    return 0;
#else
    return -1;
#endif
}

int gex_packet_gpu_select_gldc(int headless) {
#if GEX_ENABLE_GLDC_RENDERER
    gex_gl_set_headless(headless);
    use_gldc = 1;
    use_bgfx = 0;
    return 0;
#else
    (void)headless;
    return -1;
#endif
}

#if GEX_PROFILE_RENDERER
void gex_packet_gpu_profile(const char *name, unsigned draws, unsigned uploads, unsigned resolves,
                            size_t bytes) {
    static uint64_t last, last_cpu;
    static unsigned frame;
    uint64_t cpu, now = gex_packet_gpu_profile_clock(&cpu);
    if (last)
        fprintf(stderr,
                "[gpu-profile] backend=%s f=%u dt=%llu cpu_ns=%llu draws=%u uploads=%u resolves=%u "
                "bytes=%lu\n",
                name, frame, (unsigned long long)(now - last),
                (unsigned long long)(cpu >= last_cpu ? cpu - last_cpu : 0), draws, uploads,
                resolves, (unsigned long)bytes);
    ++frame;
    last = now;
    last_cpu = cpu;
}
#endif
