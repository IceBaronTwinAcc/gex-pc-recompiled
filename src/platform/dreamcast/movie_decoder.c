#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "movie_decoder.h"

/* Metadata-only support lets the shared MCI layer honour --skip-movies.
 * Decoding is explicitly unavailable rather than faking playback success. */
struct gex_movie_decoder { int unused; };
static const char unavailable[] = "Dreamcast movie decoding is not implemented; use --skip-movies";
static uint32_t little32(const unsigned char *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
gex_movie_decoder *gex_movie_decoder_open(const char *path, gex_movie_info *info,
                                         char *error, size_t size) {
    FILE *file = fopen(path, "rb");
    if (!file) { snprintf(error, size, "cannot open AVI metadata"); return NULL; }
    unsigned char header[4096];
    size_t bytes = fread(header, 1, sizeof header, file);
    int closed = fclose(file);
    if (closed || bytes < 12 || memcmp(header, "RIFF", 4) || memcmp(header + 8, "AVI ", 4))
        goto invalid;
    for (size_t at = 12; at + 64 <= bytes; at += 2) {
        if (memcmp(header + at, "avih", 4) || little32(header + at + 4) < 56) continue;
        const unsigned char *avih = header + at + 8;
        uint32_t width = little32(avih + 32), height = little32(avih + 36);
        uint64_t duration = (uint64_t)little32(avih) * little32(avih + 16) / 1000;
        if (!width || !height || width > 8192 || height > 8192 || duration > UINT32_MAX)
            goto invalid;
        *info = (gex_movie_info){.width = (int)width, .height = (int)height,
                                 .duration_ms = (uint32_t)duration};
        gex_movie_decoder *decoder = calloc(1, sizeof *decoder);
        if (!decoder) snprintf(error, size, "no memory for AVI metadata");
        return decoder;
    }
invalid:
    snprintf(error, size, "unsupported AVI metadata");
    return NULL;
}
void gex_movie_decoder_close(gex_movie_decoder *decoder) { free(decoder); }
int gex_movie_decoder_next(gex_movie_decoder *decoder, gex_movie_event *event) {
    (void)decoder; (void)event;
    return -1;
}
int gex_movie_decoder_render(gex_movie_decoder *decoder, uint8_t *pixels,
                             int w, int h, int pitch, gex_movie_pixel_format format) {
    (void)decoder; (void)pixels; (void)w; (void)h; (void)pitch; (void)format;
    return -1;
}
void gex_movie_decoder_release(gex_movie_decoder *decoder) { (void)decoder; }
void gex_movie_decoder_discard(gex_movie_decoder *decoder) { (void)decoder; }
int gex_movie_decoder_rewind(gex_movie_decoder *decoder) { (void)decoder; return -1; }
const char *gex_movie_decoder_error(const gex_movie_decoder *decoder) {
    (void)decoder;
    return unavailable;
}
