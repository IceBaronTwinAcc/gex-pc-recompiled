#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "platform/audio_backend.h"
#include "platform/runtime.h"
#include "win32hle.h"

struct gex_audio_voice {
    struct gex_audio_voice *next;
    gex_native_voice *native;
    uint8_t *pcm;
    uint32_t frames;
    int block, playing, looping, owns_pcm;
    float left, right;
};
static gex_mutex audio_lock = GEX_MUTEX_INIT;
static gex_audio_voice *voices;
static int opened, running, worker_done, paused;
static float master = 1;
#define STREAMS 4
static struct {
    gex_audio_voice *voice;
    uint64_t written, read, played;
    int channels, paused;
} streams[STREAMS];

static void stream_poll(int id) {
    gex_audio_voice *v = streams[id].voice;
    if (!v || !v->playing) return;
    gex_audio_position p = gex_native_voice_position(v->native);
    uint64_t consumed = p.played - streams[id].played;
    uint64_t available = streams[id].written - streams[id].read;
    streams[id].played = p.played;
    if (consumed > available) consumed = available;
    while (consumed) {
        uint32_t at = (uint32_t)(streams[id].read % v->frames);
        uint32_t n = v->frames - at;
        if (n > consumed) n = (uint32_t)consumed;
        memset(v->pcm + (size_t)at * v->block, 0, (size_t)n * v->block);
        gex_native_voice_update(v->native, at, n);
        streams[id].read += n;
        consumed -= n;
    }
    if (streams[id].read == streams[id].written) {
        gex_native_voice_stop(v->native);
        v->playing = 0;
        gex_native_voice_seek(v->native, (uint32_t)(streams[id].read % v->frames));
        streams[id].played = 0;
    }
}
static void *audio_worker(void *unused) {
    (void)unused;
    for (;;) {
        gex_mutex_lock(&audio_lock);
        if (!running) {
            worker_done = 1;
            gex_mutex_unlock(&audio_lock);
            return NULL;
        }
        gex_native_audio_poll();
        if (!paused)
            for (int i = 0; i < STREAMS; ++i) stream_poll(i);
        gex_mutex_unlock(&audio_lock);
        gex_runtime_sleep_us(2000);
    }
}
int gex_audio_open(void) {
    gex_mutex_lock(&audio_lock);
    if (opened) { gex_mutex_unlock(&audio_lock); return 1; }
    if (!gex_native_audio_open()) { gex_mutex_unlock(&audio_lock); return 0; }
    running = opened = 1;
    worker_done = paused = 0;
    if (gex_runtime_start_thread(audio_worker, NULL)) {
        running = opened = 0;
        gex_native_audio_close();
        gex_mutex_unlock(&audio_lock);
        return 0;
    }
    gex_mutex_unlock(&audio_lock);
    return 1;
}
static void destroy(gex_audio_voice *v) {
    gex_audio_voice **link = &voices;
    while (*link && *link != v) link = &(*link)->next;
    if (!*link) hle_fatal("destroying unknown audio voice");
    *link = v->next;
    gex_native_voice_destroy(v->native);
    if (v->owns_pcm) free(v->pcm);
    free(v);
}
void gex_audio_close(void) {
    gex_mutex_lock(&audio_lock);
    if (!opened) { gex_mutex_unlock(&audio_lock); return; }
    running = 0;
    gex_mutex_unlock(&audio_lock);
    for (;;) {
        gex_mutex_lock(&audio_lock);
        int done = worker_done;
        gex_mutex_unlock(&audio_lock);
        if (done) break;
        gex_runtime_sleep_us(1000);
    }
    gex_mutex_lock(&audio_lock);
    while (voices) destroy(voices);
    memset(streams, 0, sizeof streams);
    gex_native_audio_close();
    opened = paused = 0;
    master = 1;
    gex_mutex_unlock(&audio_lock);
}
static gex_audio_voice *create_voice(const void *pcm, uint8_t *shared, uint32_t frames,
                                    int rate, int channels, int bits) {
    if (!frames || rate <= 0 || (channels != 1 && channels != 2) ||
        (bits != 8 && bits != 16) || (uint64_t)frames * channels * (bits / 8) > SIZE_MAX) {
        fprintf(stderr, "[audio] invalid PCM voice format\n");
        return NULL;
    }
    if (!gex_audio_open()) return NULL;
    gex_audio_voice *v = calloc(1, sizeof *v);
    if (!v) { fprintf(stderr, "[audio] cannot allocate voice\n"); return NULL; }
    v->frames = frames;
    v->block = channels * (bits / 8);
    v->left = v->right = 1;
    v->owns_pcm = !shared;
    v->pcm = shared ? shared : malloc((size_t)frames * v->block);
    if (!v->pcm) { free(v); fprintf(stderr, "[audio] cannot allocate PCM\n"); return NULL; }
    if (!shared) {
        if (pcm) memcpy(v->pcm, pcm, (size_t)frames * v->block);
        else memset(v->pcm, bits == 8 ? 128 : 0, (size_t)frames * v->block);
    }
    gex_mutex_lock(&audio_lock);
    v->native = gex_native_voice_create(v->pcm, frames, rate, channels, bits);
    if (v->native) {
        v->next = voices; voices = v;
        gex_native_voice_gain(v->native, master, master);
    }
    gex_mutex_unlock(&audio_lock);
    if (!v->native) { if (v->owns_pcm) free(v->pcm); free(v); return NULL; }
    return v;
}
gex_audio_voice *gex_audio_voice_create(const void *pcm, uint32_t frames,
                                       int rate, int channels, int bits) {
    return create_voice(pcm, NULL, frames, rate, channels, bits);
}
gex_audio_voice *gex_audio_voice_create_shared(uint8_t *pcm, uint32_t frames,
                                              int rate, int channels, int bits) {
    if (!pcm) { fprintf(stderr, "[audio] missing shared PCM storage\n"); return NULL; }
    return create_voice(NULL, pcm, frames, rate, channels, bits);
}
void gex_audio_voice_destroy(gex_audio_voice *v) {
    if (!v) return;
    gex_mutex_lock(&audio_lock); destroy(v); gex_mutex_unlock(&audio_lock);
}
void gex_audio_voice_update(gex_audio_voice *v, uint32_t at, const void *pcm, uint32_t n) {
    if (!v || at > v->frames || n > v->frames - at || (!pcm && n))
        hle_fatal("invalid PCM update");
    gex_mutex_lock(&audio_lock);
    uint8_t *destination = v->pcm + (size_t)at * v->block;
    if (destination != pcm && n) memcpy(destination, pcm, (size_t)n * v->block);
    gex_native_voice_update(v->native, at, n);
    gex_mutex_unlock(&audio_lock);
}
void gex_audio_voice_play(gex_audio_voice *v, int looping) {
    gex_mutex_lock(&audio_lock);
    v->playing = 1; v->looping = looping;
    if (!paused) gex_native_voice_play(v->native, looping);
    gex_mutex_unlock(&audio_lock);
}
void gex_audio_voice_stop(gex_audio_voice *v) {
    gex_mutex_lock(&audio_lock);
    gex_native_voice_stop(v->native); v->playing = 0;
    gex_mutex_unlock(&audio_lock);
}
void gex_audio_voice_seek(gex_audio_voice *v, uint32_t frame) {
    gex_mutex_lock(&audio_lock);
    gex_native_voice_seek(v->native, frame % v->frames);
    gex_mutex_unlock(&audio_lock);
}
gex_audio_position gex_audio_voice_position(gex_audio_voice *v) {
    gex_mutex_lock(&audio_lock);
    gex_audio_position p = gex_native_voice_position(v->native);
    if (!paused) v->playing = p.playing;
    gex_mutex_unlock(&audio_lock);
    return p;
}
void gex_audio_voice_gain(gex_audio_voice *v, float left, float right) {
    gex_mutex_lock(&audio_lock);
    v->left = left; v->right = right;
    gex_native_voice_gain(v->native, left * master, right * master);
    gex_mutex_unlock(&audio_lock);
}
void gex_audio_voice_rate(gex_audio_voice *v, int rate) {
    gex_mutex_lock(&audio_lock);
    gex_native_voice_rate(v->native, rate);
    gex_mutex_unlock(&audio_lock);
}
void gex_audio_master(float gain) {
    gex_mutex_lock(&audio_lock);
    master = gain;
    for (gex_audio_voice *v = voices; v; v = v->next)
        gex_native_voice_gain(v->native, v->left * master, v->right * master);
    gex_mutex_unlock(&audio_lock);
}
void gex_audio_pause(int on) {
    gex_mutex_lock(&audio_lock);
    if (paused != !!on) {
        paused = !!on;
        for (gex_audio_voice *v = voices; v; v = v->next) {
            if (on) {
                v->playing = gex_native_voice_position(v->native).playing;
                gex_native_voice_stop(v->native);
            } else if (v->playing) gex_native_voice_play(v->native, v->looping);
        }
    }
    gex_mutex_unlock(&audio_lock);
}
int gex_audio_stream_open(int rate, int channels) {
    if (rate <= 0 || (channels != 1 && channels != 2)) {
        fprintf(stderr, "[audio] invalid stream format\n"); return -1;
    }
    gex_audio_voice *v = gex_audio_voice_create(NULL, GEX_AUDIO_STREAM_FRAMES, rate, channels, 16);
    if (!v) return -1;
    gex_mutex_lock(&audio_lock);
    for (int i = 0; i < STREAMS; ++i) if (!streams[i].voice) {
        memset(&streams[i], 0, sizeof streams[i]);
        streams[i].voice = v; streams[i].channels = channels;
        gex_mutex_unlock(&audio_lock); return i;
    }
    destroy(v);
    gex_mutex_unlock(&audio_lock);
    fprintf(stderr, "[audio] all stream slots are in use\n");
    return -1;
}
static int valid_stream(int id) { return id >= 0 && id < STREAMS && streams[id].voice; }
int gex_audio_stream_write(int id, const int16_t *pcm, int frames) {
    gex_mutex_lock(&audio_lock);
    if (!valid_stream(id) || frames < 0 || (!pcm && frames)) hle_fatal("invalid audio stream write");
    stream_poll(id);
    gex_audio_voice *v = streams[id].voice;
    int free_frames = (int)(v->frames - 1 - (streams[id].written - streams[id].read));
    int accepted = frames < free_frames ? frames : free_frames;
    int remaining = accepted;
    while (remaining) {
        uint32_t at = (uint32_t)(streams[id].written % v->frames);
        uint32_t n = v->frames - at;
        if (n > (uint32_t)remaining) n = (uint32_t)remaining;
        memcpy(v->pcm + (size_t)at * v->block, pcm, (size_t)n * v->block);
        gex_native_voice_update(v->native, at, n);
        streams[id].written += n;
        pcm += n * streams[id].channels;
        remaining -= (int)n;
    }
    if (accepted && !v->playing && !streams[id].paused) {
        v->playing = v->looping = 1;
        if (!paused) gex_native_voice_play(v->native, 1);
    }
    gex_mutex_unlock(&audio_lock);
    return accepted;
}
int gex_audio_stream_queued(int id) {
    gex_mutex_lock(&audio_lock);
    if (!valid_stream(id)) { gex_mutex_unlock(&audio_lock); return 0; }
    stream_poll(id);
    int n = (int)(streams[id].written - streams[id].read);
    gex_mutex_unlock(&audio_lock); return n;
}
void gex_audio_stream_gain(int id, float gain) {
    gex_mutex_lock(&audio_lock);
    if (valid_stream(id)) {
        gex_audio_voice *v = streams[id].voice;
        v->left = v->right = gain;
        gex_native_voice_gain(v->native, gain * master, gain * master);
    }
    gex_mutex_unlock(&audio_lock);
}
void gex_audio_stream_pause(int id, int on) {
    gex_mutex_lock(&audio_lock);
    if (valid_stream(id)) {
        stream_poll(id);
        gex_audio_voice *v = streams[id].voice;
        streams[id].paused = !!on;
        if (on) { gex_native_voice_stop(v->native); v->playing = 0; }
        else if (streams[id].written > streams[id].read) {
            v->playing = v->looping = 1;
            if (!paused) gex_native_voice_play(v->native, 1);
        }
    }
    gex_mutex_unlock(&audio_lock);
}
void gex_audio_stream_close(int id) {
    gex_mutex_lock(&audio_lock);
    if (valid_stream(id)) { destroy(streams[id].voice); memset(&streams[id], 0, sizeof streams[id]); }
    gex_mutex_unlock(&audio_lock);
}
