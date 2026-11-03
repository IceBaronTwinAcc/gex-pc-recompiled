#include <errno.h>
#include <string.h>
#include <libavutil/bswap.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include "platform/ffmpeg_pixels.h"

/* Scalar BT.601 tables, bilinear 4:1:0 chroma siting and ordered RGB565
 * dithering match FFmpeg without its general-purpose scaler's RAM footprint. */
static void initialize(gex_ffmpeg_pixels *tables) {
    const int64_t gain = 65536 * 255 / 219;
    const int coefficients[] = {104597, 132201, -25675, -53279};
    for (int i = 0; i < 1024; ++i) {
        int value = (int)((i * gain - (400LL << 16) + 32768) >> 16);
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        tables->luma[i] = (uint8_t)value;
        tables->rgb[0][i] = (uint16_t)((value >> 3) << 11);
        tables->rgb[1][i] = (uint16_t)((value >> 2) << 5);
        tables->rgb[2][i] = (uint16_t)(value >> 3);
    }
    for (int c = 0; c < 4; ++c) {
        int64_t coefficient = ((int64_t)coefficients[c] * 65536 + 32768) / gain;
        for (int i = 0; i < 256; ++i)
            tables->chroma[c][i] = (int16_t)((i * coefficient >> 16) - (coefficient >> 9));
    }
    tables->ready = 1;
}

static void horizontal(const AVFrame *frame, int plane, int row, int16_t out[160]) {
    const uint8_t *in = frame->data[plane] + row * frame->linesize[plane];
    out[0] = in[0] * 4;
    for (int x = 0; x < 79; ++x) {
        out[x * 2 + 1] = (int16_t)(in[x] * 3 + in[x + 1]);
        out[x * 2 + 2] = (int16_t)(in[x] + in[x + 1] * 3);
    }
    out[159] = in[79] * 4;
}

int gex_ffmpeg_convert(gex_ffmpeg_pixels *tables, const AVFrame *frame,
                      uint8_t *pixels, int width, int height, int pitch,
                      gex_movie_pixel_format format) {
    if (frame->format != AV_PIX_FMT_YUV410P || frame->width != width ||
        frame->height != height || width != 320 || height != 240 ||
        (format != GEX_MOVIE_RGB565_LE && format != GEX_MOVIE_BGRA32))
        return AVERROR(ENOTSUP);
    if (!tables->ready) initialize(tables);
    static const uint8_t red_dither[2][2] = {{6, 2}, {0, 4}};
    static const uint8_t green_dither[2][2] = {{1, 3}, {2, 0}};
    int16_t rows[4][160];
    uint16_t unaligned[320];
    int cached_row = -1;
    for (int y = 0; y < height; ++y) {
        int row = y < 2 ? 0 : (y - 2) / 4;
        int vertical = y < 2 || row == 59 ? 0 : 1 + 2 * ((y - 2) & 3);
        if (row != cached_row) {
            horizontal(frame, 1, row, rows[0]);
            horizontal(frame, 1, row < 59 ? row + 1 : row, rows[1]);
            horizontal(frame, 2, row, rows[2]);
            horizontal(frame, 2, row < 59 ? row + 1 : row, rows[3]);
            cached_row = row;
        }
        const uint8_t *luma = frame->data[0] + y * frame->linesize[0];
        uint8_t *out = pixels + y * pitch;
        uint16_t *packed = (uintptr_t)out & 1 ? unaligned : (uint16_t *)out;
        for (int pair = 0; pair < width / 2; ++pair) {
            int u = (rows[0][pair] * 8 + (rows[1][pair] - rows[0][pair]) * vertical + 16) >> 5;
            int v = (rows[2][pair] * 8 + (rows[3][pair] - rows[2][pair]) * vertical + 16) >> 5;
            int r = 326 + tables->chroma[0][v];
            int g = 326 + tables->chroma[2][u] + tables->chroma[3][v];
            int b = 326 + tables->chroma[1][u];
            if (format == GEX_MOVIE_RGB565_LE) {
                const uint16_t *red = tables->rgb[0] + r;
                const uint16_t *green = tables->rgb[1] + g;
                const uint16_t *blue = tables->rgb[2] + b;
                int first = luma[pair * 2], second = luma[pair * 2 + 1];
                packed[pair * 2] = av_le2ne16(red[first + red_dither[y & 1][0]] |
                    green[first + green_dither[y & 1][0]] |
                    blue[first + red_dither[(y & 1) ^ 1][0]]);
                packed[pair * 2 + 1] = av_le2ne16(red[second + red_dither[y & 1][1]] |
                    green[second + green_dither[y & 1][1]] |
                    blue[second + red_dither[(y & 1) ^ 1][1]]);
                continue;
            }
            for (int i = 0; i < 2; ++i) {
                int value = luma[pair * 2 + i];
                *out++ = tables->luma[b + value];
                *out++ = tables->luma[g + value];
                *out++ = tables->luma[r + value];
                *out++ = 255;
            }
        }
        if (format == GEX_MOVIE_RGB565_LE && packed == unaligned)
            memcpy(out, unaligned, sizeof unaligned);
    }
    return 0;
}
