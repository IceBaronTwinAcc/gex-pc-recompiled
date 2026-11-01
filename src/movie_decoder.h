#ifndef GEX_MOVIE_DECODER_H
#define GEX_MOVIE_DECODER_H

#include <stddef.h>
#include <stdint.h>

typedef struct gex_movie_decoder gex_movie_decoder;

typedef struct {
    int width, height;
    int audio_rate, audio_channels;
    uint32_t duration_ms;
} gex_movie_info;

#define GEX_MOVIE_AUDIO_CHUNK_FRAMES 4096

typedef enum {
    GEX_MOVIE_VIDEO,
    GEX_MOVIE_AUDIO
} gex_movie_event_type;

typedef struct {
    gex_movie_event_type type;
    int64_t timestamp_ms;
    const int16_t *samples;
    int audio_frames;
} gex_movie_event;

typedef enum {
    GEX_MOVIE_RGB565_LE,
    GEX_MOVIE_BGRA32
} gex_movie_pixel_format;

/* Errors are written to error (including its NUL) when error_size is nonzero. */
gex_movie_decoder *gex_movie_decoder_open(const char *path, gex_movie_info *info,
                                         char *error, size_t error_size);
void gex_movie_decoder_close(gex_movie_decoder *decoder);
/* One outstanding event at a time: 1 event, 0 EOF, -1 error. Audio is interleaved
 * native-endian signed 16-bit PCM, at most GEX_MOVIE_AUDIO_CHUNK_FRAMES frames,
 * one or two channels at info.audio_rate, borrowed until release.
 * A video event stays available for render until release. No clock or audio
 * device belongs in a decoder; the caller handles timing and partial writes.
 * Video timestamps are milliseconds from the start of the movie. EOF is stable
 * until rewind; close is valid after EOF or with an outstanding event.
 * Implementations stream input instead of loading the complete movie. */
int gex_movie_decoder_next(gex_movie_decoder *decoder, gex_movie_event *event);
int gex_movie_decoder_render(gex_movie_decoder *decoder, uint8_t *pixels,
                             int width, int height, int pitch, gex_movie_pixel_format format);
void gex_movie_decoder_release(gex_movie_decoder *decoder);
/* Discard pending data on stop; rewind additionally resets decoding to start.
 * Stop does not rewind. Rewind discards even an outstanding event. */
void gex_movie_decoder_discard(gex_movie_decoder *decoder);
int gex_movie_decoder_rewind(gex_movie_decoder *decoder);
const char *gex_movie_decoder_error(const gex_movie_decoder *decoder);

#endif
