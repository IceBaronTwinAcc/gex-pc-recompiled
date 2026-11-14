#ifndef GEX_DREAMCAST_MAP_DRAW_H
#define GEX_DREAMCAST_MAP_DRAW_H

#include "packet_gpu.h"
#include "map_plan.h"

/* Two 40-byte quads, two 12-byte page commands and a 16-byte palette upload
 * per tile bound every allocation the original chunk could make. */
enum { GEX_DREAMCAST_MAP_CHUNK_PACKET_BOUND = GEX_MAP_TILE_LIMIT * (80 + 24 + 16) };
enum { GEX_DREAMCAST_MAP_CEL_PACKET_BOUND = 128 * (80 + 24),
       GEX_DREAMCAST_MAP_SCALED_CEL_PACKET_BOUND = 128 * 80 };
enum { GEX_DREAMCAST_BACKDROP_PACKET_BOUND = 40 * 40 };

int gex_dreamcast_map_chunk(const gex_gpu_guest_io *io, uint32_t chunk,
                            uint32_t definitions, int32_t x, int32_t y,
                            int width, int height);
int gex_dreamcast_map_chunk_direct(const gex_gpu_guest_io *io, uint32_t chunk,
                                   uint32_t definitions, int32_t x, int32_t y,
                                   int width, int height);
int gex_dreamcast_backdrop_tiles_direct(const gex_gpu_guest_io *io, uint32_t location,
                                        uint16_t palette, const uint32_t roots[2]);
int gex_dreamcast_map_flipped_cel(const gex_gpu_guest_io *io,
                                  const gex_gpu_cel_draw *draw, uint32_t *previous_tail);
int gex_dreamcast_map_cel_direct(const gex_gpu_guest_io *io,
                                 const gex_gpu_cel_draw *draw, const uint32_t roots[2]);
int gex_dreamcast_map_unflipped_cel(const gex_gpu_guest_io *io,
                                    const gex_gpu_cel_draw *draw, gex_gpu_cel_result *result);
int gex_dreamcast_map_scaled_cel(const gex_gpu_guest_io *io,
                                 const gex_gpu_scaled_cel_draw *draw, uint32_t *last_atlas);
int gex_dreamcast_map_scaled_cel_direct(const gex_gpu_guest_io *io,
                                        const gex_gpu_scaled_cel_draw *draw,
                                        const uint32_t roots[2], uint32_t *last_atlas);

#endif
