#ifndef GEX_BGFX_PACKET_POLICY_H
#define GEX_BGFX_PACKET_POLICY_H

#include <SDL2/SDL.h>
#include <bgfx/c99/bgfx.h>
#include "packet_gpu.h"
#include "renderer.h"

enum { GEX_BGFX_MAX_BATCH_NODES = 262144, GEX_BGFX_MAX_BATCH_MATERIALS = 512 };
enum { GEX_BGFX_SHADER_COMPUTE, GEX_BGFX_SHADER_VERTEX, GEX_BGFX_SHADER_FRAGMENT };

typedef struct {
    int width, height;
    bgfx_texture_handle_t image, readback;
    uint32_t *transfer, *tails;
} gex_bgfx_target;

typedef enum {
    OPEN, CREATE, DESTROY, UPLOAD, DOWNLOAD, DRAW, MATERIAL, SUBMIT,
    CONFIGURE, PRESENT, CAPTURE, CLEAR, COPY, BLIT, PRESENT_TARGET, MOVIE, STOP
} gex_bgfx_operation;

typedef struct {
    gex_bgfx_operation op;
    gex_bgfx_target *device, *source;
    int width, height, x, y, pitch, key, result;
    unsigned shade;
    const uint16_t *input;
    const uint8_t *pixels;
    uint16_t *output;
    uint8_t *rgba;
    const gex_gpu_span *spans;
    size_t count;
    const gex_gpu_material *material;
    const gex_render_frame *frame;
    void *window;
    char *error;
    size_t size;
} gex_bgfx_request;

typedef struct { const uint8_t *bytes; uint32_t size; } gex_bgfx_shader_binary;

typedef struct {
    int *visible, *window_width, *window_height, *swap_buffers;
    void **native_window, **native_display;
    bgfx_frame_buffer_handle_t *compute_output;
    bgfx_uniform_handle_t *parameters, *sampler;
    bgfx_vertex_buffer_handle_t *vertices;
    gex_bgfx_target **batch_target;
    unsigned *batch_material_count, *batch_node_count, *batch_pixel_count, *queued;
    int (*fail)(gex_bgfx_request *request, const char *text);
    int (*submit)(gex_bgfx_request *request);
    int (*draw)(gex_bgfx_request *request);
    int (*invoke)(gex_bgfx_request *request);
    uint32_t (*flush)(void);
} gex_bgfx_policy_context;

typedef struct {
    int gpu_owned;
    const char *initialization_error, *readback_error;
    int (*requested_api)(gex_bgfx_request *, bgfx_renderer_type_t *);
    int (*window)(SDL_Window *, bgfx_platform_data_t *, void **, void **,
                  char *, size_t);
    int (*thread_start)(int (*worker)(void *), char *, size_t);
    void (*thread_join)(void);
    void (*after_execute)(gex_bgfx_request *);
    gex_bgfx_shader_binary (*shader)(int kind);
    int (*open)(gex_bgfx_policy_context *, gex_bgfx_request *);
    void (*close)(void);
    int (*execute)(gex_bgfx_policy_context *, gex_bgfx_request *);
} gex_bgfx_packet_policy;

const gex_bgfx_packet_policy *gex_bgfx_packet_platform(void);
gex_bgfx_policy_context *gex_bgfx_packet_context(void);
extern const gex_gpu_native_ops gex_bgfx_packet_native_ops;

#endif
