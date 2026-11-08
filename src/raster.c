#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "renderer.h"
#include "raster.h"

struct gex_raster {
    int width, height, valid;
    uint16_t *pixels;
    uint16_t *previous;
    int device_history;
    int previous_pixels;
    const gex_packet_gpu *gpu;
    void *device;
    int gpu_newer, cpu_newer, guest_newer;
    gex_gpu_span *spans;
    size_t span_count, span_capacity;
    gex_gpu_material material;
    gex_render_rect fitted;
};

typedef struct { double x, y, u, v; } vertex;

static unsigned word(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static int coordinate(const uint8_t *p) {
    unsigned value = word(p);
    return value < 32768 ? (int)value : (int)value - 65536;
}
static int maximum(int a, int b) { return a > b ? a : b; }
static int minimum(int a, int b) { return a < b ? a : b; }
static int fixed_integer(int64_t value) {
    return (int)(value >= 0 ? value / 65536 : -((-value + 65535) / 65536));
}
static int ceiling(double value) {
    int result = (int)value;
    return result + (value > result);
}

static void gpu_fail(const char *error) {
    fprintf(stderr, "[packet-gpu] %s\n", error);
    abort();
}

static void materialize(gex_raster *r) {
    if (gex_raster_gpu_owned(r)) gpu_fail("GPU-owned framebuffer readback is forbidden");
    char error[512] = {0};
    if (r->gpu_newer) {
        r->previous_pixels = 0;
        if (r->gpu->download(r->device, r->pixels, error, sizeof error)) gpu_fail(error);
    }
    r->gpu_newer = 0;
}

int gex_raster_enable_gpu(gex_raster *r, const gex_packet_gpu *gpu) {
    char error[512] = {0};
    if (!gpu || r->gpu) return -1;
    if (gpu->native && (!gpu->native->clear || !gpu->native->copy || !gpu->native->blit ||
        !gpu->native->present || !gpu->native->bind || !gpu->native->movie)) {
        fprintf(stderr, "[packet-gpu] incomplete GPU-owned target operations\n");
        return -1;
    }
    void *device = gpu->create(r->width, r->height, error, sizeof error);
    if (!device) {
        fprintf(stderr, "[packet-gpu] %s\n", error);
        return -1;
    }
    if (gpu->history && !gpu->native) {
        uint16_t *previous = gpu->history(device, 320 * 224 * sizeof *previous, error, sizeof error);
        if (!previous) {
            fprintf(stderr, "[packet-gpu] %s\n", error);
            gpu->destroy(device);
            return -1;
        }
        memcpy(previous, r->previous, 320 * 224 * sizeof *previous);
        free(r->previous);
        r->previous = previous;
        r->device_history = 1;
    }
    r->gpu = gpu;
    r->device = device;
    r->cpu_newer = !gpu->native;
    if (gpu->native) {
        free(r->pixels); r->pixels = NULL;
        free(r->previous); r->previous = NULL;
    }
    return 0;
}

void gex_raster_disable_gpu(gex_raster *r) {
    if (!r->gpu) return;
    if (!gex_raster_gpu_owned(r)) materialize(r);
    else {
        r->pixels = calloc((size_t)r->width * r->height, sizeof *r->pixels);
        r->previous = calloc(320 * 224, sizeof *r->previous);
        if (!r->pixels || !r->previous) gpu_fail("cannot restore a CPU target after device teardown");
        r->gpu_newer = r->guest_newer = r->valid = 0;
    }
    if (r->device_history) {
        uint16_t *previous = malloc(320 * 224 * sizeof *previous);
        if (!previous) gpu_fail("cannot restore CPU framebuffer history");
        memcpy(previous, r->previous, 320 * 224 * sizeof *previous);
        r->previous = previous;
        r->device_history = 0;
    }
    r->gpu->destroy(r->device);
    r->gpu = NULL;
    r->device = NULL;
    r->cpu_newer = 0;
}

static void gpu_submit(gex_raster *r) {
    if (!r->gpu || !r->span_count) return;
    char error[512] = {0};
    if (r->cpu_newer && r->gpu->upload(r->device, r->pixels, error, sizeof error))
        gpu_fail(error);
    r->cpu_newer = 0;
    if (r->gpu->draw(r->device, r->spans, r->span_count, &r->material, error, sizeof error))
        gpu_fail(error);
    r->gpu_newer = 1;
    r->guest_newer = 1;
    r->span_count = 0;
}

int gex_raster_gpu_owned(const gex_raster *r) { return r && r->gpu && r->gpu->native; }

void gex_raster_bind(gex_raster *r, const uint8_t *atlas, unsigned plane) {
    if (gex_raster_gpu_owned(r)) r->gpu->native->bind(r->device, atlas, plane);
}

void gex_raster_blit(gex_raster *r, int x, int y, int width, int height,
                     const uint8_t *pixels, int pitch, int key) {
    char error[512] = {0};
    if (!gex_raster_gpu_owned(r)) gpu_fail("asset blit requires a GPU-owned target");
    if (r->gpu->native->blit(r->device, x, y, width, height, pixels, pitch, key,
                             error, sizeof error)) gpu_fail(error);
}

void gex_raster_present(gex_raster *r) {
    char error[512] = {0};
    if (!gex_raster_gpu_owned(r)) gpu_fail("device presentation requires a GPU-owned target");
    if (r->gpu->native->present(r->device, error, sizeof error)) gpu_fail(error);
    if (gex_renderer_device_commit()) gpu_fail(gex_renderer_error());
}

void gex_raster_fill(gex_raster *r, int x, int y, int width, int height, unsigned rgb) {
    if (!gex_raster_gpu_owned(r)) {
        gpu_submit(r);
        materialize(r);
        unsigned value = ((rgb >> 3) & 31) | ((rgb >> 6) & 0x3E0) | ((rgb >> 9) & 0x7C00);
        for (int row = maximum(0, y); row < minimum(r->height, y + height); ++row)
            for (int column = maximum(0, x); column < minimum(r->width, x + width); ++column)
                r->pixels[row * r->width + column] = (uint16_t)value;
        r->previous_pixels = 0;
        r->cpu_newer = r->guest_newer = 1;
        return;
    }
    gex_gpu_material m = {.opcode = 0x60, .rgb = rgb};
    char error[512] = {0};
    if (r->gpu->native->polygon) {
        gex_gpu_vertex v[] = {{x, y, 0, 0}, {x + width, y, 0, 0},
                             {x, y + height, 0, 0}, {x + width, y + height, 0, 0}};
        if (r->gpu->native->polygon(r->device, v, 0, 0, r->width, r->height,
                                    &m, error, sizeof error)) gpu_fail(error);
    } else {
        int left = maximum(0, x), top = maximum(0, y);
        int right = minimum(r->width, x + width), bottom = minimum(r->height, y + height);
        if (left >= right || top >= bottom) return;
        size_t count = (size_t)(bottom - top);
        gex_gpu_span *spans = malloc(count * sizeof *spans);
        if (!spans) gpu_fail("cannot allocate GPU fill coverage");
        for (size_t i = 0; i < count; ++i)
            spans[i] = (gex_gpu_span){.x = left, .y = top + (int)i,
                                      .count = right - left, .scale = 1};
        if (r->gpu->draw(r->device, spans, count, &m, error, sizeof error)) {
            free(spans);
            gpu_fail(error);
        }
        free(spans);
        r->gpu_newer = r->guest_newer = 1;
    }
}

void gex_raster_movie(gex_raster *r, const uint8_t *pixels, int width, int height, int pitch) {
    if (!gex_raster_gpu_owned(r)) gpu_fail("device movie requires a GPU-owned target");
    char error[512] = {0};
    if (r->gpu->native->movie(r->device, pixels, width, height, pitch,
                              error, sizeof error)) gpu_fail(error);
    if (gex_renderer_device_commit()) gpu_fail(gex_renderer_error());
}

static void gpu_span_emit(gex_raster *r, const gex_raster_state *s, int x, int y,
                     int count, int mode, double u, double v, double du, double dv) {
    int scale = s->pixel_scale ? s->pixel_scale : 1;
    int offset = s->pixel_scale ? s->offset_x : 0;
    int left = maximum(0, x * scale + offset);
    int right = minimum(r->width, (x + count) * scale + offset);
    if (left >= right) return;
    for (int row = maximum(0, y * scale); row < minimum(r->height, (y + 1) * scale); ++row) {
#ifdef GEX_PACKET_SPAN_LIMIT
        if (r->span_count == GEX_PACKET_SPAN_LIMIT) gpu_submit(r);
#endif
        if (r->span_count == r->span_capacity) {
            size_t capacity = r->span_capacity ? r->span_capacity * 2 : 256;
            void *next = realloc(r->spans, capacity * sizeof *r->spans);
            if (!next) gpu_fail("cannot allocate packet coverage");
            r->spans = next;
            r->span_capacity = capacity;
        }
        r->spans[r->span_count++] = (gex_gpu_span){
            left, row, right - left, mode, u, v, du, dv, scale,
            left - (x * scale + offset), {0, 0}
        };
    }
}

static int fitted_x(const gex_raster *r, int x) {
    return r->fitted.x + x * r->fitted.width / 320;
}

static int gpu_rectangle(gex_raster *r, const gex_raster_state *s,
                         int x, int y, int width, int height, int mode,
                         double u, double v, double du, int row_step) {
    if (!r->gpu->rectangle_spans || s->pixel_scale ||
        (r->fitted.width && (r->fitted.width != 320 || r->fitted.height != 224)))
        return 0;
    if (width <= 0 || height <= 0) return 1;
    gpu_span_emit(r, s, x + (r->fitted.width ? r->fitted.x : 0),
                  y + (r->fitted.width ? r->fitted.y : 0),
                  width, mode, u, v, du, 0);
    if (r->span_count) {
        gex_gpu_span *span = &r->spans[r->span_count - 1];
        span->reserved[0] = height;
        span->reserved[1] = row_step;
    }
    return 1;
}

static int fitted_y(const gex_raster *r, int y) {
    return r->fitted.y + y * r->fitted.height / 224;
}

static void gpu_span(gex_raster *r, const gex_raster_state *s, int x, int y,
                     int count, int mode, double u, double v, double du, double dv) {
    if (!r->fitted.width) {
        gpu_span_emit(r, s, x, y, count, mode, u, v, du, dv);
        return;
    }
    if (r->fitted.width == 320 && r->fitted.height == 224) {
        gpu_span_emit(r, s, x + r->fitted.x, y + r->fitted.y,
                      count, mode, u, v, du, dv);
        return;
    }
    int top = fitted_y(r, y), bottom = fitted_y(r, y + 1);
    for (int i = 0; i < count;) {
        int run = mode == 0 && du == 0 && dv == 0 ? count - i : 1;
        int left = fitted_x(r, x + i), right = fitted_x(r, x + i + run);
        double tu = mode == 1 ? fixed_integer((int64_t)(u + i * du)) : u + i * du;
        double tv = mode == 1 ? fixed_integer((int64_t)(v + i * dv)) : v + i * dv;
        for (int row = top; row < bottom; ++row)
            gpu_span_emit(r, s, left, row, right - left, 0, tu, tv, 0, 0);
        i += run;
    }
}

gex_raster *gex_raster_create(int width, int height) {
    if (!gex_renderer_valid_size(width, height)) return NULL;
    gex_raster *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->pixels = calloc((size_t)width * height, sizeof *r->pixels);
    r->previous = calloc(320 * 224, sizeof *r->previous);
    if (!r->pixels || !r->previous) {
        free(r->pixels); free(r->previous); free(r); return NULL;
    }
    r->width = width; r->height = height;
    return r;
}

void gex_raster_destroy(gex_raster *r) {
    if (r) {
        if (r->gpu) r->gpu->destroy(r->device);
        if (!r->device_history) free(r->previous);
        free(r->spans); free(r->pixels); free(r);
    }
}

const uint8_t *gex_raster_pixels(const gex_raster *r) {
    materialize((gex_raster *)r);
    return (const uint8_t *)r->pixels;
}

void gex_raster_clear(gex_raster *r) {
    if (gex_raster_gpu_owned(r)) {
        char error[512] = {0};
        if (r->gpu->native->clear(r->device, error, sizeof error)) gpu_fail(error);
        return;
    }
    r->previous_pixels = 0;
    memset(r->pixels, 0, (size_t)r->width * r->height * sizeof *r->pixels);
    r->gpu_newer = 0;
    r->cpu_newer = 1;
    r->guest_newer = 1;
}

void gex_raster_copy(gex_raster *destination, const gex_raster *source) {
    gex_raster_copy_shaded(destination, source, 255);
}

void gex_raster_copy_shaded(gex_raster *destination, const gex_raster *source, unsigned shade) {
    if (shade > 255) gpu_fail("invalid picture copy shade");
    if (gex_raster_gpu_owned(destination) && gex_raster_gpu_owned(source)) {
        char error[512] = {0};
        if (destination->gpu->native->copy(destination->device, source->device, shade,
                                            error, sizeof error)) gpu_fail(error);
        return;
    }
    materialize((gex_raster *)source);
    destination->previous_pixels = 0;
    memcpy(destination->pixels, source->pixels,
           (size_t)destination->width * destination->height * sizeof *destination->pixels);
    if (shade < 255)
        for (int i = 0; i < destination->width * destination->height; ++i) {
            unsigned value = destination->pixels[i];
            destination->pixels[i] = (uint16_t)((value & 0x8000) |
                ((value & 31) * shade / 255) |
                (((value >> 5 & 31) * shade / 255) << 5) |
                (((value >> 10 & 31) * shade / 255) << 10));
        }
    destination->gpu_newer = 0;
    destination->cpu_newer = 1;
    destination->guest_newer = 1;
}

void gex_raster_pause(gex_raster *destination, const gex_raster *source, const uint8_t *banner) {
    if (gex_raster_gpu_owned(destination) && gex_raster_gpu_owned(source)) {
        char error[512] = {0};
        if (destination->gpu->native->copy(destination->device, source->device, 127,
                                            error, sizeof error)) gpu_fail(error);
        gex_raster_blit(destination, 91, 106, 138, 27, banner, 138 * 2, (int)word(banner));
        return;
    }
    materialize((gex_raster *)source);
    destination->previous_pixels = 0;
    destination->gpu_newer = 0;
    destination->cpu_newer = 1;
    destination->guest_newer = 1;
    for (int i = 0; i < source->width * source->height; ++i)
        destination->pixels[i] = (uint16_t)((source->pixels[i] & 0x7BDE) >> 1);
    gex_render_rect canvas = gex_renderer_fit(source->width, source->height, 320, 224);
    int left = canvas.x + 91 * canvas.width / 320;
    int right = canvas.x + 229 * canvas.width / 320;
    int top = canvas.y + 98 * canvas.height / 224;
    int bottom = canvas.y + 125 * canvas.height / 224;
    unsigned key = word(banner);
    for (int y = top; y < bottom; ++y)
        for (int x = left; x < right; ++x) {
            int sx = (x - left) * 138 / (right - left);
            int sy = (y - top) * 27 / (bottom - top);
            unsigned color = word(banner + (sy * 138 + sx) * 2);
            if (color != key) destination->pixels[y * source->width + x] = (uint16_t)color;
        }
}

void gex_raster_remember(gex_raster *r, const uint8_t *picture, int pitch) {
    if (gex_raster_gpu_owned(r)) gpu_fail("GPU-owned framebuffer history is forbidden");
    r->previous_pixels = 0;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    for (int y = 0; y < 224; ++y)
        memcpy(r->previous + y * 320, picture + y * pitch, 320 * sizeof *r->previous);
#else
    for (int y = 0; y < 224; ++y)
        for (int x = 0; x < 320; ++x)
            r->previous[y * 320 + x] = (uint16_t)word(picture + y * pitch + x * 2);
#endif
    r->valid = 1;
}

void gex_raster_sync(gex_raster *r, const uint8_t *picture, int pitch) {
    if (gex_raster_gpu_owned(r)) gpu_fail("GPU-owned framebuffer CPU import is forbidden");
    for (int y = 0; y < 224; ++y) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        /* Until the next native pixel write, an exported 320-wide CPU image
         * is also the history. Avoid reading uncached device history then. */
        const uint16_t *previous = r->previous_pixels ? r->pixels : r->previous;
        if (r->valid && !memcmp(previous + y * 320, picture + y * pitch,
                               320 * sizeof *r->previous)) continue;
#endif
        for (int x = 0; x < 320; ++x) {
            unsigned value = word(picture + y * pitch + x * 2);
            if (r->valid && r->previous[y * 320 + x] == value) continue;
            if (x < r->width && y < r->height) {
                materialize(r);
                r->pixels[y * r->width + x] = (uint16_t)value;
                r->cpu_newer = 1;
            }
            r->previous[y * 320 + x] = (uint16_t)value;
        }
    }
    r->valid = 1;
}

void gex_raster_export(gex_raster *r, uint8_t *picture, int pitch) {
    if (gex_raster_gpu_owned(r)) gpu_fail("GPU-owned framebuffer CPU export is forbidden");
    if (!r->guest_newer) return;
    materialize(r);
    int width = minimum(320, r->width), height = minimum(224, r->height);
    for (int y = 0; y < height; ++y)
        memcpy(picture + y * pitch, r->pixels + y * r->width,
               (size_t)width * sizeof *r->pixels);
    gex_raster_remember(r, picture, pitch);
    r->previous_pixels = r->width == 320 && r->height >= 224;
    r->guest_newer = 0;
}

#if GEX_ENABLE_SOFTWARE_RENDERER
static unsigned modulate(unsigned color, const uint8_t *packet,
                         const uint8_t *table, int retain_mask) {
    unsigned result = retain_mask ? color & 0x8000 : 0;
    for (int component = 0; component < 3; ++component)
        result |= (unsigned)table[((packet[4 + component] >> 3) << 5) |
                                  ((color >> (component * 5)) & 31)] << (component * 5);
    return result;
}

static void native_span(gex_raster *r, int x, int y, int count, unsigned color, int blend) {
    r->previous_pixels = 0;
    r->guest_newer = 1;
    uint16_t *target = r->pixels + y * r->width + x;
    if (blend) {
        unsigned foreground = (color >> 1) & 0x3DEF;
        for (int i = 0; i < count; ++i)
            target[i] = (uint16_t)(foreground + ((target[i] >> 1) & 0x3DEF));
    } else {
        for (int i = 0; i < count; ++i) target[i] = (uint16_t)color;
    }
}

static void span(gex_raster *r, const gex_raster_state *s, int x, int y,
                 int count, unsigned color, int blend) {
    if (r->fitted.width) {
        int left = fitted_x(r, x), right = fitted_x(r, x + count);
        int top = fitted_y(r, y), bottom = fitted_y(r, y + 1);
        for (int row = top; row < bottom; ++row)
            native_span(r, left, row, right - left, color, blend);
        return;
    }
    if (!s->pixel_scale) {
        native_span(r, x, y, count, color, blend);
        return;
    }
    int scale = s->pixel_scale ? s->pixel_scale : 1;
    int left = maximum(0, x * scale + s->offset_x);
    int right = minimum(r->width, (x + count) * scale + s->offset_x);
    if (left >= right) return;
    for (int row = maximum(0, y * scale); row < minimum(r->height, (y + 1) * scale); ++row)
        native_span(r, left, row, right - left, color, blend);
}

static unsigned texel(const gex_raster_state *s, unsigned page, int u, int v,
                       const unsigned *palette) {
    unsigned format = (page >> 7) & 3;
    unsigned base = (page & 15) * 128 + ((page & 16) ? 0x80000 : 0);
    unsigned offset = base + (v & 255) * 2048;
    u &= 255;
    if (format == 0) {
        unsigned packed = s->atlas[(offset + (unsigned)u / 2) & 0xFFFFF];
        return palette[(packed >> ((u & 1) * 4)) & 15];
    }
    if (format == 1) return palette[s->atlas[(offset + u) & 0xFFFFF]];
    return word(s->atlas + ((offset + u * 2) & 0xFFFFE));
}
#endif

#if defined(GEX_NATIVE_GAMEPLAY_WIDTH) && defined(__GNUC__)
__attribute__((noinline))
#endif
static void raster_packet_decode(gex_raster *r, const uint8_t *p, const gex_raster_state *s,
                           int offset_x, int offset_y) {
    r->fitted.width = 0;
    int width = s->pixel_scale ? 320 : r->width;
    int height = s->pixel_scale ? 224 : r->height;
    unsigned code = p[7] & ~2u, page = s->page;
    int textured = code == 0x2C || code == 0x64;
    if (!textured && code != 0x60 && code != 0x68 && code != 0x70 && code != 0x78)
        return; /* The original dispatcher ignores these opcodes too. */
    struct { int x, y, u, v; } q[4];
#if GEX_ENABLE_SOFTWARE_RENDERER
    unsigned palette[256];
#endif
    int quad = code == 0x2C, full_picture = 0;
    if (quad) {
        page = word(p + 22);
        for (int i = 0; i < 4; ++i) {
            q[i].x = coordinate(p + 8 + i * 8);
            q[i].y = coordinate(p + 10 + i * 8);
            q[i].u = p[12 + i * 8];
            q[i].v = p[13 + i * 8];
        }
        /* Zero marks the far edge of a 256-texel page, not a wrapped start. */
        if (!p[20]) q[1].u = 256;
        if (!p[29]) q[2].v = 256;
        if (!p[36]) q[3].u = 256;
        if (!p[37]) q[3].v = 256;
    } else {
        int x = coordinate(p + 8), y = coordinate(p + 10);
        int w = code == 0x68 ? 1 : code == 0x70 ? 8 : code == 0x78 ? 16 :
                coordinate(p + (textured ? 16 : 12));
        int h = code == 0x68 ? 1 : code == 0x70 ? 8 : code == 0x78 ? 16 :
                coordinate(p + (textured ? 18 : 14));
        if (w <= 0 || h <= 0) return;
        /* Full-picture solid packets implement clears and fades, not scenery. */
        if (!textured && x == 0 && w == 320 &&
            ((y == s->origin_y && h == 240) ||
             (y == s->origin_y + 8 && h == 224))) {
            full_picture = 1;
            if (!s->layout) {
                w = width;
                h = height + (y == s->origin_y ? 16 : 0);
            }
        }
        int u = textured ? p[12] : 0, v = textured ? p[13] : 0;
        for (unsigned i = 0; i < 4; ++i) {
            q[i].x = x + (i & 1 ? w : 0);
            q[i].y = y + (i & 2 ? h : 0);
            q[i].u = u + (i & 1 ? w : 0);
            q[i].v = v + (i & 2 ? h : 0);
        }
    }
    /* Fit the native pixel grid, not each CEL piece's texture sampling. */
    if (s->layout == 1 && !full_picture && !s->pixel_scale) {
        r->fitted = gex_renderer_fit(r->width, r->height, 320, 224);
        width = 320;
        height = 224;
    }
    unsigned format = (page >> 7) & 3;
    if (textured && format == 3) return;
#if GEX_ENABLE_SOFTWARE_RENDERER
    if (!r->gpu && textured && format < 2) {
        unsigned address = word(p + 14) * 32;
        int count = format == 0 ? 16 : 256;
        for (int i = 0; i < count; ++i) {
            unsigned color = word(s->atlas + ((address + i * 2) & 0xFFFFE));
            palette[i] = modulate(color, p, s->modulation, 1);
        }
    }
    unsigned solid = (p[4] >> 3) | ((p[5] >> 3) << 5) | ((p[6] >> 3) << 10);
#endif
    if (r->gpu)
        r->material = (gex_gpu_material){page, textured ? word(p + 14) : 0, p[7],
            p[4] | (unsigned)p[5] << 8 | (unsigned)p[6] << 16, s->atlas, s->modulation};
    int left = maximum(0, s->left);
    int right = minimum(width, s->right == 320 ? width : s->right);
    int top = maximum(8, s->top - s->origin_y);
    int bottom = s->bottom - s->origin_y;
    bottom = minimum(height + 8,
                     bottom == 232 || bottom == 240 ? height + 8 : bottom);
    if (s->layout == 2 && !full_picture) {
        left = maximum(0, s->left);
        right = minimum(320, s->right);
        top = maximum(8, s->top - s->origin_y);
        bottom = minimum(232, s->bottom - s->origin_y);
    } else if (s->layout && !r->fitted.width) {
        gex_render_rect rect = full_picture ?
            (gex_render_rect){0, 0, r->width, r->height} :
            gex_renderer_fit(r->width, r->height, 320, 224);
        for (int i = 0; i < 4; ++i) {
            q[i].x = rect.x + fixed_integer((int64_t)q[i].x * rect.width * 65536 / 320);
            q[i].y = s->origin_y + 8 + rect.y + fixed_integer(
                (int64_t)(q[i].y - s->origin_y - 8) * rect.height * 65536 / 224);
        }
        left = maximum(0, rect.x + s->left * rect.width / 320);
        right = minimum(r->width, rect.x + s->right * rect.width / 320);
        top = maximum(8, 8 + rect.y + (s->top - s->origin_y - 8) * rect.height / 224);
        bottom = minimum(r->height + 8,
                         8 + rect.y + (s->bottom - s->origin_y - 8) * rect.height / 224);
    }
    if (s->world_clip && !s->layout && !s->pixel_scale) {
        left = maximum(left, s->world_left);
        right = minimum(right, s->world_right);
        top = maximum(top, s->world_top);
        bottom = minimum(bottom, s->world_bottom);
    }
    if (left >= right || top >= bottom) return;
    for (int i = 0; i < 4; ++i) {
        q[i].x += offset_x;
        q[i].y += offset_y - s->origin_y - 8;
    }
    int x0 = maximum(0, left + offset_x), x1 = minimum(width, right + offset_x);
    int y0 = maximum(0, top - 8 + offset_y), y1 = minimum(height, bottom - 8 + offset_y);
    if (gex_raster_gpu_owned(r) && r->gpu->native->polygon) {
        int scale = s->pixel_scale ? s->pixel_scale : 1;
        int ox = s->pixel_scale ? s->offset_x : r->fitted.width ? r->fitted.x : 0;
        int oy = r->fitted.width ? r->fitted.y : 0;
        gex_gpu_vertex vertices[4];
        for (int i = 0; i < 4; ++i)
            vertices[i] = (gex_gpu_vertex){(float)(q[i].x * scale + ox),
                (float)(q[i].y * scale + oy), (float)q[i].u, (float)q[i].v};
        char error[512] = {0};
        if (r->gpu->native->polygon(r->device, vertices,
                maximum(0, x0 * scale + ox), maximum(0, y0 * scale + oy),
                minimum(r->width, x1 * scale + ox), minimum(r->height, y1 * scale + oy),
                &r->material, error, sizeof error)) gpu_fail(error);
        r->guest_newer = r->gpu_newer = 1;
        return;
    }
    if (r->gpu && !quad && (!textured ||
        (q[3].x - q[0].x == q[3].u - q[0].u &&
         q[3].y - q[0].y == q[3].v - q[0].v))) {
        int ax = (int)q[0].x, ay = (int)q[0].y;
        x0 = maximum(x0, ax); x1 = minimum(x1, (int)q[3].x);
        y0 = maximum(y0, ay); y1 = minimum(y1, (int)q[3].y);
        if (gpu_rectangle(r, s, x0, y0, x1 - x0, y1 - y0, textured ? 1 : 0,
                          textured ? (q[0].u + x0 - ax) * 65536.0 : 0,
                          textured ? (q[0].v + y0 - ay) * 65536.0 : 0,
                          textured ? 65536 : 0, textured ? 65536 : 0))
            return;
        for (int y = y0; y < y1; ++y)
            gpu_span(r, s, x0, y, maximum(0, x1 - x0), textured ? 1 : 0,
                     textured ? (q[0].u + x0 - ax) * 65536.0 : 0,
                     textured ? (q[0].v + y - ay) * 65536.0 : 0,
                     textured ? 65536 : 0, 0);
        return;
    }
    if (quad && q[0].x == q[2].x && q[1].x == q[3].x &&
        q[0].y == q[1].y && q[2].y == q[3].y) {
        int ax = (int)q[0].x, ay = (int)q[0].y;
        int bx = (int)q[3].x, by = (int)q[3].y;
        int width = abs(bx - ax), height = abs(by - ay);
        if (!width || !height) return;
        int flip_x = ax > bx, flip_y = ay > by;
        int64_t du = ((int64_t)q[3].u - (int)q[0].u) * 65536 / width;
        int64_t dv = ((int64_t)q[3].v - (int)q[0].v) * 65536 / height;
        x0 = maximum(x0, minimum(ax, bx)); x1 = minimum(x1, maximum(ax, bx));
        y0 = maximum(y0, minimum(ay, by)); y1 = minimum(y1, maximum(ay, by));
        if (r->gpu && !flip_y && dv == 65536) {
            int column = flip_x ? ax - 1 - x0 : x0 - ax;
            int64_t u = ((int)q[0].u + flip_x) * INT64_C(65536) + column * du;
            if (gpu_rectangle(r, s, x0, y0, x1 - x0, y1 - y0, 1,
                              (double)u, ((int)q[0].v + y0 - ay) * 65536.0,
                              (double)(flip_x ? -du : du), 65536))
                return;
        }
        /* The original rectangle path samples integer texels, starting one
         * texel later on a mirrored axis. Each CEL piece uses its own origin. */
        for (int y = y0; y < y1; ++y) {
            int row = flip_y ? ay - 1 - y : y - ay;
            int v = (int)q[0].v + flip_y + fixed_integer(row * dv);
            int column = flip_x ? ax - 1 - x0 : x0 - ax;
            int64_t u = ((int)q[0].u + flip_x) * INT64_C(65536) + column * du;
            if (r->gpu) {
                gpu_span(r, s, x0, y, maximum(0, x1 - x0), 1,
                         (double)u, v * 65536.0, (double)(flip_x ? -du : du), 0);
                continue;
            }
#if GEX_ENABLE_SOFTWARE_RENDERER
            for (int x = x0; x < x1; ++x) {
                unsigned color = texel(s, page, fixed_integer(u), v, palette);
                if (color)
                    span(r, s, x, y, 1, color, (p[7] & 2) && (color & 0x8000));
                u += flip_x ? -du : du;
            }
#endif

        }
        return;
    }
    double min_y = q[0].y, max_y = q[0].y;
    for (int i = 1; i < 4; ++i) {
        if (q[i].y < min_y) min_y = q[i].y;
        if (q[i].y > max_y) max_y = q[i].y;
    }
    y0 = maximum(y0, ceiling(min_y - 0.5)); y1 = minimum(y1, ceiling(max_y - 0.5));
    static const int order[] = {0, 1, 3, 2, 0};
    for (int y = y0; y < y1; ++y) {
        vertex edges[4];
        int count = 0;
        double sample_y = y + 0.5;
        for (int i = 0; i < 4; ++i) {
            unsigned ai = order[i], bi = order[i + 1];
            vertex a = {q[ai].x, q[ai].y, q[ai].u, q[ai].v};
            vertex b = {q[bi].x, q[bi].y, q[bi].u, q[bi].v};
            if (!((a.y <= sample_y && b.y > sample_y) ||
                  (b.y <= sample_y && a.y > sample_y))) continue;
            double t = (sample_y - a.y) / (b.y - a.y);
            edges[count++] = (vertex){a.x + (b.x - a.x) * t, sample_y,
                a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
        }
        if (count != 2) continue;
        vertex a = edges[0], b = edges[1];
        if (a.x > b.x) { vertex swap = a; a = b; b = swap; }
        int start = maximum(x0, ceiling(a.x - 0.5));
        int end = minimum(x1, ceiling(b.x - 0.5));
        if (start >= end) continue;
        double du = (b.u - a.u) / (b.x - a.x), dv = (b.v - a.v) / (b.x - a.x);
        double u = a.u + (start + 0.5 - a.x) * du;
        double v = a.v + (start + 0.5 - a.x) * dv;
        if (r->gpu && textured && !quad && du == 1 && dv == 0) {
            gpu_span(r, s, start, y, end - start, 1,
                     (int)u * 65536.0, (int)v * 65536.0, 65536, 0);
            continue;
        }
        for (int x = start; x < end;) {
            int count = 1;
            /* Sample repeated texels once when the game scales a sprite. */
            if (!textured || (du == 0 && dv == 0)) count = end - x;
            else if (dv == 0 && du > 0) {
                count = maximum(1, ceiling(((int)u + 1 - u) / du));
                count = minimum(count, end - x);
            }
            if (r->gpu) {
                /* Preserve the reference's texel-run boundaries, including
                 * floating-point rounding, without fetching any texels. */
                gpu_span(r, s, x, y, count, 0, (int)u, (int)v, 0, 0);
            }
#if GEX_ENABLE_SOFTWARE_RENDERER
            else {
                unsigned color = textured ? texel(s, page, (int)u, (int)v, palette) : solid;
                if (!textured || color) {
                    if (textured && !quad && !(p[7] & 2) && format == 2 &&
                        p[4] != 128 && p[5] != 128 && p[6] != 128)
                        color = modulate(color, p, s->modulation, 0);
                    span(r, s, x, y, count, color, (p[7] & 2) && (!textured || (color & 0x8000)));
                }
            }
#endif
            x += count;
            u = a.u + (x + 0.5 - a.x) * du;
            v = a.v + (x + 0.5 - a.x) * dv;
        }
    }
}

static void raster_packet(gex_raster *r, const uint8_t *p, const gex_raster_state *s,
                          int offset_x, int offset_y) {
    if (gex_raster_gpu_owned(r) && r->gpu->native->packet) {
        char error[256] = {0};
        int consumed = r->gpu->native->packet(r->device, p, s, offset_x, offset_y,
                                              error, sizeof error);
        if (consumed < 0) gpu_fail(error);
        if (consumed) {
            r->guest_newer = r->gpu_newer = 1;
            return;
        }
    }
    raster_packet_decode(r, p, s, offset_x, offset_y);
    if (gex_raster_gpu_owned(r) && r->gpu->native->packet) {
        char error[256] = {0};
        if (r->gpu->native->packet(r->device, NULL, NULL, 0, 0, error, sizeof error) < 0)
            gpu_fail(error);
    }
}

void gex_raster_packet(gex_raster *r, const uint8_t *p, const gex_raster_state *s) {
#if !GEX_ENABLE_SOFTWARE_RENDERER
    if (!r->gpu) gpu_fail("software packet renderer was excluded at build time");
#endif
    r->span_count = 0;
    if (s->layout == 2) {
        unsigned code = p[7] & ~2u;
        int y = coordinate(p + 10) - s->origin_y;
        if (code == 0x60 && !coordinate(p + 8) && coordinate(p + 12) == 320 &&
            ((y == 0 && coordinate(p + 14) == 240) ||
             (y == 8 && coordinate(p + 14) == 224))) {
            raster_packet(r, p, s, 0, 0);
            goto submit;
        }
        for (int y = 0; y < r->height; y += 224)
            for (int x = 0; x < r->width; x += 320)
                raster_packet(r, p, s, x, y);
    } else {
        raster_packet(r, p, s, 0, 0);
    }
submit:
    gpu_submit(r);
}
