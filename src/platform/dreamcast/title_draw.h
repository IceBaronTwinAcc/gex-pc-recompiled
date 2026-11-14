#ifndef GEX_DREAMCAST_TITLE_DRAW_H
#define GEX_DREAMCAST_TITLE_DRAW_H

#include "packet_gpu.h"
#include "raster.h"

typedef int (*gex_title_image_submit)(void *target, const gex_gpu_vertex vertices[4],
                                      int left, int top, int right, int bottom,
                                      const gex_gpu_image *image, uint64_t image_id,
                                      unsigned rgb, int vertex_tint,
                                      char *error, size_t size);

typedef struct {
    uint16_t x[4], y[4], wrap_x, wrap_y, page, clut;
    uint16_t width, height;
    uint8_t u[4], v[4], layer, is_quad, page_change;
    uint32_t rgb, palette_address;
} gex_dreamcast_tile_draw;

typedef int (*gex_title_polygon_submit)(void *target, const gex_gpu_vertex vertices[4],
                                        int left, int top, int right, int bottom,
                                        const gex_gpu_material *material,
                                        char *error, size_t size);

int gex_dreamcast_title_record(const gex_gpu_guest_io *io, uint32_t object, uint32_t cel,
                               char *error, size_t size);
int gex_dreamcast_title_record_direct(const gex_gpu_guest_io *io, uint32_t object,
                                      uint32_t cel, const uint32_t roots[2],
                                      char *error, size_t size);
int gex_dreamcast_title_has_direct(uint32_t root, uint32_t anchor);
int gex_dreamcast_title_draw_direct(void *target, uint32_t root, uint32_t anchor,
                                     const gex_raster_state *state, gex_title_image_submit submit,
                                     char *error, size_t size);
int gex_dreamcast_title_draw_direct_tiles(void *target, uint32_t root, uint32_t anchor,
                                           const gex_raster_state *state,
                                           gex_title_image_submit image,
                                           gex_title_polygon_submit polygon,
                                           gex_gpu_direct_state *result,
                                           char *error, size_t size);
int gex_dreamcast_title_record_tiles(const gex_gpu_guest_io *io,
                                     const gex_dreamcast_tile_draw *tiles, size_t count,
                                     const uint32_t tails[4]);
int gex_dreamcast_title_record_cel_tiles(const gex_dreamcast_tile_draw *tiles, size_t count,
                                         const uint32_t roots[2], const uint32_t tails[2]);
void gex_dreamcast_title_merge_lists(const uint32_t source[2], const uint32_t roots[2],
                                      const uint32_t tails[2]);
int gex_dreamcast_palette_record(uint32_t anchor, const gex_gpu_palette_upload *upload);
int gex_dreamcast_palette_take(uint32_t anchor, gex_gpu_palette_upload *upload);
void gex_dreamcast_palette_reset(void);
void gex_dreamcast_title_reset_lists(uint32_t first, uint32_t second);
int gex_dreamcast_backdrop_record(const gex_gpu_guest_io *io, uint32_t scene,
                                  char *error, size_t size);
int gex_dreamcast_backdrop_record_direct(const gex_gpu_guest_io *io, uint32_t scene,
                                         const uint32_t roots[2], char *error, size_t size);
int gex_dreamcast_map_cel_record(const gex_gpu_guest_io *io, uint32_t scene,
                                 uint32_t object, uint32_t cel, char *error, size_t size);
int gex_dreamcast_scene_cel_record_direct(const gex_gpu_guest_io *io, uint32_t scene,
                                          uint32_t object, uint32_t cel,
                                          const uint32_t roots[2], char *error, size_t size);
int gex_dreamcast_title_packet(void *target, uint32_t address, const uint8_t *packet,
                               const gex_raster_state *state, gex_title_image_submit submit,
                               char *error, size_t size);
void gex_dreamcast_title_forget(uint32_t begin, uint32_t end);
void gex_dreamcast_title_reset(void);

#endif
