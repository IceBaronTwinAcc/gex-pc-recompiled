#ifndef GEX_RASTER_H
#define GEX_RASTER_H

#include <stdint.h>
#include "packet_gpu.h"

typedef struct gex_raster gex_raster;

typedef struct gex_raster_state {
    const uint8_t *atlas; /* Original 1024x512 BGR555/packed-index texture atlas. */
    const uint8_t *modulation; /* Original 32x32 component multiplication table. */
    unsigned page;
    int left, top, right, bottom;
    int origin_y;
    int layout; /* 0 = world pixels, 1 = centered UI, 2 = tiled 320x224 backdrop. */
    int pixel_scale, offset_x; /* Replicate rasterized HUD pixels; zero means 1x. */
    int world_clip, world_left, world_top, world_right, world_bottom;
} gex_raster_state;

gex_raster *gex_raster_create(int width, int height);
int gex_raster_enable_gpu(gex_raster *raster, const gex_packet_gpu *backend);
void gex_raster_disable_gpu(gex_raster *raster);
int gex_raster_gpu_owned(const gex_raster *raster);
void gex_raster_bind(gex_raster *raster, const uint8_t *atlas, unsigned plane);
void gex_raster_blit(gex_raster *raster, int x, int y, int width, int height,
                     const uint8_t *pixels, int pitch, int key);
void gex_raster_present(gex_raster *raster);
void gex_raster_fill(gex_raster *raster, int x, int y, int width, int height, unsigned rgb);
void gex_raster_movie(gex_raster *raster, const uint8_t *pixels, int width, int height, int pitch);
void gex_raster_destroy(gex_raster *raster);
const uint8_t *gex_raster_pixels(const gex_raster *raster);
void gex_raster_clear(gex_raster *raster);
/* Both targets must have the same dimensions. */
void gex_raster_copy(gex_raster *destination, const gex_raster *source);
void gex_raster_copy_shaded(gex_raster *destination, const gex_raster *source, unsigned shade);
void gex_raster_pause(gex_raster *destination, const gex_raster *source,
                      const uint8_t *banner);
/* Import only writes made outside the packet renderer; remember records the
 * legacy renderer's result without replacing independently rasterized pixels. */
void gex_raster_sync(gex_raster *raster, const uint8_t *picture, int pitch);
void gex_raster_remember(gex_raster *raster, const uint8_t *picture, int pitch);
/* Export native writes to the logical guest picture; CPU-only imports do not
 * require another export. Call sync first to preserve external writes. */
void gex_raster_export(gex_raster *raster, uint8_t *picture, int pitch);
void gex_raster_packet(gex_raster *raster, const uint8_t *packet,
                       const gex_raster_state *state);
int gex_raster_title_packet(gex_raster *raster, uint32_t address, const uint8_t *packet,
                            const gex_raster_state *state);

#endif
