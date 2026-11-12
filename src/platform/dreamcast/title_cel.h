#ifndef GEX_DREAMCAST_TITLE_CEL_H
#define GEX_DREAMCAST_TITLE_CEL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int left, top;
    unsigned width, height, format, tiles;
} gex_title_cel;

enum {
    GEX_TITLE_CEL_OK,
    GEX_TITLE_CEL_UNSUPPORTED,
    GEX_TITLE_CEL_INVALID,
    GEX_TITLE_CEL_OVERLAP,
    GEX_TITLE_CEL_NO_MEMORY
};

typedef int (*gex_title_cel_read)(void *context, uint32_t address, void *out, size_t size);
typedef int (*gex_title_cel_match)(void *context, uint32_t address,
                                   const void *data, size_t size);

typedef struct {
    gex_title_cel cel;
    uint32_t *pixels;
    uint8_t palette[512];
    size_t palette_size;
    uint8_t *source;
    size_t source_size;
    uint32_t image_address, palette_address;
} gex_title_cel_snapshot;

int gex_title_cel_bounds(const uint8_t *image, size_t size, gex_title_cel *out);
/* Bit 16 marks a covered pixel; the low 16 bits hold the original texel. */
int gex_title_cel_decode(const uint8_t *image, size_t size,
                         const gex_title_cel *cel, uint32_t *pixels, size_t count);
int gex_title_cel_visibility(const gex_title_cel *cel, const uint8_t *palette,
                             size_t palette_size, const uint8_t *modulation,
                             unsigned rgb, uint32_t visible[8]);
/* Split raw texels into ARGB1555 planes; modulation determines indexed transparency. */
int gex_title_cel_colors(const gex_title_cel *cel, const uint32_t *pixels,
                         size_t count, const uint8_t *palette, size_t palette_size,
                         const uint8_t *modulation, unsigned rgb, int semitransparent,
                         uint16_t *opaque, uint16_t *blended);
/* Produce 32-bit PVR-twiddled words, including transparent padding. */
int gex_title_cel_twiddle(const gex_title_cel *cel, const uint16_t *pixels,
                          size_t count, unsigned width, unsigned height,
                          uint32_t *words, size_t word_count);

/* Capture pixels and, for indexed images, the palette for deferred replay.
 * The palette address must contain 16 or 256 BGR555 entries matching the
 * image format. The output must be fresh or released. The reader returns zero
 * only for complete reads; no output is owned on failure. */
int gex_title_cel_capture(gex_title_cel_read read, void *context,
                          uint32_t image, uint32_t palette,
                          gex_title_cel_snapshot *out);
int gex_title_cel_matches(gex_title_cel_read read, gex_title_cel_match match, void *context,
                          uint32_t image, uint32_t palette,
                          const gex_title_cel_snapshot *snapshot);
int gex_title_cel_matches_image(gex_title_cel_read read, gex_title_cel_match match, void *context,
                                uint32_t image, const gex_title_cel_snapshot *snapshot);
void gex_title_cel_release(gex_title_cel_snapshot *snapshot);

#endif
