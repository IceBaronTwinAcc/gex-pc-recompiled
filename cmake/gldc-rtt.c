#include <kos.h>
#include "gldc-rtt.h"

int gex_pvr_scene_begin_rtt(void *target, uint32_t width, uint32_t height, uint32_t stride) {
    /* Older KOS renders RTT at the video-mode size, with a caller-owned stride. */
    if (width < (uint32_t)vid_mode->width || height < (uint32_t)vid_mode->height ||
        stride < width)
        return -1;
    pvr_scene_begin_txr(target, &stride, &height);
    return 0;
}
