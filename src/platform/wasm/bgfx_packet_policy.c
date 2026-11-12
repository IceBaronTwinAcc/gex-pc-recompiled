#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include "platform/bgfx/packet_policy.h"
#include "cs_copy_wgsl.h"
#include "cs_packet_wgsl.h"
#include "fs_present_packet_wgsl.h"
#include "vs_present_wgsl.h"
#include "fs_present_wgsl.h"

#define VALID(handle) ((handle).idx != UINT16_MAX)
#define INVALID(handle) ((handle).idx = UINT16_MAX)
static pthread_t thread;
static int (*thread_worker)(void *);
static bgfx_program_handle_t copy_program = {UINT16_MAX}, packet_presenter = {UINT16_MAX};
static bgfx_uniform_handle_t copy_source = {UINT16_MAX};
static int web_presented;

static int requested_api(gex_bgfx_request *r, bgfx_renderer_type_t *type) {
    const char *name = SDL_getenv("GEX_BGFX_API");
    if (!name) name = "auto";
    if (!strcmp(name, "auto") || !strcmp(name, "webgpu")) {
        *type = BGFX_RENDERER_TYPE_WEBGPU;
        return 0;
    }
    snprintf(r->error, r->size, "bgfx: WebAssembly bgfx requires GEX_BGFX_API=auto or webgpu");
    return -1;
}
static int window(SDL_Window *window, bgfx_platform_data_t *platform,
                  void **native_window, void **native_display, char *error, size_t size) {
    (void)platform; (void)native_display; (void)error; (void)size;
    if (window) *native_window = (void *)"#canvas";
    return 0;
}
static void *worker_entry(void *arg) {
    thread_worker(arg);
    return NULL;
}
static int thread_start(int (*worker)(void *), char *error, size_t size) {
    pthread_attr_t attr;
    int result = worker ? pthread_attr_init(&attr) : ENOMEM;
    if (!result) {
        result = emscripten_pthread_attr_settransferredcanvases(&attr, "canvas#canvas");
        if (!result) {
            thread_worker = worker;
            result = pthread_create(&thread, &attr, worker_entry, NULL);
        }
        pthread_attr_destroy(&attr);
    }
    if (result)
        snprintf(error, size, "bgfx: cannot create the canvas-owning API worker (error %d)", result);
    return result ? -1 : 0;
}
static void thread_join(void) { pthread_join(thread, NULL); thread = 0; }
static void after_execute(gex_bgfx_request *r) {
    if (!r->result && r->op == PRESENT_TARGET) emscripten_sleep(0);
}
static gex_bgfx_shader_binary shader_binary(int kind) {
    switch (kind) {
    case GEX_BGFX_SHADER_COMPUTE:
        return (gex_bgfx_shader_binary){gex_bgfx_cs_packet_wgsl, sizeof gex_bgfx_cs_packet_wgsl};
    case GEX_BGFX_SHADER_VERTEX:
        return (gex_bgfx_shader_binary){gex_bgfx_vs_present_wgsl, sizeof gex_bgfx_vs_present_wgsl};
    case GEX_BGFX_SHADER_FRAGMENT:
        return (gex_bgfx_shader_binary){gex_bgfx_fs_present_wgsl, sizeof gex_bgfx_fs_present_wgsl};
    default: return (gex_bgfx_shader_binary){NULL, 0};
    }
}
static int open_extra(gex_bgfx_policy_context *ctx, gex_bgfx_request *r) {
    bgfx_shader_handle_t cs = bgfx_create_shader(bgfx_copy(gex_bgfx_cs_copy_wgsl, sizeof gex_bgfx_cs_copy_wgsl));
    bgfx_shader_handle_t vs = bgfx_create_shader(bgfx_copy(gex_bgfx_vs_present_wgsl, sizeof gex_bgfx_vs_present_wgsl));
    bgfx_shader_handle_t fs = bgfx_create_shader(bgfx_copy(gex_bgfx_fs_present_packet_wgsl, sizeof gex_bgfx_fs_present_packet_wgsl));
    if (!VALID(cs) || !VALID(vs) || !VALID(fs)) {
        if (VALID(cs)) bgfx_destroy_shader(cs);
        if (VALID(vs)) bgfx_destroy_shader(vs);
        if (VALID(fs)) bgfx_destroy_shader(fs);
        return ctx->fail(r, "cannot create GPU-owned WebGPU shaders");
    }
    copy_program = bgfx_create_compute_program(cs, true);
    packet_presenter = bgfx_create_program(vs, fs, true);
    copy_source = bgfx_create_uniform("s_source", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    if (!VALID(copy_program) || !VALID(packet_presenter) || !VALID(copy_source))
        return ctx->fail(r, "cannot allocate GPU-owned WebGPU programs");
    return 0;
}
static void close_extra(void) {
    if (VALID(copy_program)) bgfx_destroy_program(copy_program);
    if (VALID(packet_presenter)) bgfx_destroy_program(packet_presenter);
    if (VALID(copy_source)) bgfx_destroy_uniform(copy_source);
    INVALID(copy_program); INVALID(packet_presenter); INVALID(copy_source);
}
static int clear_target(gex_bgfx_policy_context *ctx, gex_bgfx_request *r) {
    gex_bgfx_target *t = r->device;
    if (ctx->submit(r)) return -1;
    if (*ctx->queued) ctx->flush();
    size_t bytes = (size_t)t->width * t->height * sizeof *t->transfer;
    memset(t->transfer, 0, bytes);
    bgfx_update_texture_2d(t->image, 0, 0, 0, 0, t->width, t->height,
                           bgfx_copy(t->transfer, (uint32_t)bytes), UINT16_MAX);
    memset(t->tails, 0xff, (size_t)t->width * t->height * sizeof *t->tails);
    ctx->flush();
    return 0;
}
static int copy_target(gex_bgfx_policy_context *ctx, gex_bgfx_request *r) {
    gex_bgfx_target *destination = r->device, *source = r->source;
    if (!source || destination == source || destination->width != source->width ||
        destination->height != source->height || r->shade > 255)
        return ctx->fail(r, "invalid GPU-owned picture copy");
    if (ctx->submit(r)) return -1;
    if (*ctx->queued) ctx->flush();
    float values[4] = {(float)destination->width, (float)destination->height, (float)r->shade, 0};
    bgfx_set_view_frame_buffer(0, *ctx->compute_output);
    bgfx_set_uniform(*ctx->parameters, values, 1);
    bgfx_set_texture(0, copy_source, source->image, UINT32_MAX);
    bgfx_set_image(1, destination->image, 0, BGFX_ACCESS_READWRITE, BGFX_TEXTURE_FORMAT_R32U);
    bgfx_dispatch(0, copy_program, (destination->width + 7) / 8,
                  (destination->height + 7) / 8, 1, BGFX_DISCARD_ALL);
    memset(destination->tails, 0xff,
           (size_t)destination->width * destination->height * sizeof *destination->tails);
    ctx->flush();
    return 0;
}
static int blit_target(gex_bgfx_policy_context *ctx, gex_bgfx_request *r) {
    gex_bgfx_target *t = r->device;
    if (!r->pixels || r->width <= 0 || r->height <= 0 || r->pitch < r->width * 2 ||
        r->key < -1 || r->key > 65535)
        return ctx->fail(r, "invalid GPU-owned image blit");
    int source_left = r->x < 0 ? -r->x : 0;
    int source_top = r->y < 0 ? -r->y : 0;
    int source_right = r->width < t->width - r->x ? r->width : t->width - r->x;
    int source_bottom = r->height < t->height - r->y ? r->height : t->height - r->y;
    if (source_left >= source_right || source_top >= source_bottom) return 0;
    for (int sy = source_top; sy < source_bottom; ++sy) {
        const uint8_t *row = r->pixels + (size_t)sy * r->pitch;
        int sx = source_left;
        while (sx < source_right) {
            unsigned pixel = row[sx * 2] | (unsigned)row[sx * 2 + 1] << 8;
            if (r->key >= 0 && pixel == (unsigned)r->key) { ++sx; continue; }
            int end = sx + 1;
            while (end < source_right) {
                unsigned next = row[end * 2] | (unsigned)row[end * 2 + 1] << 8;
                if (next != pixel || (r->key >= 0 && next == (unsigned)r->key)) break;
                ++end;
            }
            gex_gpu_material material = {
                .opcode = 0x60, .rgb = ((pixel & 31u) << 3) |
                    (((pixel >> 5) & 31u) << 11) | (((pixel >> 10) & 31u) << 19)
            };
            while (sx < end) {
                if (*ctx->batch_target != t || *ctx->batch_material_count == GEX_BGFX_MAX_BATCH_MATERIALS ||
                    *ctx->batch_node_count == GEX_BGFX_MAX_BATCH_NODES ||
                    *ctx->batch_pixel_count == GEX_BGFX_MAX_BATCH_NODES)
                    if (ctx->submit(r)) return -1;
                int count = end - sx;
                int capacity = GEX_BGFX_MAX_BATCH_NODES - *ctx->batch_node_count;
                if (count > capacity) count = capacity;
                gex_gpu_span span = {.x = r->x + sx, .y = r->y + sy, .count = count, .scale = 1};
                gex_bgfx_request draw = *r;
                draw.spans = &span; draw.count = 1; draw.material = &material;
                if (ctx->draw(&draw)) return -1;
                sx += count;
            }
        }
    }
    return 0;
}
static int present_target(gex_bgfx_policy_context *ctx, gex_bgfx_request *r) {
    gex_bgfx_target *t = r->device;
    if (ctx->submit(r)) return -1;
    if (*ctx->visible) {
        int width, height;
        EMSCRIPTEN_RESULT result = emscripten_get_canvas_element_size("#canvas", &width, &height);
        if (result != EMSCRIPTEN_RESULT_SUCCESS) {
            snprintf(r->error, r->size, "bgfx: cannot read WebGPU canvas size (%d)", result);
            return -1;
        }
        if (width > 0 && height > 0 && (width != *ctx->window_width || height != *ctx->window_height)) {
            bgfx_init_t defaults;
            bgfx_init_ctor(&defaults);
            defaults.swapChain.nwh = *ctx->native_window;
            defaults.swapChain.ndt = *ctx->native_display;
            defaults.swapChain.width = (uint32_t)width;
            defaults.swapChain.height = (uint32_t)height;
            defaults.swapChain.numBackBuffers = (uint8_t)*ctx->swap_buffers;
            defaults.swapChain.maxFrameLatency = (uint8_t)*ctx->swap_buffers;
            bgfx_reset(BGFX_RESET_VSYNC, &defaults.swapChain);
            *ctx->window_width = width; *ctx->window_height = height;
        }
    }
    int width = *ctx->visible ? *ctx->window_width : t->width;
    int height = *ctx->visible ? *ctx->window_height : t->height;
    if (width <= 0 || height <= 0) return ctx->fail(r, "invalid GPU-owned presentation dimensions");
    gex_render_rect rect = gex_renderer_fit(width, height, t->width, t->height);
    float values[4] = {(float)t->width, (float)t->height, 0, 0};
    const bgfx_frame_buffer_handle_t backbuffer = {UINT16_MAX};
    bgfx_set_view_frame_buffer(2, backbuffer);
    bgfx_set_view_frame_buffer(3, backbuffer);
    bgfx_set_view_rect(2, 0, 0, width, height, 0, 1);
    bgfx_set_view_clear(2, BGFX_CLEAR_COLOR, 0x000000ff, 1, 0);
    bgfx_touch(2);
    bgfx_set_view_rect(3, rect.x, rect.y, rect.width, rect.height, 0, 1);
    bgfx_set_uniform(*ctx->parameters, values, 1);
    bgfx_set_vertex_buffer(0, *ctx->vertices, 0, 3);
    bgfx_set_texture(0, *ctx->sampler, t->image, UINT32_MAX);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(3, packet_presenter, 0, BGFX_DISCARD_ALL);
    ctx->flush();
    if (*ctx->visible && !web_presented) {
        web_presented = 1;
        MAIN_THREAD_ASYNC_EM_ASM({
            if (document.getElementById("status").textContent === "Game files loaded. Starting Gex...")
                window.gexSetStatus("Gex is running.");
        });
    }
    return 0;
}
static int execute_extra(gex_bgfx_policy_context *ctx, gex_bgfx_request *r) {
    switch (r->op) {
    case CLEAR: return clear_target(ctx, r);
    case COPY: return copy_target(ctx, r);
    case BLIT: return blit_target(ctx, r);
    case PRESENT_TARGET: return present_target(ctx, r);
    case MOVIE: return ctx->fail(r, "WebGPU movie uploads are unavailable in this build");
    default: return ctx->fail(r, "unknown worker operation");
    }
}
static int native_clear(void *device, char *error, size_t size) {
    gex_bgfx_request r = {.op = CLEAR, .device = device, .error = error, .size = size};
    return gex_bgfx_packet_context()->invoke(&r);
}
static int native_copy(void *destination, void *source, unsigned shade, char *error, size_t size) {
    gex_bgfx_request r = {.op = COPY, .device = destination, .source = source, .shade = shade,
                          .error = error, .size = size};
    return gex_bgfx_packet_context()->invoke(&r);
}
static int native_blit(void *device, int x, int y, int width, int height,
                       const uint8_t *pixels, int pitch, int key, char *error, size_t size) {
    gex_bgfx_request r = {.op = BLIT, .device = device, .x = x, .y = y, .width = width,
                          .height = height, .pixels = pixels, .pitch = pitch, .key = key,
                          .error = error, .size = size};
    return gex_bgfx_packet_context()->invoke(&r);
}
static int native_present(void *device, char *error, size_t size) {
    gex_bgfx_request r = {.op = PRESENT_TARGET, .device = device, .error = error, .size = size};
    return gex_bgfx_packet_context()->invoke(&r);
}
static void native_bind(void *device, const uint8_t *atlas, unsigned plane) {
    (void)device; (void)atlas; (void)plane;
}
static int native_movie(void *device, const uint8_t *pixels, int width, int height, int pitch,
                        char *error, size_t size) {
    (void)device; (void)pixels; (void)width; (void)height; (void)pitch;
    snprintf(error, size, "WebGPU movie uploads are unavailable in this build");
    return -1;
}
const gex_gpu_native_ops gex_bgfx_packet_native_ops = {
    .clear = native_clear, .copy = native_copy, .blit = native_blit,
    .present = native_present, .bind = native_bind, .movie = native_movie
};
const gex_bgfx_packet_policy *gex_bgfx_packet_platform(void) {
    static const gex_bgfx_packet_policy policy = {
        1, "cannot initialise WebGPU; check browser support and GPU adapter availability",
        "synchronous WebGPU packet readback is unsupported",
        requested_api, window, thread_start, thread_join, after_execute, shader_binary,
        open_extra, close_extra, execute_extra
    };
    return &policy;
}
