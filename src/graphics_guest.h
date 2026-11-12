#ifndef GEX_GRAPHICS_GUEST_H
#define GEX_GRAPHICS_GUEST_H

#include <stdint.h>
#include "packet_gpu.h"

/* Configure before guest graphics initialization; not a live resize API. */
int gex_graphics_set_render_size(int width, int height);
int gex_graphics_resize(int width, int height);
int gex_graphics_enable_gpu(void);
int gex_graphics_gpu_owned(void);
int gex_graphics_native_operation(uint32_t function);
void gex_graphics_movie(const uint8_t *pixels, int width, int height, int pitch);
/* Capture the outgoing frame once; shade is 0 (black) through 255 (unchanged). */
void gex_graphics_fade_capture(void);
void gex_graphics_fade_present(unsigned shade);
void gex_graphics_fade_release(void);
/* Hide cached outgoing frames until the retail scene transition completes. */
void gex_graphics_hold_black(int enabled);
/* Shade freshly rendered guest frames without changing their source pixels. */
void gex_graphics_frame_shade(unsigned shade);
/* Opaque Options widgets in the authored 320x224 canvas, before frame shading. */
void gex_graphics_menu_rect(int x, int y, int width, int height, unsigned rgb);
void gex_graphics_menu_clip_begin(int top, int bottom);
void gex_graphics_menu_clip_end(void);
void gex_graphics_cpu_access(void);
void gex_graphics_atlas_access(uint32_t rectangle);
int gex_graphics_view_width(void);
int gex_graphics_view_height(void);
/* Map a presented authored screen to its 320x240 menu coordinates. */
int gex_graphics_screen_point(int *x, int *y);
int gex_graphics_draw_tiles(void);
int gex_graphics_map_chunk_try(uint32_t chunk, uint32_t definitions, int32_t x, int32_t y);
int gex_graphics_map_flipped_cel_try(uint32_t image, uint32_t atlas, uint32_t flags,
                                     uint32_t tint, uint32_t rgb, uint16_t x, uint16_t y,
                                     uint16_t wrap_x, uint16_t wrap_y, uint16_t palette,
                                     uint32_t *previous_tail);
int gex_graphics_map_unflipped_cel_try(uint32_t image, uint32_t atlas, uint32_t flags,
                                       uint32_t tint, uint32_t rgb, uint16_t x, uint16_t y,
                                       uint16_t wrap_x, uint16_t wrap_y, uint16_t palette,
                                       gex_gpu_cel_result *result);
int gex_graphics_map_scaled_cel_try(const gex_gpu_scaled_cel_draw *draw, uint32_t *last_atlas);
void gex_graphics_packet_arena(void);
void gex_graphics_packet_allocated(uint32_t next);
void gex_graphics_scene_begin(void);
void gex_graphics_scene_end(void);
void gex_graphics_camera_begin(void);
void gex_graphics_camera_barrier(uint32_t object);
void gex_graphics_draw_player_sprite(void);
void gex_graphics_backdrop_begin(void);
void gex_graphics_backdrop_end(void);
int gex_graphics_backdrop_try(void);
void gex_graphics_transition_begin(void);
void gex_graphics_transition_end(void);
void gex_graphics_pause_begin(void);
void gex_graphics_pause_end(void);
/* Horizontal anchor: 0 = left, 1 = right, 2 = first third, 3 = second third. */
int gex_graphics_hud_begin(uint32_t object, int anchor, int scaled);
void gex_graphics_hud_end(uint32_t object);
void gex_graphics_shutdown(void);
void gex_graphics_format(void);
void gex_graphics_create(void);
void gex_graphics_destroy(void);
void gex_graphics_resolution(void);
void gex_graphics_begin_frame(void);
void gex_graphics_end_frame(void);
int gex_graphics_draw_packet(uint32_t packet);
int gex_graphics_title_try(uint32_t object, uint32_t cel);
void gex_graphics_flush(void);
void gex_graphics_forbidden(void);

#endif
