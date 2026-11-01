#ifndef GEX_GLDC_RTT_H
#define GEX_GLDC_RTT_H
#include <stdint.h>
int gex_pvr_scene_begin_rtt(void *target, uint32_t width, uint32_t height, uint32_t stride);
#define pvr_scene_begin_rtt gex_pvr_scene_begin_rtt
#endif
