#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "movie_decoder.h"

static int failures;
#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

int main(int argc, char **argv) {
    if (argc != 2 && (argc != 3 || strcmp(argv[2], "--second-movie"))) return 1;
    int second_movie = argc == 3;
    unsigned expected_video_frames = second_movie ? 867 : 457;
    unsigned expected_audio_frames = second_movie ? 1274490 : 335899;
    gex_movie_info info;
    char error[256] = {0};
    CHECK(!gex_movie_decoder_open("", &info, error, sizeof error));
    CHECK(error[0]);
    gex_movie_decoder *decoder = gex_movie_decoder_open(argv[1], &info, error, sizeof error);
    if (!decoder) { fprintf(stderr, "%s\n", error); return 1; }
    CHECK(info.width == 320 && info.height == 240);
    CHECK(info.audio_rate == (second_movie ? 22050 : 11025));
    CHECK(info.audio_channels == (second_movie ? 1 : 2));
    CHECK(info.duration_ms == (second_movie ? 57800u : 30466u));
    size_t pixel_size = (size_t)info.width * info.height * 4;
    uint8_t *pixels = malloc(pixel_size + 32);
    if (!pixels) return 1;
    uint64_t previous_video = 0, previous_audio = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        unsigned video_frames = 0, audio_frames = 0;
        uint64_t video_hash = 0, audio_hash = 0;
        int64_t last_timestamp = -1;
        gex_movie_event event;
        CHECK(gex_movie_decoder_render(decoder, pixels, info.width, info.height,
                                       info.width * 4, GEX_MOVIE_BGRA32) == -1);
        int rc;
        while ((rc = gex_movie_decoder_next(decoder, &event)) > 0) {
            if (event.type == GEX_MOVIE_AUDIO) {
                CHECK(event.samples && event.audio_frames > 0 &&
                      event.audio_frames <= GEX_MOVIE_AUDIO_CHUNK_FRAMES);
                audio_frames += (unsigned)event.audio_frames;
                audio_hash = hash_bytes(audio_hash, event.samples,
                    (size_t)event.audio_frames * info.audio_channels * sizeof(int16_t));
            } else {
                CHECK(event.type == GEX_MOVIE_VIDEO);
                CHECK(event.timestamp_ms >= last_timestamp);
                last_timestamp = event.timestamp_ms;
                gex_movie_pixel_format format = video_frames % 2 ? GEX_MOVIE_BGRA32 : GEX_MOVIE_RGB565_LE;
                int pitch = info.width * (format == GEX_MOVIE_BGRA32 ? 4 : 2);
                size_t size = (size_t)pitch * info.height;
                memset(pixels, 0xA5, pixel_size + 32);
                CHECK(gex_movie_decoder_render(decoder, pixels, info.width, info.height, pitch - 1, format) == -1);
                CHECK(pixels[0] == 0xA5);
                CHECK(!gex_movie_decoder_render(decoder, pixels, info.width, info.height, pitch, format));
                for (unsigned i = 0; i < 32; ++i) CHECK(pixels[size + i] == 0xA5);
                video_hash = hash_bytes(video_hash, pixels, size);
                ++video_frames;
            }
            gex_movie_event other;
            CHECK(gex_movie_decoder_next(decoder, &other) == -1);
            CHECK(gex_movie_decoder_error(decoder)[0]);
            gex_movie_decoder_release(decoder);
        }
        if (rc < 0) fprintf(stderr, "%s\n", gex_movie_decoder_error(decoder));
        CHECK(rc == 0 && gex_movie_decoder_next(decoder, &event) == 0);
        CHECK(video_frames == expected_video_frames && audio_frames == expected_audio_frames);
        CHECK(video_hash && audio_hash);
        if (pass) CHECK(video_hash == previous_video && audio_hash == previous_audio);
        previous_video = video_hash; previous_audio = audio_hash;
        CHECK(!gex_movie_decoder_rewind(decoder));
    }
    gex_movie_event event;
    CHECK(gex_movie_decoder_next(decoder, &event) == 1);
    CHECK(!gex_movie_decoder_rewind(decoder));
    CHECK(gex_movie_decoder_next(decoder, &event) == 1);
    gex_movie_decoder_discard(decoder);
    CHECK(!gex_movie_decoder_rewind(decoder));
    CHECK(gex_movie_decoder_next(decoder, &event) == 1);
    gex_movie_decoder_close(decoder);
    gex_movie_decoder_close(NULL);
    free(pixels);
    return failures ? 1 : 0;
}
