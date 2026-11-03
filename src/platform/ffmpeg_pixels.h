#ifndef GEX_FFMPEG_PIXELS_H
#define GEX_FFMPEG_PIXELS_H

#include "movie_decoder.h"

struct AVFrame;
typedef struct {
    uint16_t rgb[3][1024];
    uint8_t luma[1024];
    int16_t chroma[4][256];
    int ready;
} gex_ffmpeg_pixels;

int gex_ffmpeg_convert(gex_ffmpeg_pixels *tables, const struct AVFrame *frame,
                      uint8_t *pixels, int width, int height, int pitch,
                      gex_movie_pixel_format format);

#endif
