#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "win32hle.h"
#include "movie.h"
#include "movie_decoder.h"
#include "renderer.h"
#include "graphics_guest.h"

static gex_movie_decoder *decoder;
static gex_movie_info info;
static gex_movie_event event;
static uint32_t movie_window, parent_window;
static int skip_movies;
static int audio_stream = -1;
static int pending, playing;
static uint32_t movie_start, callback;
static uint32_t pause_started;
static int paused;
static int audio_offset;
static uint8_t *movie_pixels;
static unsigned video_frames;
static uint64_t audio_frames;

static void cancel_playback(void) {
    if (playing)
        fprintf(stderr, "[movie] playback cancelled: video=%u audio=%llu elapsed-ms=%u queued=%d\n",
                video_frames, (unsigned long long)audio_frames, (unsigned)(hle_ticks_ms() - movie_start),
                audio_stream >= 0 ? hle_audio_stream_queued(audio_stream) : 0);
    if (playing && callback) hle_post_message(callback, 0x3B9, 4, 1);
    playing = paused = pending = audio_offset = 0;
    if (decoder) gex_movie_decoder_discard(decoder);
    if (audio_stream >= 0) hle_audio_stream_close(audio_stream);
    audio_stream = -1;
}

static void release_movie(void) {
    cancel_playback();
    gex_movie_decoder_close(decoder);
    decoder = NULL;
    free(movie_pixels);
    movie_pixels = NULL;
    if (movie_window) {
        uint32_t args[] = {movie_window};
        hle_call_guest(hle_resolve("DestroyWindow"), 1, args);
        movie_window = 0;
    }
}

static void open_movie(void) {
    uint32_t params = A32(3);
    const char *name = hle_ptr(MEM32(params + 12));
    char path[1024];
    if (!name || !hle_host_path(name, path, sizeof path)) RET(275, 4);
    release_movie();
    char error[256];
    decoder = gex_movie_decoder_open(path, &info, error, sizeof error);
    if (!decoder) {
        fprintf(stderr, "[movie] cannot open %s: %s\n", path, error);
        RET(277, 4);
    }
    parent_window = MEM32(params + 24);
    uint32_t mark = hle_scratch_mark();
    uint32_t klass = hle_scratch_copy("STATIC", 7);
    uint32_t args[] = {0, klass, 0, 0x50000000u, 0, 0,
                      (uint32_t)info.width, (uint32_t)info.height,
                      parent_window, 0, 0x400000, 0};
    movie_window = hle_call_guest(hle_resolve("CreateWindowExA"), 12, args);
    hle_scratch_release(mark);
    if (!movie_window) {
        release_movie();
        RET(277, 4);
    }
    MEM32(params + 4) = 1;
    fprintf(stderr, "[movie] opened %s (%dx%d)\n", name, info.width, info.height);
    RET(0, 4);
}

static void play_movie(void) {
    if (!decoder) RET(257, 4);
    if (playing) hle_fatal("MCI_PLAY while a movie is already playing");
    callback = (A32(2) & 1) ? MEM32(A32(3)) : 0;
    if (A32(2) & 2) hle_fatal("synchronous MCI_PLAY is not supported");
    if (skip_movies) {
        fprintf(stderr, "[movie] skipped by --skip-movies\n");
        if (callback) hle_post_message(callback, 0x3B9, 1, 1);
        RET(0, 4);
    }
    if (info.audio_channels) {
        audio_stream = hle_audio_stream_open(info.audio_rate, info.audio_channels);
        if (audio_stream < 0) hle_fatal("cannot open AVI audio stream");
    }
    movie_start = hle_ticks_ms();
    video_frames = 0;
    audio_frames = 0;
    playing = 1;
    RET(0, 4);
}

void gex_movie_pump(void) {
    if (!playing || paused) return;
    for (;;) {
        if (pending && event.type == GEX_MOVIE_AUDIO) {
            int count = event.audio_frames - audio_offset;
            int written = hle_audio_stream_write(audio_stream,
                event.samples + audio_offset * info.audio_channels, count);
            audio_offset += written;
            audio_frames += written;
            if (audio_offset == event.audio_frames) {
                pending = audio_offset = 0;
                gex_movie_decoder_release(decoder);
            } else if (written < count) return;
            continue;
        }
        if (pending) {
            if ((int64_t)(hle_ticks_ms() - movie_start) < event.timestamp_ms) return;
#if GEX_ENABLE_PVR_RENDERER || GEX_ENABLE_GLDC_RENDERER
            if (gex_graphics_gpu_owned()) {
                if (!movie_pixels) movie_pixels = malloc((size_t)info.width * info.height * 2);
                if (!movie_pixels) hle_fatal("cannot allocate native movie decode buffer");
                if (gex_movie_decoder_render(decoder, movie_pixels, info.width, info.height,
                                              info.width * 2, GEX_MOVIE_RGB565_LE))
                    hle_fatal("movie decode failed: %s", gex_movie_decoder_error(decoder));
                gex_graphics_movie(movie_pixels, info.width, info.height, info.width * 2);
                ++video_frames;
                pending = 0;
                gex_movie_decoder_release(decoder);
                return;
            }
#endif
            gex_render_frame frame;
            if (gex_renderer_frame(&frame))
                hle_fatal("movie target unavailable: %s", gex_renderer_error());
            gex_render_rect rect = gex_renderer_fit(frame.width, frame.height, info.width, info.height);
            memset(frame.pixels, 0, (size_t)frame.pitch * frame.height);
            if (gex_movie_decoder_render(decoder,
                frame.pixels + (size_t)rect.y * frame.pitch + rect.x * 2, rect.width, rect.height,
                frame.pitch, GEX_MOVIE_RGB565_LE))
                hle_fatal("movie render failed: %s", gex_movie_decoder_error(decoder));
            if (gex_renderer_commit()) hle_fatal("movie presentation failed: %s", gex_renderer_error());
            ++video_frames;
            pending = 0;
            gex_movie_decoder_release(decoder);
            return;
        }
        int rc = gex_movie_decoder_next(decoder, &event);
        if (rc > 0) {
            pending = 1;
            continue;
        }
        if (rc == 0) {
            if (audio_stream >= 0 && hle_audio_stream_queued(audio_stream)) return;
            playing = 0;
            if (audio_stream >= 0) hle_audio_stream_close(audio_stream);
            audio_stream = -1;
            fprintf(stderr, "[movie] playback completed: video=%u audio=%llu elapsed-ms=%u\n",
                    video_frames, (unsigned long long)audio_frames, (unsigned)(hle_ticks_ms() - movie_start));
            if (callback) hle_post_message(callback, 0x3B9, 1, 1);
            return;
        }
        hle_fatal("movie decode failed: %s", gex_movie_decoder_error(decoder));
    }
}

static void mci_send_command(void) {
    uint32_t command = A32(1), params = A32(3);
    switch (command) {
    case 0x803: open_movie(); return;
    case 0x806: play_movie(); return;
    case 0x804: release_movie(); RET(0, 4);
    case 0x814:
        if (!params || !decoder) RET(257, 4);
        switch (MEM32(params + 8)) {
        case 0x4001: MEM32(params + 4) = movie_window; break;
        case 1: MEM32(params + 4) = info.duration_ms; break;
        default: hle_fatal("unsupported MCI_STATUS item 0x%X", MEM32(params + 8));
        }
        RET(0, 4);
    case 0x80D:
        if (!params || ((A32(2) & 0x400) && MEM32(params + 4) != 0)) RET(274, 4);
        RET(0, 4); /* MCI_FORMAT_MILLISECONDS is the decoder's time base. */
    case 0x840: RET(0, 4); /* RGB output needs no palette realization. */
    case 0x843:
        if (!params || !decoder) RET(257, 4);
        MEM32(params + 4) = 0; MEM32(params + 8) = 0;
        MEM32(params + 12) = info.width; MEM32(params + 16) = info.height;
        RET(0, 4);
    case 0x841:
        if (A32(2) != 0x40000) hle_fatal("unsupported MCI_WINDOW flags 0x%X", A32(2));
        { uint32_t args[] = {movie_window, MEM32(params + 8)};
          hle_call_guest(hle_resolve("ShowWindow"), 2, args); }
        RET(0, 4);
    case 0x808:
        cancel_playback();
        RET(0, 4);
    case 0x809:
        if (playing && !paused) {
            pause_started = hle_ticks_ms();
            paused = 1;
            if (audio_stream >= 0) hle_audio_stream_pause(audio_stream, 1);
        }
        RET(0, 4);
    case 0x855:
        if (playing && paused) {
            movie_start += hle_ticks_ms() - pause_started;
            paused = 0;
            if (audio_stream >= 0) hle_audio_stream_pause(audio_stream, 0);
        }
        RET(0, 4);
    case 0x807:
        if (!decoder || !(A32(2) & 0x100)) RET(274, 4);
        cancel_playback();
        if (gex_movie_decoder_rewind(decoder)) {
            fprintf(stderr, "[movie] %s\n", gex_movie_decoder_error(decoder));
            RET(277, 4);
        }
        RET(0, 4);
    case 0x854: /* update */
        gex_movie_pump();
        RET(0, 4);
    default:
        hle_fatal("unsupported MCI command 0x%X", command);
    }
}

static const win32hle_shim shims[] = {
    {"mciSendCommandA", mci_send_command},
    {NULL, NULL}
};

void gex_movie_register(int skip) {
    skip_movies = skip;
    win32hle_register(shims);
}
