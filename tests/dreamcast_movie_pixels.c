#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/cpu.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
#include "platform/ffmpeg_pixels.h"

int main(void) {
    av_force_cpu_flags(0);
    AVFrame *frame = av_frame_alloc();
    gex_ffmpeg_pixels *tables = calloc(1, sizeof *tables);
    enum { PITCH = 320 * 4 + 33, BYTES = PITCH * 240 + 32 };
    uint8_t *actual = malloc(BYTES), *expected = malloc(BYTES);
    if (!frame || !tables || !actual || !expected) return 1;
    frame->format = AV_PIX_FMT_YUV410P;
    frame->width = 320;
    frame->height = 240;
    if (av_frame_get_buffer(frame, 32)) return 1;
    uint32_t random = 1;
    for (unsigned pass = 0; pass < 12; ++pass) {
        for (int plane = 0; plane < 3; ++plane)
            for (int y = 0; y < (plane ? 60 : 240); ++y)
                for (int x = 0; x < (plane ? 80 : 320); ++x) {
                    random ^= random << 13;
                    random ^= random >> 17;
                    random ^= random << 5;
                    frame->data[plane][y * frame->linesize[plane] + x] =
                        pass < 3 ? (uint8_t)(pass * 127 + (plane == 0)) : (uint8_t)random;
                }
        for (int format = 0; format < 2; ++format) {
            enum AVPixelFormat pixel_format = format ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGB565LE;
            gex_movie_pixel_format native_format = format ? GEX_MOVIE_BGRA32 : GEX_MOVIE_RGB565_LE;
            int pitch = 320 * (format ? 4 : 2) + (pass & 1 ? 33 : 32);
            struct SwsContext *scale = sws_getContext(320, 240, AV_PIX_FMT_YUV410P,
                320, 240, pixel_format, SWS_BILINEAR, NULL, NULL, NULL);
            if (!scale) return 1;
            memset(actual, 0xA5, BYTES);
            memset(expected, 0xA5, BYTES);
            uint8_t *destination[] = {expected};
            int strides[] = {pitch};
            if (sws_scale(scale, (const uint8_t *const *)frame->data,
                          frame->linesize, 0, 240, destination, strides) != 240 ||
                gex_ffmpeg_convert(tables, frame, actual, 320, 240, pitch, native_format))
                return 1;
            for (int i = 0; i < BYTES; ++i)
                if (actual[i] != expected[i]) {
                    fprintf(stderr, "movie pixels: pass=%u format=%d row=%d byte=%d actual=%u reference=%u\n",
                            pass, format, i / pitch, i % pitch, actual[i], expected[i]);
                    return 1;
                }
            sws_freeContext(scale);
        }
    }
    if (gex_ffmpeg_convert(tables, frame, actual, 319, 240, 640, GEX_MOVIE_RGB565_LE) >= 0)
        return 1;
    frame->format = AV_PIX_FMT_YUV420P;
    if (gex_ffmpeg_convert(tables, frame, actual, 320, 240, 640, GEX_MOVIE_RGB565_LE) >= 0)
        return 1;
    free(actual);
    free(expected);
    free(tables);
    av_frame_free(&frame);
    return 0;
}
