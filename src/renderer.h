#ifndef GEX_RENDERER_H
#define GEX_RENDERER_H

#ifndef GEX_ENABLE_SOFTWARE_RENDERER
#define GEX_ENABLE_SOFTWARE_RENDERER 1
#endif
#ifndef GEX_ENABLE_OPENGL_RENDERER
#define GEX_ENABLE_OPENGL_RENDERER 0
#endif
#ifndef GEX_ENABLE_PVR_RENDERER
#define GEX_ENABLE_PVR_RENDERER 0
#endif
#ifndef GEX_ENABLE_GLDC_RENDERER
#define GEX_ENABLE_GLDC_RENDERER 0
#endif
#ifndef GEX_ENABLE_BGFX_RENDERER
#define GEX_ENABLE_BGFX_RENDERER 0
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *pixels; /* NULL in GPU-owned observations and borrowed frames. */
    int width, height, pitch;
    uint32_t sequence;
} gex_render_frame;

typedef struct { int x, y, width, height; } gex_render_rect;

/* Frames are RGB565 little-endian with 4-byte-aligned pitch. Frame descriptors
 * are borrowed for the callback duration; pixel storage follows configure's
 * lifetime contract. The host serializes calls; a backend must keep platform
 * rendering on its owning thread, copying pixels for asynchronous ownership. */
typedef struct {
    int (*open)(int headless, char *error, size_t size);
    /* Pixels are NULL for GPU-owned output. On failure, leave the old configuration usable.
     * On success, the frame's
     * storage remains valid until the next successful configure or shutdown. */
    int (*configure)(const gex_render_frame *frame, char *error, size_t size);
    int (*present)(const gex_render_frame *frame, char *error, size_t size);
    int (*pump)(char *error, size_t size);
    void (*shutdown)(void);
    /* Optional fused upload. Return 1 to use the portable path, -1 on error,
     * or 0 after updating frame pixels AND presenting. The host then advances
     * the frame sequence and notifies observers without presenting again. */
    int (*upload_bgr555)(const gex_render_frame *frame, const uint8_t *pixels,
                         int width, int height, int pitch, char *error, size_t size);
    void (*device_window)(void *window);
} gex_renderer_backend;

const gex_renderer_backend *gex_renderer_backend_get(void);
const gex_renderer_backend *gex_bgfx_renderer(void);
/* Bound memory use without requiring a particular aspect or integer scale. */
int gex_renderer_valid_size(int width, int height);
int gex_renderer_parse_size(const char *text, int *width, int *height);
/* All dimensions must be positive. Odd spare pixels go to the right/bottom. */
gex_render_rect gex_renderer_fit(int width, int height, int source_width, int source_height);
int gex_renderer_init(const gex_renderer_backend *backend, int headless);
int gex_renderer_resize(int width, int height);
int gex_renderer_frame(gex_render_frame *frame);
int gex_renderer_commit(void);
/* Notify a GPU-owned presentation without copying device pixels to front. */
int gex_renderer_device_commit(void);
int gex_renderer_device_mode(int enabled);
void gex_renderer_device_window(void *window);
int gex_renderer_pump(void);
int gex_renderer_upload_bgr555(const uint8_t *pixels, int width, int height, int pitch);
int gex_renderer_upload_bgr555_shaded(const uint8_t *pixels, int width, int height, int pitch,
                                     unsigned shade);
void gex_renderer_observe(void (*observer)(const gex_render_frame *frame));
void gex_renderer_shutdown(void);
const char *gex_renderer_error(void);

#endif
