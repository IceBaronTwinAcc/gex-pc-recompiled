#ifdef __DREAMCAST__
#include <kos.h>
KOS_INIT_FLAGS(INIT_DEFAULT);
#else
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <libavutil/cpu.h>
#endif
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "movie_decoder.h"
#ifdef GEX_MOVIE_CUSTOM_IO
#include <libavformat/avformat.h>
#include "platform/ffmpeg_io.h"

static int check_disc_io(const char *path) {
    char error[256];
    AVFormatContext *format = avformat_alloc_context();
    FILE *reference = fopen(path, "rb");
    if (!format || !reference) return 1;
    gex_ffmpeg_io *io = gex_ffmpeg_io_open(path, format, error, sizeof error);
    if (!io) { printf("[movie-test] %s\n", error); return 1; }
    AVIOContext *context = format->pb;
    int64_t size = context->seek(context->opaque, 0, AVSEEK_SIZE | AVSEEK_FORCE);
    static const int64_t positions[] = {0, 2047, 65531, 65536, 65537, 131069, 19};
    uint8_t expected[4093], actual[4093];
    int failed = size <= 131069 + (int)sizeof actual;
    for (unsigned i = 0; i < sizeof positions / sizeof *positions; ++i) {
        int64_t position = positions[i];
        failed |= context->seek(context->opaque, position, SEEK_SET) != position;
        failed |= fseek(reference, (long)position, SEEK_SET) != 0;
        failed |= fread(expected, 1, sizeof expected, reference) != sizeof expected;
        failed |= context->read_packet(context->opaque, actual, sizeof actual) != sizeof actual;
        failed |= memcmp(actual, expected, sizeof actual) != 0;
    }
    int64_t position = context->seek(context->opaque, 0, SEEK_CUR);
    failed |= context->seek(context->opaque, -1, SEEK_SET) >= 0;
    failed |= context->seek(context->opaque, INT64_MAX, SEEK_CUR) >= 0;
    failed |= context->seek(context->opaque, 0, SEEK_CUR) != position;
    failed |= context->seek(context->opaque, -1, SEEK_END) != size - 1;
    failed |= fseek(reference, -1, SEEK_END) != 0;
    failed |= context->read_packet(context->opaque, actual, sizeof actual) != 1;
    failed |= actual[0] != (uint8_t)fgetc(reference);
    failed |= context->read_packet(context->opaque, actual, sizeof actual) != AVERROR_EOF;
    failed |= context->seek(context->opaque, 0, SEEK_SET | AVSEEK_FORCE) != 0;
    failed |= context->read_packet(context->opaque, actual, 1) != 1;
    rewind(reference);
    failed |= actual[0] != (uint8_t)fgetc(reference);
    gex_ffmpeg_io_close(io);
    avformat_free_context(format);
    failed |= fclose(reference) != 0;
    if (failed) printf("[movie-test] FAIL disc seeks, sector/cache boundaries or EOF\n");
    return failed;
}
#endif

static uint64_t now_us(void) {
#ifdef __DREAMCAST__
    return timer_us_gettime64();
#else
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000 + time.tv_nsec / 1000;
#endif
}

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t bytes) {
    const uint8_t *p = data;
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * UINT32_C(16777619);
    return hash;
}

int main(void) {
#ifdef __DREAMCAST__
    arch_set_exit_path(ARCH_EXIT_MENU);
#else
    av_force_cpu_flags(0);
#endif
    static const char *names[] = {"GEX000.AVI", "GEX001.AVI"};
    static const unsigned video_counts[] = {457, 867}, audio_counts[] = {335899, 1274490};
    static const uint32_t video_hashes[] = {0x3b2ef603, 0x7b58d437};
    static const uint32_t audio_hashes[] = {0x41d64008, 0xc297cbcf};
    uint8_t *pixels = malloc(320 * 240 * 2 + 32);
    if (!pixels) return 1;
    for (unsigned movie = 0; movie < 2; ++movie) {
        char path[128], error[256];
        snprintf(path, sizeof path,
#ifdef __DREAMCAST__
                 "/cd/Gex/AVI/%s",
#else
                 "Gex/AVI/%s",
#endif
                 names[movie]);
#ifdef GEX_MOVIE_CUSTOM_IO
        if (check_disc_io(path)) return 1;
#endif
        gex_movie_info info;
        gex_movie_decoder *decoder = gex_movie_decoder_open(path, &info, error, sizeof error);
        if (!decoder) { printf("[movie-test] %s\n", error); return 1; }
        if (info.width != 320 || info.height != 240 ||
            info.audio_channels != (movie ? 1 : 2) ||
            info.audio_rate != (movie ? 22050 : 11025)) return 1;
        uint32_t previous_video = 0, previous_audio = 0;
        for (unsigned pass = 0; pass < 2; ++pass) {
            unsigned video = 0, audio = 0;
            uint32_t video_hash = 2166136261u, audio_hash = 2166136261u;
            uint64_t decode_us = 0, render_us = 0, maximum_us = 0;
            int64_t last_timestamp = -1;
            gex_movie_event event;
            int result;
            for (;;) {
                uint64_t start = now_us();
                result = gex_movie_decoder_next(decoder, &event);
                uint64_t decode = now_us() - start;
                decode_us += decode;
                if (decode > maximum_us) maximum_us = decode;
                if (result <= 0) break;
                if (event.type == GEX_MOVIE_VIDEO) {
                    if (event.timestamp_ms < last_timestamp) return 1;
                    last_timestamp = event.timestamp_ms;
                    memset(pixels + 320 * 240 * 2, 0xA5, 32);
                    start = now_us();
                    if (gex_movie_decoder_render(decoder, pixels, 320, 240, 640, GEX_MOVIE_RGB565_LE))
                        return 1;
                    render_us += now_us() - start;
                    for (unsigned i = 0; i < 32; ++i)
                        if (pixels[320 * 240 * 2 + i] != 0xA5) return 1;
                    video_hash = hash_bytes(video_hash, pixels, 320 * 240 * 2);
                    ++video;
                } else {
                    if (!event.samples || event.audio_frames <= 0 ||
                        event.audio_frames > GEX_MOVIE_AUDIO_CHUNK_FRAMES) return 1;
                    audio += (unsigned)event.audio_frames;
                    audio_hash = hash_bytes(audio_hash, event.samples,
                        (size_t)event.audio_frames * info.audio_channels * 2);
                }
                gex_movie_decoder_release(decoder);
            }
            if (result || video != video_counts[movie] || audio != audio_counts[movie]) {
                printf("[movie-test] FAIL %s video=%u audio=%u: %s\n",
                       names[movie], video, audio, gex_movie_decoder_error(decoder));
                return 1;
            }
            if (pass && (video_hash != previous_video || audio_hash != previous_audio)) return 1;
            if (video_hash != video_hashes[movie] || audio_hash != audio_hashes[movie]) {
                printf("[movie-test] FAIL reference pixels/audio for %s\n", names[movie]);
                return 1;
            }
            previous_video = video_hash; previous_audio = audio_hash;
            printf("[movie-test] %s pass=%u video=%u audio=%u hashes=%08lx/%08lx "
                   "decode-us=%llu render-us=%llu max-next-us=%llu heap=%lu\n",
                   names[movie], pass, video, audio, (unsigned long)video_hash,
                   (unsigned long)audio_hash, (unsigned long long)decode_us,
                   (unsigned long long)render_us, (unsigned long long)maximum_us,
#ifdef __DREAMCAST__
                   (unsigned long)mallinfo().uordblks);
#else
                   (unsigned long)mallinfo2().uordblks);
#endif
            if (gex_movie_decoder_next(decoder, &event) || gex_movie_decoder_rewind(decoder)) return 1;
        }
        gex_movie_decoder_close(decoder);
    }
    free(pixels);
    printf("[movie-test] both original AVIs, complete pixels/audio, guards and rewind PASS\n");
    return 0;
}
