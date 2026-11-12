#ifndef GEX_DREAMCAST_MAP_DRAW_H
#define GEX_DREAMCAST_MAP_DRAW_H

#include "packet_gpu.h"

int gex_dreamcast_map_chunk(const gex_gpu_guest_io *io, uint32_t chunk,
                            uint32_t definitions, int32_t x, int32_t y,
                            int width, int height);
int gex_dreamcast_map_flipped_cel(const gex_gpu_guest_io *io,
                                  const gex_gpu_cel_draw *draw, uint32_t *previous_tail);
int gex_dreamcast_map_unflipped_cel(const gex_gpu_guest_io *io,
                                    const gex_gpu_cel_draw *draw, gex_gpu_cel_result *result);
int gex_dreamcast_map_scaled_cel(const gex_gpu_guest_io *io,
                                 const gex_gpu_scaled_cel_draw *draw, uint32_t *last_atlas);

#endif
