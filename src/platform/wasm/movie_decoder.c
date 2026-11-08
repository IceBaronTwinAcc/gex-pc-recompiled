#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "movie_decoder.h"

struct gex_movie_decoder {
    char error[128];
};

gex_movie_decoder *gex_movie_decoder_open(const char *path, gex_movie_info *info,
                                          char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (error_size) snprintf(error, error_size, "cannot open movie file");
        return NULL;
    }
    fclose(file);
    gex_movie_decoder *decoder = calloc(1, sizeof *decoder);
    if (!decoder) {
        if (error_size) snprintf(error, error_size, "cannot allocate movie state");
        return NULL;
    }
    *info = (gex_movie_info){ .width = 320, .height = 240 };
    snprintf(decoder->error, sizeof decoder->error,
             "AVI decoding is not available in the WebAssembly build");
    if (error_size) error[0] = 0;
    return decoder;
}

void gex_movie_decoder_close(gex_movie_decoder *decoder) { free(decoder); }
int gex_movie_decoder_next(gex_movie_decoder *decoder, gex_movie_event *event) {
    (void)event;
    snprintf(decoder->error, sizeof decoder->error,
             "AVI decoding is not available in the WebAssembly build");
    return -1;
}
int gex_movie_decoder_render(gex_movie_decoder *decoder, uint8_t *pixels,
                             int width, int height, int pitch,
                             gex_movie_pixel_format format) {
    (void)pixels; (void)width; (void)height; (void)pitch; (void)format;
    snprintf(decoder->error, sizeof decoder->error,
             "AVI decoding is not available in the WebAssembly build");
    return -1;
}
void gex_movie_decoder_release(gex_movie_decoder *decoder) { (void)decoder; }
void gex_movie_decoder_discard(gex_movie_decoder *decoder) { (void)decoder; }
int gex_movie_decoder_rewind(gex_movie_decoder *decoder) {
    snprintf(decoder->error, sizeof decoder->error,
             "AVI decoding is not available in the WebAssembly build");
    return -1;
}
const char *gex_movie_decoder_error(const gex_movie_decoder *decoder) {
    return decoder->error;
}
