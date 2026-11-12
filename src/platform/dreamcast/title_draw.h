#ifndef GEX_DREAMCAST_TITLE_DRAW_H
#define GEX_DREAMCAST_TITLE_DRAW_H

#include "packet_gpu.h"
#include "raster.h"

typedef int (*gex_title_image_submit)(void *target, const gex_gpu_vertex vertices[4],
                                      int left, int top, int right, int bottom,
                                      const gex_gpu_image *image, uint64_t image_id,
                                      unsigned rgb, int vertex_tint,
                                      char *error, size_t size);

int gex_dreamcast_title_record(const gex_gpu_guest_io *io, uint32_t object, uint32_t cel,
                               char *error, size_t size);
int gex_dreamcast_title_packet(void *target, uint32_t address, const uint8_t *packet,
                               const gex_raster_state *state, gex_title_image_submit submit,
                               char *error, size_t size);
void gex_dreamcast_title_forget(uint32_t begin, uint32_t end);
void gex_dreamcast_title_reset(void);

#endif
