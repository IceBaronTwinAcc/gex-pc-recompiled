#include <kos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "movie_decoder.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t size) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    return hash;
}

int main(void) {
    arch_set_exit_path(ARCH_EXIT_MENU);
    static const char *names[] = {"/cd/Gex/AVI/GEX000.AVI", "/cd/Gex/AVI/GEX001.AVI"};
    static const unsigned expected[] = {457, 867};
    uint8_t *pixels = malloc(320 * 240 * 2 + 32);
    if (!pixels) return 1;
    for (int movie = 0; movie < 2; ++movie) {
        char error[256];
        gex_movie_info info;
        if (gex_movie_decoder_open("/cd/missing.mpg", &info, error, sizeof error) || !error[0])
            return 1;
        gex_movie_decoder *d = gex_movie_decoder_open(names[movie], &info, error, sizeof error);
        if (!d) { printf("[mpeg-test] open: %s\n", error); return 1; }
        if (info.width != 320 || info.height != 240 ||
            info.audio_channels != 2 || info.audio_rate != 32000) return 1;
        uint32_t previous_video = 0, previous_audio = 0;
        for (int pass = 0; pass < 2; ++pass) {
            unsigned video = 0, audio = 0;
            uint32_t video_hash = 2166136261u, audio_hash = 2166136261u;
            uint64_t decode_us = 0, render_us = 0;
            int64_t last_timestamp = -1;
            gex_movie_event event;
            int rc;
            for (;;) {
                uint64_t start = timer_us_gettime64();
                rc = gex_movie_decoder_next(d, &event);
                decode_us += timer_us_gettime64() - start;
                if (rc <= 0) break;
                gex_movie_event other;
                if (gex_movie_decoder_next(d, &other) != -1 || !gex_movie_decoder_error(d)[0])
                    return 1;
                if (event.type == GEX_MOVIE_VIDEO) {
                    if (event.timestamp_ms < last_timestamp) return 1;
                    last_timestamp = event.timestamp_ms;
                    memset(pixels + 320 * 240 * 2, 0xA5, 32);
                    start = timer_us_gettime64();
                    if (gex_movie_decoder_render(d, pixels, 320, 240, 640, GEX_MOVIE_RGB565_LE))
                        return 1;
                    render_us += timer_us_gettime64() - start;
                    for (int i = 0; i < 32; ++i)
                        if (pixels[320 * 240 * 2 + i] != 0xA5) return 1;
                    video_hash = hash_bytes(video_hash, pixels, 320 * 240 * 2);
                    ++video;
                } else {
                    if (!event.samples || event.audio_frames <= 0 ||
                        event.audio_frames > GEX_MOVIE_AUDIO_CHUNK_FRAMES) return 1;
                    audio += (unsigned)event.audio_frames;
                    audio_hash = hash_bytes(audio_hash, event.samples, (size_t)event.audio_frames * 4);
                }
                gex_movie_decoder_release(d);
            }
            if (rc || video != expected[movie] || !audio ||
                (pass && (video_hash != previous_video || audio_hash != previous_audio))) {
                printf("[mpeg-test] FAIL %s video=%u audio=%u: %s\n",
                       names[movie], video, audio, gex_movie_decoder_error(d));
                return 1;
            }
            previous_video = video_hash;
            previous_audio = audio_hash;
            printf("[mpeg-test] %s pass=%d video=%u audio=%u decode-us=%llu render-us=%llu\n",
                   names[movie], pass, video, audio, (unsigned long long)decode_us,
                   (unsigned long long)render_us);
            if (gex_movie_decoder_next(d, &event) || gex_movie_decoder_rewind(d)) return 1;
        }
        gex_movie_event event;
        if (gex_movie_decoder_next(d, &event) != 1 || gex_movie_decoder_rewind(d) ||
            gex_movie_decoder_next(d, &event) != 1) return 1;
        gex_movie_decoder_discard(d);
        if (gex_movie_decoder_rewind(d) || gex_movie_decoder_next(d, &event) != 1) return 1;
        gex_movie_decoder_close(d);
    }
    free(pixels);
    printf("[mpeg-test] complete decode and rewind PASS\n");
    return 0;
}
