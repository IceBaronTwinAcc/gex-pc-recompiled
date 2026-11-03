#ifndef GEX_PACKET_GPU_H
#define GEX_PACKET_GPU_H

#include <stddef.h>
#include <stdint.h>

/* Pixel coverage and interpolation are prepared without sampling textures.
 * mode 1 uses 16.16 coordinates; mode 0 is a constant integer-texel run. */
typedef struct {
    int32_t x, y, count, mode;
    double u, v, du, dv;
    /* Rectangle-capable backends may receive row count and V step in
     * reserved[0..1]; zero row count retains ordinary single-row coverage. */
    int32_t scale, phase, reserved[2];
} gex_gpu_span;

typedef struct {
    unsigned page, clut, opcode, rgb;
    const uint8_t *atlas, *modulation;
} gex_gpu_material;

typedef struct { float x, y, u, v; } gex_gpu_vertex;
struct gex_raster_state;

/* GPU-owned pictures never need CPU materialization. Asset blits upload CPU
 * source data, while picture copies and sampling remain entirely on device. */
typedef struct {
    int (*clear)(void *target, char *error, size_t size);
    int (*copy)(void *destination, void *source, unsigned shade, char *error, size_t size);
    int (*blit)(void *target, int x, int y, int width, int height,
                const uint8_t *pixels, int pitch, int key, char *error, size_t size);
    int (*present)(void *target, char *error, size_t size);
    void (*bind)(void *target, const uint8_t *atlas, unsigned plane);
    int (*polygon)(void *target, const gex_gpu_vertex vertices[4],
                   int left, int top, int right, int bottom,
                   const gex_gpu_material *material, char *error, size_t size);
    int (*movie)(void *target, const uint8_t *pixels, int width, int height, int pitch,
                 char *error, size_t size);
    /* Notify before CPU writes to the atlas. Callers opting in must report
     * every write; other callers retain byte-comparison validation. */
    void (*atlas_write)(const uint8_t *atlas, unsigned offset, size_t bytes);
    /* Optional exact packet/state recognition before raster preparation.
     * Return 1 when consumed, 0 to decode normally, or -1 on failure.
     * A NULL packet ends decoding and discards any unused pending identity. */
    int (*packet)(void *target, const uint8_t *packet, const struct gex_raster_state *state,
                  int offset_x, int offset_y, char *error, size_t size);
    /* Optional batched notification, with the same pre-write contract. */
    void (*atlas_write_rect)(const uint8_t *atlas, unsigned offset, size_t pitch,
                            size_t row_bytes, unsigned rows);
} gex_gpu_native_ops;

typedef struct {
    void *(*create)(int width, int height, char *error, size_t size);
    void (*destroy)(void *target);
    int (*upload)(void *target, const uint16_t *pixels, char *error, size_t size);
    int (*download)(void *target, uint16_t *pixels, char *error, size_t size);
    int (*draw)(void *target, const gex_gpu_span *spans, size_t count,
                const gex_gpu_material *material, char *error, size_t size);
    /* Optional CPU-addressable history storage owned by the target. This
     * lets memory-constrained devices keep compatibility snapshots in VRAM. */
    uint16_t *(*history)(void *target, size_t bytes, char *error, size_t size);
    int rectangle_spans;
    const gex_gpu_native_ops *native;
} gex_packet_gpu;

/* NULL means this build has no packet GPU implementation, not a fallback. */
const gex_packet_gpu *gex_packet_gpu_get(void);
const gex_packet_gpu *gex_packet_gpu_gldc(void);
int gex_packet_gpu_select_gldc(int headless);
const gex_packet_gpu *gex_packet_gpu_bgfx(void);
int gex_packet_gpu_select_bgfx(void);
void gex_packet_gpu_shutdown_bgfx(void);
#if GEX_PROFILE_RENDERER
uint64_t gex_packet_gpu_profile_clock(uint64_t *cpu);
void gex_packet_gpu_profile(const char *name, unsigned draws, unsigned uploads,
                            unsigned resolves, size_t bytes);
#endif

#endif
