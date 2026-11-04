#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL_syswm.h>
#include <bgfx/c99/bgfx.h>
#include "platform/bgfx/backend.h"
#include "packet_gpu.h"
#include "preferences.h"
#include "cs_packet_spirv.h"
#include "cs_packet_metal.h"
#include "cs_packet_glsl.h"
#include "cs_packet_essl.h"
#include "vs_present_spirv.h"
#include "vs_present_metal.h"
#include "vs_present_glsl.h"
#include "vs_present_essl.h"
#include "fs_present_spirv.h"
#include "fs_present_metal.h"
#include "fs_present_glsl.h"
#include "fs_present_essl.h"
#ifdef _WIN32
#include "cs_packet_dxbc.h"
#include "cs_packet_dxil.h"
#include "vs_present_dxbc.h"
#include "vs_present_dxil.h"
#include "fs_present_dxbc.h"
#include "fs_present_dxil.h"
#endif

#define VALID(handle) ((handle).idx != UINT16_MAX)
#define INVALID(handle) ((handle).idx = UINT16_MAX)
enum { MAX_BATCH_NODES = 262144, MAX_BATCH_MATERIALS = 512, MAX_PIXEL_DEPTH = 64,
       MAX_QUEUED = 512 };
typedef struct {
    int width, height;
    bgfx_texture_handle_t image, readback;
    uint32_t *transfer, *tails;
} target;
typedef enum { OPEN, CREATE, DESTROY, UPLOAD, DOWNLOAD, DRAW, MATERIAL, SUBMIT,
               CONFIGURE, PRESENT, CAPTURE, STOP } operation;
typedef struct {
    operation op;
    target *device;
    int width, height, result;
    const uint16_t *input;
    uint16_t *output;
    uint8_t *rgba;
    const gex_gpu_span *spans;
    size_t count;
    const gex_gpu_material *material;
    const gex_render_frame *frame;
    void *window;
    char *error;
    size_t size;
} request;

static SDL_Thread *thread;
static SDL_mutex *calls, *queue;
static SDL_cond *work, *completed;
static request *pending;
static int done, initialised, visible;
static bgfx_platform_data_t platform;
static void *native_window, *native_display;
#ifdef __ANDROID__
static SDL_Window *presentation_window;
#endif
static int window_width, window_height;
static int swap_buffers;
static bgfx_program_handle_t program = {UINT16_MAX}, presenter = {UINT16_MAX};
static bgfx_uniform_handle_t parameters = {UINT16_MAX}, sampler = {UINT16_MAX};
static bgfx_index_buffer_handle_t atlas_buffer = {UINT16_MAX}, table_buffer = {UINT16_MAX};
static bgfx_vertex_buffer_handle_t vertices = {UINT16_MAX};
static bgfx_texture_handle_t frame_texture = {UINT16_MAX};
static bgfx_frame_buffer_handle_t compute_output = {UINT16_MAX};
static bgfx_frame_buffer_handle_t frame_output = {UINT16_MAX};
static bgfx_texture_handle_t frame_readback = {UINT16_MAX};
static uint8_t atlas_cache[0x100000], table_cache[1024];
static target *batch_target;
static uint32_t batch_pixels[MAX_BATCH_NODES * 2];
static uint32_t batch_nodes[MAX_BATCH_NODES * 3];
static uint8_t batch_depths[MAX_BATCH_NODES];
static uint32_t batch_materials[MAX_BATCH_MATERIALS * 4];
static unsigned batch_pixel_count, batch_node_count, batch_material_count;
static unsigned queued;
static int frame_width, frame_height;
static int invoke_worker(request *r);
#if GEX_PROFILE_RENDERER
static unsigned profile_draws, profile_uploads, profile_resolves;
static size_t profile_bytes;
static uint64_t profile_ticks[STOP + 1];
#define PROFILE(field, value) (profile_##field += (value))
#else
#define PROFILE(field, value) ((void)0)
#endif

static int fail(request *r, const char *text) {
    snprintf(r->error, r->size, "bgfx: %s", text);
    return -1;
}

static uint32_t flush(void) {
    /* Compute views bind framebuffers too. Keep packet flushes off the native
     * swapchain, including after bgfx_reset clears view framebuffer bindings. */
    bgfx_set_view_frame_buffer(0, compute_output);
    queued = 0;
    return bgfx_frame(false);
}

typedef struct { const uint8_t *bytes; uint32_t size; } shader_binary;
#define BINARY(name, profile) {gex_bgfx_##name##_##profile, sizeof gex_bgfx_##name##_##profile}
#ifdef _WIN32
#define DIRECTX_BINARIES(name) BINARY(name, dxbc), BINARY(name, dxil),
#else
#define DIRECTX_BINARIES(name) {NULL, 0}, {NULL, 0},
#endif
#define SHADER_BINARIES(name) {DIRECTX_BINARIES(name) BINARY(name, metal), \
    BINARY(name, glsl), BINARY(name, essl), BINARY(name, spirv)}
static const shader_binary compute_shaders[] = SHADER_BINARIES(cs_packet);
static const shader_binary vertex_shaders[] = SHADER_BINARIES(vs_present);
static const shader_binary fragment_shaders[] = SHADER_BINARIES(fs_present);

static int shader_profile(bgfx_renderer_type_t type) {
    switch (type) {
    case BGFX_RENDERER_TYPE_DIRECT3D11: return 0;
    case BGFX_RENDERER_TYPE_DIRECT3D12: return 1;
    case BGFX_RENDERER_TYPE_METAL: return 2;
    case BGFX_RENDERER_TYPE_OPENGL: return 3;
    case BGFX_RENDERER_TYPE_OPENGLES: return 4;
    case BGFX_RENDERER_TYPE_VULKAN: return 5;
    default: return -1;
    }
}

static bgfx_shader_handle_t shader(const shader_binary *binaries, int profile) {
    return bgfx_create_shader(bgfx_copy(binaries[profile].bytes, binaries[profile].size));
}

static int requested_api(request *r, bgfx_renderer_type_t *type) {
    static const struct { const char *name; bgfx_renderer_type_t type; } names[] = {
        {"auto", BGFX_RENDERER_TYPE_COUNT}, {"vulkan", BGFX_RENDERER_TYPE_VULKAN},
        {"metal", BGFX_RENDERER_TYPE_METAL}, {"opengl", BGFX_RENDERER_TYPE_OPENGL},
        {"opengles", BGFX_RENDERER_TYPE_OPENGLES},
        {"direct3d11", BGFX_RENDERER_TYPE_DIRECT3D11},
        {"direct3d12", BGFX_RENDERER_TYPE_DIRECT3D12}
    };
    const char *name = SDL_getenv("GEX_BGFX_API");
    if (!name) name = gex_api_name(gex_prefs.api);
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i)
        if (!strcmp(name, names[i].name)) {
            *type = names[i].type;
            return 0;
        }
    return fail(r, "GEX_BGFX_API requires auto, vulkan, metal, opengl, opengles, direct3d11 or direct3d12");
}

static int api_index(bgfx_renderer_type_t type) {
    switch (type) {
    case BGFX_RENDERER_TYPE_VULKAN: return GEX_API_VULKAN;
    case BGFX_RENDERER_TYPE_METAL: return GEX_API_METAL;
    case BGFX_RENDERER_TYPE_OPENGL: return GEX_API_OPENGL;
    case BGFX_RENDERER_TYPE_OPENGLES: return GEX_API_OPENGLES;
    case BGFX_RENDERER_TYPE_DIRECT3D11: return GEX_API_D3D11;
    case BGFX_RENDERER_TYPE_DIRECT3D12: return GEX_API_D3D12;
    default: return GEX_API_AUTO;
    }
}

uint32_t gex_bgfx_supported_apis(void) {
    bgfx_renderer_type_t types[BGFX_RENDERER_TYPE_COUNT];
    uint8_t count = bgfx_get_supported_renderers(BGFX_RENDERER_TYPE_COUNT, types);
    uint32_t mask = 0;
    for (unsigned i = 0; i < count; ++i) {
        int api = api_index(types[i]);
#ifdef __APPLE__
        /* macOS OpenGL stops at 4.1 and cannot run the compute packet shader. */
        if (api != GEX_API_METAL) continue;
#endif
        if (api && shader_profile(types[i]) >= 0)
            mask |= 1u << api;
    }
    return mask;
}

static void close_device(void) {
    if (!initialised) return;
    if (VALID(frame_texture)) bgfx_destroy_texture(frame_texture);
    if (VALID(compute_output)) bgfx_destroy_frame_buffer(compute_output);
    if (VALID(frame_output)) bgfx_destroy_frame_buffer(frame_output);
    if (VALID(frame_readback)) bgfx_destroy_texture(frame_readback);
    if (VALID(vertices)) bgfx_destroy_vertex_buffer(vertices);
    if (VALID(atlas_buffer)) bgfx_destroy_index_buffer(atlas_buffer);
    if (VALID(table_buffer)) bgfx_destroy_index_buffer(table_buffer);
    if (VALID(program)) bgfx_destroy_program(program);
    if (VALID(presenter)) bgfx_destroy_program(presenter);
    if (VALID(parameters)) bgfx_destroy_uniform(parameters);
    if (VALID(sampler)) bgfx_destroy_uniform(sampler);
    bgfx_shutdown();
    INVALID(frame_texture); INVALID(vertices); INVALID(atlas_buffer); INVALID(table_buffer);
    INVALID(program); INVALID(presenter); INVALID(parameters); INVALID(sampler);
    INVALID(compute_output); INVALID(frame_output); INVALID(frame_readback);
    initialised = queued = frame_width = frame_height = 0;
    batch_target = NULL;
    batch_pixel_count = batch_node_count = batch_material_count = 0;
}

static int open_device(request *r) {
    if (initialised) return 0;
    bgfx_init_t init;
    bgfx_init_ctor(&init);
    if (requested_api(r, &init.type)) return -1;
    init.fallback = init.type == BGFX_RENDERER_TYPE_COUNT;
    if (!init.fallback) {
        bgfx_renderer_type_t supported[BGFX_RENDERER_TYPE_COUNT];
        uint8_t count = bgfx_get_supported_renderers(BGFX_RENDERER_TYPE_COUNT, supported);
        unsigned i;
        for (i = 0; i < count && supported[i] != init.type; ++i) {}
        if (i == count) return fail(r, "requested graphics API was not compiled into bgfx");
    }
    init.platformData = platform;
    init.swapChain.nwh = native_window;
    init.swapChain.ndt = native_display;
    init.swapChain.width = visible ? (uint32_t)window_width : 0;
    init.swapChain.height = visible ? (uint32_t)window_height : 0;
    swap_buffers = gex_prefs.triple ? 3 : 2;
    init.swapChain.numBackBuffers = (uint8_t)swap_buffers;
    init.swapChain.maxFrameLatency = (uint8_t)swap_buffers;
    init.reset = visible ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
    if (!bgfx_init(&init)) return fail(r, "cannot initialise the platform GPU backend");
    initialised = 1;
    bgfx_renderer_type_t actual = bgfx_get_renderer_type();
    gex_prefs.active_api = api_index(actual);
    int profile = shader_profile(actual);
    if ((!init.fallback && actual != init.type) || profile < 0 ||
        !compute_shaders[profile].bytes) {
        close_device();
        return fail(r, "selected graphics API has no matching embedded shaders");
    }
    const bgfx_caps_t *caps = bgfx_get_caps();
    /* ES 3.1 permits R32U read/write without the broader IMAGE_RW capability. */
    uint64_t required = BGFX_CAPS_COMPUTE | BGFX_CAPS_INDEX32;
    uint32_t image = BGFX_CAPS_FORMAT_TEXTURE_IMAGE_READ | BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE;
    if ((caps->supported & required) != required ||
        (caps->formats[BGFX_TEXTURE_FORMAT_R32U] & image) != image) {
        snprintf(r->error, r->size,
            "bgfx: %s requires compute/32-bit buffers (caps=0x%llx, required=0x%llx) "
            "and R32U image read/write (format=0x%x, required=0x%x)",
            bgfx_get_renderer_name(actual), (unsigned long long)caps->supported,
            (unsigned long long)required, caps->formats[BGFX_TEXTURE_FORMAT_R32U], image);
        close_device();
        return -1;
    }
    bgfx_shader_handle_t cs = shader(compute_shaders, profile);
    bgfx_shader_handle_t vs = shader(vertex_shaders, profile);
    bgfx_shader_handle_t fs = shader(fragment_shaders, profile);
    if (!VALID(cs) || !VALID(vs) || !VALID(fs)) {
        if (VALID(cs)) bgfx_destroy_shader(cs);
        if (VALID(vs)) bgfx_destroy_shader(vs);
        if (VALID(fs)) bgfx_destroy_shader(fs);
        close_device();
        return fail(r, "cannot create portable shaders");
    }
    program = bgfx_create_compute_program(cs, true);
    presenter = bgfx_create_program(vs, fs, true);
    parameters = bgfx_create_uniform("u_batch", BGFX_UNIFORM_TYPE_VEC4, 1);
    sampler = bgfx_create_uniform("s_frame", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    /* Full-screen triangle. No platform graphics calls or native shader compilation. */
    const float triangle[][5] = {{-1, 1, 0, 0, 0}, {3, 1, 0, 2, 0}, {-1, -3, 0, 0, 2}};
    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&layout);
    vertices = bgfx_create_vertex_buffer(bgfx_copy(triangle, sizeof triangle), &layout, 0);
    atlas_buffer = bgfx_create_index_buffer(bgfx_copy(atlas_cache, sizeof atlas_cache),
        BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ);
    table_buffer = bgfx_create_index_buffer(bgfx_copy(table_cache, sizeof table_cache),
        BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ);
    compute_output = bgfx_create_frame_buffer(1, 1, BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_RT_WRITE_ONLY);
    if (!VALID(program) || !VALID(presenter) || !VALID(parameters) || !VALID(sampler) ||
        !VALID(vertices) || !VALID(atlas_buffer) || !VALID(table_buffer) || !VALID(compute_output)) {
        close_device();
        return fail(r, "cannot allocate shader programs or presentation resources");
    }
    bgfx_set_view_mode(0, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_set_view_mode(3, BGFX_VIEW_MODE_SEQUENTIAL);
    fprintf(stderr, "[bgfx] %s; exact integer palette/blend shaders; GPU API access through bgfx only\n",
            bgfx_get_renderer_name(bgfx_get_renderer_type()));
    return 0;
}

static int create_target(request *r) {
    if (open_device(r)) return -1;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (r->width < 1 || r->height < 1 || (uint32_t)r->width > caps->limits.maxTextureSize ||
        (uint32_t)r->height > caps->limits.maxTextureSize)
        return fail(r, "target dimensions exceed GPU texture limits");
    target *t = calloc(1, sizeof *t);
    if (!t) return fail(r, "cannot allocate target");
    t->width = r->width; t->height = r->height;
    t->transfer = calloc((size_t)t->width * t->height, sizeof *t->transfer);
    t->tails = malloc((size_t)t->width * t->height * sizeof *t->tails);
    t->image = bgfx_create_texture_2d(t->width, t->height, false, 1, BGFX_TEXTURE_FORMAT_R32U,
        BGFX_TEXTURE_COMPUTE_WRITE | BGFX_TEXTURE_BLIT_DST, NULL, 0);
    t->readback = bgfx_create_texture_2d(t->width, t->height, false, 1, BGFX_TEXTURE_FORMAT_R32U,
        BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK, NULL, 0);
    if (!t->transfer || !t->tails || !VALID(t->image) || !VALID(t->readback)) {
        if (VALID(t->image)) bgfx_destroy_texture(t->image);
        if (VALID(t->readback)) bgfx_destroy_texture(t->readback);
        free(t->tails); free(t->transfer); free(t);
        return fail(r, "cannot allocate GPU target/readback storage");
    }
    memset(t->tails, 0xff, (size_t)t->width * t->height * sizeof *t->tails);
    r->device = t;
    return 0;
}

static int submit_batch(request *r) {
    if (!batch_node_count) {
        batch_target = NULL;
        batch_material_count = 0;
        return 0;
    }
    unsigned node_offset = batch_pixel_count * 2;
    unsigned material_offset = node_offset + batch_node_count * 3;
    unsigned bytes = (material_offset + batch_material_count * 4) * sizeof(uint32_t);
    const bgfx_memory_t *memory = bgfx_alloc(bytes);
    memcpy(memory->data, batch_pixels, node_offset * sizeof(uint32_t));
    memcpy(memory->data + node_offset * sizeof(uint32_t), batch_nodes,
           batch_node_count * 3 * sizeof(uint32_t));
    memcpy(memory->data + material_offset * sizeof(uint32_t), batch_materials,
           batch_material_count * 4 * sizeof(uint32_t));
    bgfx_index_buffer_handle_t buffer = bgfx_create_index_buffer(memory,
        BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ);
    if (!VALID(buffer)) return fail(r, "cannot allocate ordered packet batch");
    float values[4] = {(float)batch_target->width, (float)batch_pixel_count,
                      (float)node_offset, (float)material_offset};
    bgfx_set_uniform(parameters, values, 1);
    bgfx_set_image(0, batch_target->image, 0, BGFX_ACCESS_READWRITE, BGFX_TEXTURE_FORMAT_R32U);
    bgfx_set_compute_index_buffer(1, atlas_buffer, BGFX_ACCESS_READ, 0, UINT32_MAX);
    bgfx_set_compute_index_buffer(2, table_buffer, BGFX_ACCESS_READ, 0, UINT32_MAX);
    bgfx_set_compute_index_buffer(3, buffer, BGFX_ACCESS_READ, 0, UINT32_MAX);
    bgfx_dispatch(0, program, (batch_pixel_count + 63) / 64, 1, 1, BGFX_DISCARD_ALL);
    bgfx_destroy_index_buffer(buffer);
    PROFILE(draws, 1);
    PROFILE(bytes, bytes);
    for (unsigned i = 0; i < batch_pixel_count; ++i)
        batch_target->tails[batch_pixels[i * 2]] = UINT32_MAX;
    batch_target = NULL;
    batch_pixel_count = batch_node_count = batch_material_count = 0;
    if (++queued >= MAX_QUEUED) flush();
    return 0;
}

static int submit_from_caller(request *r) {
    request submission = {.op = SUBMIT, .error = r->error, .size = r->size};
    return invoke_worker(&submission);
}

static int upload_target(request *r) {
    target *t = r->device;
    /* Resource updates precede dispatches within a bgfx frame. Close the old
     * frame before overwriting an image used by previously queued packets. */
    if (submit_batch(r)) return -1;
    if (queued) flush();
    for (size_t i = 0; i < (size_t)t->width * t->height; ++i) t->transfer[i] = r->input[i];
    bgfx_update_texture_2d(t->image, 0, 0, 0, 0, t->width, t->height,
        bgfx_copy(t->transfer, (uint32_t)((size_t)t->width * t->height * 4)), UINT16_MAX);
    PROFILE(uploads, 1);
    PROFILE(bytes, (size_t)t->width * t->height * 4);
    return 0;
}

static int download_target(request *r) {
    target *t = r->device;
    if (submit_batch(r)) return -1;
    bgfx_texture_region_t src = {.handle = t->image}, dst = {.handle = t->readback};
    bgfx_blit(1, &dst, &src);
    uint32_t ready = bgfx_read_texture(&dst, t->transfer);
    uint32_t current;
    do { current = flush(); } while ((int32_t)(ready - current) > 0);
    PROFILE(resolves, 1);
    for (size_t i = 0; i < (size_t)t->width * t->height; ++i) r->output[i] = (uint16_t)t->transfer[i];
    return 0;
}

static int snapshot(bgfx_index_buffer_handle_t *buffer, uint8_t *cache, const uint8_t *data,
                    uint32_t bytes, request *r) {
    if (VALID(*buffer) && !memcmp(cache, data, bytes)) return 0;
    if (submit_batch(r)) return -1;
    bgfx_index_buffer_handle_t next = bgfx_create_index_buffer(bgfx_copy(data, bytes),
        BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ);
    if (!VALID(next)) return fail(r, "cannot allocate immutable material snapshot");
    if (VALID(*buffer)) bgfx_destroy_index_buffer(*buffer);
    *buffer = next;
    memcpy(cache, data, bytes);
    PROFILE(uploads, 1);
    PROFILE(bytes, bytes);
    return 0;
}

static int atlas_range_equal(const uint8_t *atlas, unsigned offset, unsigned bytes) {
    offset &= 0xfffff;
    unsigned first = bytes < 0x100000 - offset ? bytes : 0x100000 - offset;
    return !memcmp(atlas_cache + offset, atlas + offset, first) &&
        (first == bytes || !memcmp(atlas_cache, atlas, bytes - first));
}

static int material_changed(const gex_gpu_material *m) {
    unsigned format = (m->page >> 7) & 3;
    unsigned base = (m->page & 15) * 128 + (m->page & 16 ? 0x80000 : 0);
    if (memcmp(table_cache, m->modulation, sizeof table_cache)) return 1;
    if (format < 2 && !atlas_range_equal(m->atlas, m->clut * 32, format ? 512 : 32))
        return 1;
    /* Validate the sampled page, not the whole megabyte for every sprite.
     * The immutable device snapshot is replaced before any changed page or
     * palette is used, including pages that wrap around the atlas. */
    for (unsigned y = 0; y < 256; ++y)
        if (!atlas_range_equal(m->atlas, base + y * 2048, 128u << format)) return 1;
    return 0;
}

static int batch_material(target *t, const gex_gpu_material *m, unsigned *index, request *r) {
    if ((batch_target && batch_target != t) || batch_material_count == MAX_BATCH_MATERIALS)
        if (submit_from_caller(r)) return -1;
    batch_target = t;
    *index = batch_material_count++;
    uint32_t *values = batch_materials + *index * 4;
    values[0] = m->page; values[1] = m->clut;
    values[2] = m->opcode; values[3] = m->rgb & 0xffffffu;
    return 0;
}

static int draw_target(request *r) {
    target *t = r->device;
    const gex_gpu_material *m = r->material;
    unsigned material;
    if (batch_material(t, m, &material, r)) return -1;
    for (size_t i = 0; i < r->count; ++i) {
        const gex_gpu_span *s = &r->spans[i];
        if (s->count < 0 || s->scale < 1 || s->x < 0 || s->y < 0 ||
            s->y >= t->height || s->count > t->width - s->x)
            return fail(r, "invalid pixel coverage");
        for (int x = 0; x < s->count; ++x) {
            unsigned destination = (unsigned)s->y * t->width + s->x + x;
            if (batch_node_count == MAX_BATCH_NODES ||
                (t->tails[destination] != UINT32_MAX &&
                 batch_depths[t->tails[destination]] == MAX_PIXEL_DEPTH)) {
                if (submit_from_caller(r) || batch_material(t, m, &material, r)) return -1;
            }
            int k = (x + s->phase) / s->scale;
            double u = s->u + k * s->du, v = s->v + k * s->dv;
            int tu = s->mode ? (int)floor(u / 65536.0) : (int)u;
            int tv = s->mode ? (int)floor(v / 65536.0) : (int)v;
            unsigned node = batch_node_count++;
            /* One invocation owns each destination and follows its packet
             * chain in order, preserving overlap/blending without a dispatch
             * and device-wide dependency for every tiny sprite. */
            if (t->tails[destination] == UINT32_MAX) {
                batch_depths[node] = 1;
                batch_pixels[batch_pixel_count * 2] = destination;
                batch_pixels[batch_pixel_count * 2 + 1] = node;
                ++batch_pixel_count;
            } else {
                batch_depths[node] = batch_depths[t->tails[destination]] + 1;
                batch_nodes[t->tails[destination] * 3 + 2] = node;
            }
            t->tails[destination] = node;
            batch_nodes[node * 3] = ((unsigned)tu & 255) | (((unsigned)tv & 255) << 8);
            batch_nodes[node * 3 + 1] = material;
            batch_nodes[node * 3 + 2] = UINT32_MAX;
        }
    }
    return 0;
}

static int configure_frame(request *r) {
    if (!r->frame->pixels || !gex_renderer_valid_size(r->frame->width, r->frame->height) ||
        r->frame->pitch < r->frame->width * 2 ||
        (uint32_t)r->frame->width > bgfx_get_caps()->limits.maxTextureSize ||
        (uint32_t)r->frame->height > bgfx_get_caps()->limits.maxTextureSize)
        return fail(r, "invalid presentation dimensions or pixel storage");
    bgfx_texture_handle_t next = bgfx_create_texture_2d(r->frame->width, r->frame->height,
        false, 1, BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT |
        BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT, NULL, 0);
    if (!VALID(next)) return fail(r, "cannot allocate presentation texture");
    bgfx_frame_buffer_handle_t output = {UINT16_MAX};
    bgfx_texture_handle_t readback = {UINT16_MAX};
    if (!visible) {
        output = bgfx_create_frame_buffer(r->frame->width, r->frame->height,
            BGFX_TEXTURE_FORMAT_RGBA8, BGFX_TEXTURE_RT);
        readback = bgfx_create_texture_2d(r->frame->width, r->frame->height, false, 1,
            BGFX_TEXTURE_FORMAT_RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK, NULL, 0);
        if (!VALID(output) || !VALID(readback)) {
            if (VALID(output)) bgfx_destroy_frame_buffer(output);
            if (VALID(readback)) bgfx_destroy_texture(readback);
            bgfx_destroy_texture(next);
            return fail(r, "cannot allocate headless presentation target");
        }
    }
    if (VALID(frame_texture)) bgfx_destroy_texture(frame_texture);
    if (VALID(frame_output)) bgfx_destroy_frame_buffer(frame_output);
    if (VALID(frame_readback)) bgfx_destroy_texture(frame_readback);
    frame_texture = next;
    frame_output = output;
    frame_readback = readback;
    frame_width = r->frame->width; frame_height = r->frame->height;
    return 0;
}

static int present_frame(request *r) {
    if (submit_batch(r)) return -1;
    const gex_render_frame *f = r->frame;
    if (!f->pixels || f->width != frame_width || f->height != frame_height ||
        f->pitch < f->width * 2)
        return fail(r, "invalid presentation frame");
    if (!visible) { r->width = frame_width; r->height = frame_height; }
    if (r->width <= 0 || r->height <= 0) return 0;
    if (visible && (r->width != window_width || r->height != window_height ||
                    swap_buffers != (gex_prefs.triple ? 3 : 2) ||
                    (r->window && r->window != native_window))) {
        if (r->window) native_window = r->window;
        bgfx_init_t defaults;
        bgfx_init_ctor(&defaults);
        defaults.swapChain.nwh = native_window;
        defaults.swapChain.ndt = native_display;
        defaults.swapChain.width = (uint32_t)r->width;
        defaults.swapChain.height = (uint32_t)r->height;
        swap_buffers = gex_prefs.triple ? 3 : 2;
        defaults.swapChain.numBackBuffers = (uint8_t)swap_buffers;
        defaults.swapChain.maxFrameLatency = (uint8_t)swap_buffers;
        bgfx_reset(BGFX_RESET_VSYNC, &defaults.swapChain);
        window_width = r->width; window_height = r->height;
    }
    const bgfx_memory_t *memory = bgfx_alloc((uint32_t)((size_t)f->width * f->height * 4));
    for (int y = 0; y < f->height; ++y)
        for (int x = 0; x < f->width; ++x) {
            const uint8_t *p = f->pixels + (size_t)y * f->pitch + x * 2;
            unsigned rgb = p[0] | (unsigned)p[1] << 8;
            uint8_t *out = memory->data + ((size_t)y * f->width + x) * 4;
            out[0] = (uint8_t)(((rgb >> 11) * 255) / 31);
            out[1] = (uint8_t)((((rgb >> 5) & 63) * 255) / 63);
            out[2] = (uint8_t)(((rgb & 31) * 255) / 31);
            out[3] = 255;
        }
    bgfx_update_texture_2d(frame_texture, 0, 0, 0, 0, f->width, f->height, memory, UINT16_MAX);
    gex_render_rect rect = gex_renderer_fit(r->width, r->height, f->width, f->height);
    bgfx_set_view_frame_buffer(2, frame_output);
    bgfx_set_view_frame_buffer(3, frame_output);
    bgfx_set_view_rect(2, 0, 0, r->width, r->height, 0, 1);
    bgfx_set_view_clear(2, BGFX_CLEAR_COLOR, 0x000000ff, 1, 0);
    bgfx_touch(2);
    bgfx_set_view_rect(3, rect.x, rect.y, rect.width, rect.height, 0, 1);
    bgfx_set_vertex_buffer(0, vertices, 0, 3);
    bgfx_set_texture(0, sampler, frame_texture, UINT32_MAX);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(3, presenter, 0, BGFX_DISCARD_ALL);
    flush();
    return 0;
}

static int capture_frame(request *r) {
    if (visible || !VALID(frame_output)) return fail(r, "capture requires a headless presentation target");
    if (submit_batch(r)) return -1;
    bgfx_texture_region_t src = {.handle = bgfx_get_texture(frame_output, 0)};
    bgfx_texture_region_t dst = {.handle = frame_readback};
    bgfx_blit(4, &dst, &src);
    uint32_t ready = bgfx_read_texture(&dst, r->rgba), current;
    do { current = flush(); } while ((int32_t)(ready - current) > 0);
    if (bgfx_get_caps()->originBottomLeft)
        for (int y = 0; y < frame_height / 2; ++y)
            for (int x = 0; x < frame_width * 4; ++x) {
                uint8_t *top = r->rgba + (size_t)y * frame_width * 4 + x;
                uint8_t *bottom = r->rgba + (size_t)(frame_height - y - 1) * frame_width * 4 + x;
                uint8_t value = *top; *top = *bottom; *bottom = value;
            }
    return 0;
}

static int execute(request *r) {
    if (!initialised && r->op != OPEN && r->op != CREATE && r->op != STOP)
        return fail(r, "graphics device is not initialised");
    switch (r->op) {
    case OPEN: return open_device(r);
    case CREATE: return create_target(r);
    case DESTROY:
        if (submit_batch(r)) return -1;
        bgfx_destroy_texture(r->device->image);
        bgfx_destroy_texture(r->device->readback);
        free(r->device->tails); free(r->device->transfer); free(r->device);
        return 0;
    case UPLOAD: return upload_target(r);
    case DOWNLOAD: return download_target(r);
    case DRAW: return draw_target(r);
    case MATERIAL:
        return snapshot(&atlas_buffer, atlas_cache, r->material->atlas, sizeof atlas_cache, r) ||
               snapshot(&table_buffer, table_cache, r->material->modulation, sizeof table_cache, r)
               ? -1 : 0;
    case SUBMIT: return submit_batch(r);
    case CONFIGURE: return configure_frame(r);
    case PRESENT: return present_frame(r);
    case CAPTURE: return capture_frame(r);
    case STOP: close_device(); return 0;
    }
    return fail(r, "unknown worker operation");
}

static int worker(void *unused) {
    (void)unused;
    SDL_LockMutex(queue);
    for (;;) {
        while (!pending) SDL_CondWait(work, queue);
        request *r = pending;
        SDL_UnlockMutex(queue);
#if GEX_PROFILE_RENDERER
        uint64_t started = SDL_GetPerformanceCounter();
#endif
        r->result = execute(r);
#if GEX_PROFILE_RENDERER
        profile_ticks[r->op] += SDL_GetPerformanceCounter() - started;
        if (r->op == PRESENT) {
            uint64_t frequency = SDL_GetPerformanceFrequency();
            fprintf(stderr, "[bgfx-cost] draw-us=%llu download-us=%llu upload-us=%llu present-us=%llu\n",
                (unsigned long long)(profile_ticks[DRAW] * 1000000 / frequency),
                (unsigned long long)(profile_ticks[DOWNLOAD] * 1000000 / frequency),
                (unsigned long long)(profile_ticks[UPLOAD] * 1000000 / frequency),
                (unsigned long long)(profile_ticks[PRESENT] * 1000000 / frequency));
            gex_packet_gpu_profile("bgfx", profile_draws, profile_uploads, profile_resolves, profile_bytes);
            profile_draws = profile_uploads = profile_resolves = 0;
            profile_bytes = 0;
            memset(profile_ticks, 0, sizeof profile_ticks);
        }
#endif
        SDL_LockMutex(queue);
        pending = NULL; done = 1;
        SDL_CondSignal(completed);
        if (r->op == STOP) break;
    }
    SDL_UnlockMutex(queue);
    return 0;
}

static int invoke_worker(request *r) {
    SDL_LockMutex(queue);
    pending = r; done = 0;
    SDL_CondSignal(work);
    while (!done) SDL_CondWait(completed, queue);
    SDL_UnlockMutex(queue);
    return r->result;
}

static int invoke(request *r) {
    if (!thread) {
        calls = SDL_CreateMutex(); queue = SDL_CreateMutex();
        work = SDL_CreateCond(); completed = SDL_CreateCond();
        if (calls && queue && work && completed) thread = SDL_CreateThread(worker, "bgfx-api", NULL);
        if (!thread) {
            SDL_DestroyCond(completed); SDL_DestroyCond(work);
            SDL_DestroyMutex(queue); SDL_DestroyMutex(calls);
            calls = queue = NULL; work = completed = NULL;
            return fail(r, "cannot create the graphics API worker");
        }
    }
    /* Guest workers can migrate; bgfx's API always stays on this one thread. */
    SDL_LockMutex(calls);
    int result = invoke_worker(r);
    SDL_UnlockMutex(calls);
    return result;
}

static void *create(int width, int height, char *error, size_t size) {
    request r = {.op = CREATE, .width = width, .height = height, .error = error, .size = size};
    return invoke(&r) ? NULL : r.device;
}
static void destroy(void *device) {
    char error[512];
    request r = {.op = DESTROY, .device = device, .error = error, .size = sizeof error};
    if (invoke(&r)) { fprintf(stderr, "[bgfx] %s\n", error); abort(); }
}
static int upload(void *device, const uint16_t *pixels, char *error, size_t size) {
    request r = {.op = UPLOAD, .device = device, .input = pixels, .error = error, .size = size};
    return invoke(&r);
}
static int download(void *device, uint16_t *pixels, char *error, size_t size) {
    request r = {.op = DOWNLOAD, .device = device, .output = pixels, .error = error, .size = size};
    return invoke(&r);
}
static int draw(void *device, const gex_gpu_span *spans, size_t count,
                const gex_gpu_material *material, char *error, size_t size) {
    request r = {.op = DRAW, .device = device, .spans = spans, .count = count,
                 .material = material, .error = error, .size = size};
    SDL_LockMutex(calls);
#if GEX_PROFILE_RENDERER
    uint64_t started = SDL_GetPerformanceCounter();
#endif
    unsigned code = material->opcode & ~2u;
    int result = 0;
    if ((code == 0x2c || code == 0x64) && material_changed(material)) {
        r.op = MATERIAL;
        result = invoke_worker(&r);
    }
    if (!result) result = draw_target(&r);
#if GEX_PROFILE_RENDERER
    profile_ticks[DRAW] += SDL_GetPerformanceCounter() - started;
#endif
    SDL_UnlockMutex(calls);
    return result;
}

int gex_bgfx_open(SDL_Window *window, char *error, size_t size) {
    if (thread) {
        snprintf(error, size, "bgfx: renderer must open before packet targets");
        return -1;
    }
    memset(&platform, 0, sizeof platform);
    native_window = native_display = NULL;
    window_width = window_height = 0;
    visible = window != NULL;
#ifdef __ANDROID__
    presentation_window = window;
#endif
    if (window) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) {
            snprintf(error, size, "bgfx native window handle: %s", SDL_GetError());
            return -1;
        }
        switch (info.subsystem) {
#if defined(SDL_VIDEO_DRIVER_X11)
        case SDL_SYSWM_X11:
            native_display = info.info.x11.display;
            native_window = (void *)(uintptr_t)info.info.x11.window;
            break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            native_display = info.info.wl.display;
            native_window = info.info.wl.surface;
            platform.type = BGFX_NATIVE_WINDOW_HANDLE_TYPE_WAYLAND;
            break;
#endif
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
        case SDL_SYSWM_WINDOWS: native_window = info.info.win.window; break;
#endif
#if defined(SDL_VIDEO_DRIVER_COCOA)
        case SDL_SYSWM_COCOA: native_window = info.info.cocoa.window; break;
#endif
#if defined(SDL_VIDEO_DRIVER_ANDROID)
        case SDL_SYSWM_ANDROID: native_window = info.info.android.window; break;
#endif
        default:
            snprintf(error, size, "bgfx: unsupported SDL window subsystem %d", info.subsystem);
            return -1;
        }
        SDL_GetWindowSizeInPixels(window, &window_width, &window_height);
    }
    request r = {.op = OPEN, .error = error, .size = size};
    return invoke(&r);
}

int gex_bgfx_configure(const gex_render_frame *frame, char *error, size_t size) {
    request r = {.op = CONFIGURE, .frame = frame, .error = error, .size = size};
    return invoke(&r);
}
int gex_bgfx_present(const gex_render_frame *frame, int width, int height,
                     char *error, size_t size) {
    request r = {.op = PRESENT, .frame = frame, .width = width, .height = height,
                 .error = error, .size = size};
#ifdef __ANDROID__
    if (visible) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!presentation_window || !SDL_GetWindowWMInfo(presentation_window, &info) ||
            info.subsystem != SDL_SYSWM_ANDROID || !info.info.android.window) {
            snprintf(error, size, "bgfx: cannot recover Android presentation surface: %s", SDL_GetError());
            return -1;
        }
        r.window = info.info.android.window;
    }
#endif
    return invoke(&r);
}
int gex_bgfx_read_present(uint8_t *rgba, char *error, size_t size) {
    request r = {.op = CAPTURE, .rgba = rgba, .error = error, .size = size};
    return invoke(&r);
}
void gex_packet_gpu_shutdown_bgfx(void) {
    if (!thread) return;
    char error[512];
    request r = {.op = STOP, .error = error, .size = sizeof error};
    if (invoke(&r)) { fprintf(stderr, "[bgfx] %s\n", error); abort(); }
    SDL_WaitThread(thread, NULL); thread = NULL;
    SDL_DestroyCond(completed); SDL_DestroyCond(work);
    SDL_DestroyMutex(queue); SDL_DestroyMutex(calls);
    calls = queue = NULL; work = completed = NULL;
    memset(&platform, 0, sizeof platform);
    native_window = native_display = NULL;
#ifdef __ANDROID__
    presentation_window = NULL;
#endif
    visible = 0;
}
const gex_packet_gpu *gex_packet_gpu_bgfx(void) {
    static const gex_packet_gpu api = {
        .create = create, .destroy = destroy, .upload = upload, .download = download, .draw = draw
    };
    return &api;
}
