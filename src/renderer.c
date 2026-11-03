#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "renderer.h"

static gex_render_frame front;
static const gex_renderer_backend *output;
static void (*observe)(const gex_render_frame *);
static char error[256];
static int device_mode;

int gex_renderer_valid_size(int width, int height) {
    return width > 0 && height > 0 && width <= 8192 && height <= 8192 &&
        (size_t)width * height <= 16777216;
}

gex_render_rect gex_renderer_fit(int width, int height, int source_width, int source_height) {
    gex_render_rect rect = {0, 0, width, height};
    if ((int64_t)width * source_height > (int64_t)height * source_width)
        rect.width = (int)((int64_t)height * source_width / source_height);
    else
        rect.height = (int)((int64_t)width * source_height / source_width);
    if (rect.width < 1) rect.width = 1;
    if (rect.height < 1) rect.height = 1;
    rect.x = (width - rect.width) / 2;
    rect.y = (height - rect.height) / 2;
    return rect;
}

int gex_renderer_parse_size(const char *text, int *width, int *height) {
    unsigned w = 0, h = 0;
    if (!text || !width || !height || *text < '0' || *text > '9') return -1;
    while (*text >= '0' && *text <= '9') {
        w = w * 10 + (unsigned)(*text++ - '0');
        if (w > 8192) return -1;
    }
    if (*text++ != 'x' || *text < '0' || *text > '9') return -1;
    while (*text >= '0' && *text <= '9') {
        h = h * 10 + (unsigned)(*text++ - '0');
        if (h > 8192) return -1;
    }
    if (*text || !gex_renderer_valid_size((int)w, (int)h)) return -1;
    *width = (int)w; *height = (int)h;
    return 0;
}

static int fail(const char *message) {
    snprintf(error, sizeof error, "%s", message);
    return -1;
}

const char *gex_renderer_error(void) { return error; }

int gex_renderer_init(const gex_renderer_backend *backend, int headless) {
    if (output) return fail("renderer already initialized");
    if (!backend || !backend->open || !backend->configure || !backend->present ||
        !backend->pump || !backend->shutdown)
        return fail("incomplete renderer backend");
    error[0] = 0;
    if (backend->open(headless, error, sizeof error)) {
        backend->shutdown();
        if (!error[0]) fail("renderer backend initialization failed");
        return -1;
    }
    output = backend;
    return 0;
}

int gex_renderer_resize(int width, int height) {
    if (!output) return fail("renderer not initialized");
    if (!gex_renderer_valid_size(width, height))
        return fail("renderer dimensions must be within 1..8192, at most 16777216 pixels");
    if ((front.pixels || device_mode) && front.width == width && front.height == height) return 0;
    gex_render_frame next = {0};
    next.width = width; next.height = height; next.pitch = (width * 2 + 3) & ~3;
    next.sequence = front.sequence + 1;
    if (!device_mode) {
        next.pixels = calloc((size_t)next.pitch, height);
        if (!next.pixels) return fail("cannot allocate renderer framebuffer");
    }
    error[0] = 0;
    if (output->configure(&next, error, sizeof error)) {
        free(next.pixels);
        if (!error[0]) fail("renderer backend configuration failed");
        return -1;
    }
    free(front.pixels);
    front = next;
    return 0;
}

int gex_renderer_frame(gex_render_frame *frame) {
    if (!frame || !front.width || (!device_mode && !front.pixels)) return fail("renderer has no framebuffer");
    *frame = front;
    if (device_mode) frame->pixels = NULL;
    return 0;
}

static int commit(int presented) {
    if (!front.pixels) return fail("renderer has no framebuffer");
    ++front.sequence;
    error[0] = 0;
    if (!presented && output->present(&front, error, sizeof error)) {
        if (!error[0]) fail("renderer backend presentation failed");
        return -1;
    }
    if (observe) observe(&front);
    return 0;
}

int gex_renderer_commit(void) {
    if (device_mode) return fail("GPU-owned output forbids CPU framebuffer presentation");
    return commit(0);
}

int gex_renderer_device_mode(int enabled) {
    enabled = !!enabled;
    if (enabled == device_mode) return 0;
    if (!output) return fail("renderer not initialized");
    gex_render_frame next = front;
    next.pixels = NULL;
    if (!enabled && front.width) {
        next.pixels = calloc((size_t)next.pitch, next.height);
        if (!next.pixels) return fail("cannot restore software framebuffer");
    }
    error[0] = 0;
    if (front.width && output->configure(&next, error, sizeof error)) {
        free(next.pixels);
        if (!error[0]) fail("renderer backend configuration failed");
        return -1;
    }
    free(front.pixels);
    front = next;
    device_mode = enabled;
    return 0;
}
void gex_renderer_device_window(void *window) {
    if (output && output->device_window) output->device_window(window);
}

int gex_renderer_device_commit(void) {
    if (!device_mode) return fail("renderer is not GPU-owned");
    if (!front.width) return fail("renderer has no framebuffer");
    ++front.sequence;
    if (observe) {
        gex_render_frame frame = front;
        frame.pixels = NULL;
        observe(&frame);
    }
    return 0;
}

int gex_renderer_pump(void) {
    if (!output) return fail("renderer not initialized");
    error[0] = 0;
    if (output->pump(error, sizeof error)) {
        if (!error[0]) fail("renderer backend event pump failed");
        return -1;
    }
    return 0;
}

int gex_renderer_upload_bgr555(const uint8_t *pixels, int width, int height, int pitch) {
    return gex_renderer_upload_bgr555_shaded(pixels, width, height, pitch, 255);
}

int gex_renderer_upload_bgr555_shaded(const uint8_t *pixels, int width, int height, int pitch,
                                     unsigned shade) {
    if (device_mode) return fail("GPU-owned output forbids CPU framebuffer conversion");
    if (!pixels || !gex_renderer_valid_size(width, height) ||
        pitch < width * 2 || !front.pixels || shade > 255)
        return fail("invalid renderer upload");
    if (shade == 255 && output->upload_bgr555) {
        error[0] = 0;
        int result = output->upload_bgr555(&front, pixels, width, height, pitch,
                                           error, sizeof error);
        if (result < 0) {
            if (!error[0]) fail("renderer backend upload failed");
            return -1;
        }
        if (!result) return commit(1);
    }
    int previous_source_y = -1;
    for (int y = 0; y < front.height; ++y) {
        int sy = height == front.height ? y : y * height / front.height;
        const uint8_t *source = pixels + (size_t)sy * pitch;
        uint8_t *target = front.pixels + (size_t)y * front.pitch;
        if (sy == previous_source_y) {
            memcpy(target, target - front.pitch, (size_t)front.width * 2);
            continue;
        }
        previous_source_y = sy;
        if (width == front.width) {
            int x = 0;
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            /* Convert pairs without unaligned loads or type-punning. memcpy
             * becomes word transfers once the checked alignment is known. */
            if (!(((uintptr_t)source | (uintptr_t)target) & 3u)) {
                const uint8_t *aligned_source = __builtin_assume_aligned(source, 4);
                uint8_t *aligned_target = __builtin_assume_aligned(target, 4);
                for (; x + 3 < width * 2; x += 4) {
                    uint32_t value;
                    memcpy(&value, aligned_source + x, sizeof value);
                    uint32_t rgb = (value & UINT32_C(0x001F001F)) << 11 |
                        (value & UINT32_C(0x03E003E0)) << 1 |
                        (value >> 10 & UINT32_C(0x001F001F));
                    memcpy(aligned_target + x, &rgb, sizeof rgb);
                }
            }
#endif
            for (; x < width * 2; x += 2) {
                unsigned value = source[x] | (unsigned)source[x + 1] << 8;
                unsigned rgb = (value & 31) << 11 | (value & 0x3E0) << 1 | (value >> 10 & 31);
                target[x] = (uint8_t)rgb;
                target[x + 1] = (uint8_t)(rgb >> 8);
            }
            continue;
        }
        for (int x = 0; x < front.width; ++x) {
            int sx = (x * width / front.width) * 2;
            unsigned value = source[sx] | (unsigned)source[sx + 1] << 8;
            unsigned rgb = (value & 31) << 11 | (value & 0x3E0) << 1 | (value >> 10 & 31);
            target[x * 2] = (uint8_t)rgb;
            target[x * 2 + 1] = (uint8_t)(rgb >> 8);
        }
    }
    if (shade < 255)
        for (int y = 0; y < front.height; ++y)
            for (int x = 0; x < front.width; ++x) {
                uint8_t *p = front.pixels + y * front.pitch + x * 2;
                unsigned value = p[0] | (unsigned)p[1] << 8;
                unsigned rgb = ((value >> 11) * shade / 255) << 11 |
                    (((value >> 5 & 63) * shade / 255) << 5) |
                    ((value & 31) * shade / 255);
                p[0] = (uint8_t)rgb;
                p[1] = (uint8_t)(rgb >> 8);
            }
    return gex_renderer_commit();
}

void gex_renderer_observe(void (*observer)(const gex_render_frame *)) { observe = observer; }

void gex_renderer_shutdown(void) {
    device_mode = 0;
    if (output) output->shutdown();
    free(front.pixels);
    memset(&front, 0, sizeof front);
    output = NULL;
    observe = NULL;
}
