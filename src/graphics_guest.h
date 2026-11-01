#ifndef GEX_GRAPHICS_GUEST_H
#define GEX_GRAPHICS_GUEST_H

#include <stdint.h>

/* Configure before guest graphics initialization; not a live resize API. */
int gex_graphics_set_render_size(int width, int height);
int gex_graphics_enable_gpu(void);
int gex_graphics_gpu_owned(void);
int gex_graphics_native_operation(uint32_t function);
void gex_graphics_movie(const uint8_t *pixels, int width, int height, int pitch);
void gex_graphics_cpu_access(void);
void gex_graphics_atlas_access(uint32_t rectangle);
int gex_graphics_view_width(void);
int gex_graphics_view_height(void);
int gex_graphics_draw_tiles(void);
void gex_graphics_packet_arena(void);
void gex_graphics_packet_allocated(uint32_t next);
void gex_graphics_scene_begin(void);
void gex_graphics_scene_end(void);
void gex_graphics_camera_begin(void);
void gex_graphics_camera_barrier(uint32_t object);
void gex_graphics_draw_player_sprite(void);
void gex_graphics_backdrop_begin(void);
void gex_graphics_backdrop_end(void);
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
void gex_graphics_flush(void);
void gex_graphics_forbidden(void);

#endif
