#include <al.h>
#include <alc.h>
#if defined(__has_include)
#if __has_include(<alext.h>)
#include <alext.h>
#endif
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "platform/audio_backend.h"
#include "win32hle.h"

#define QUEUES 8
#define CHUNK 512
struct gex_native_voice {
    struct gex_native_voice *next;
    const uint8_t *pcm;
    ALuint source[2], buffer[2][QUEUES];
    uint32_t length[QUEUES], frames, cursor, next_frame;
    uint64_t played;
    int channels, bits, rate, frequency, playing, looping, head, queued;
    unsigned underruns;
};
static ALCdevice *device;
static ALCcontext *context;
static gex_native_voice *voices;
static void check(const char *operation) {
    ALenum error = alGetError();
    if (error != AL_NO_ERROR) hle_fatal("OpenAL %s failed (0x%x)", operation, error);
}
int gex_native_audio_open(void) {
    /* Dummy SDL is an explicit headless test request, not a device-failure fallback. */
    const char *dummy = getenv("SDL_AUDIODRIVER");
    if (dummy && !strcmp(dummy, "dummy") && !getenv("ALSOFT_DRIVERS"))
        setenv("ALSOFT_DRIVERS", "null", 0);
    device = alcOpenDevice(NULL);
    if (!device) { fprintf(stderr, "[audio] OpenAL device unavailable\n"); return 0; }
    ALCint attributes[5];
    int count = 0, limiter = 0;
#ifdef ALC_SOFT_output_limiter
    limiter = alcIsExtensionPresent(device, "ALC_SOFT_output_limiter");
    if (limiter) {
        attributes[count++] = ALC_OUTPUT_LIMITER_SOFT;
        attributes[count++] = ALC_TRUE;
    } else fprintf(stderr, "[audio] OpenAL output limiter unavailable; overlapping voices may clip\n");
#else
    fprintf(stderr, "[audio] OpenAL headers lack output limiter support; overlapping voices may clip\n");
#endif
#ifdef ALC_SOFT_HRTF
    if (alcIsExtensionPresent(device, "ALC_SOFT_HRTF")) {
        attributes[count++] = ALC_HRTF_SOFT;
        attributes[count++] = ALC_FALSE;
    }
#endif
    attributes[count] = 0;
    context = alcCreateContext(device, attributes);
    if (!context || !alcMakeContextCurrent(context)) {
        fprintf(stderr, "[audio] cannot create OpenAL context\n");
        if (context) alcDestroyContext(context);
        alcCloseDevice(device); context = NULL; device = NULL; return 0;
    }
    alDistanceModel(AL_NONE);
    check("initialize");
#ifdef ALC_SOFT_output_limiter
    if (limiter) {
        ALCint enabled = ALC_FALSE;
        alcGetIntegerv(device, ALC_OUTPUT_LIMITER_SOFT, 1, &enabled);
        if (alcGetError(device) != ALC_NO_ERROR || !enabled) {
            fprintf(stderr, "[audio] OpenAL failed to enable the output limiter\n");
            alcMakeContextCurrent(NULL);
            alcDestroyContext(context); alcCloseDevice(device);
            context = NULL; device = NULL;
            return 0;
        }
        fprintf(stderr, "[audio] OpenAL output limiter enabled\n");
    }
#endif
    fprintf(stderr, "[audio] OpenAL native voices (%s)\n", alcGetString(device, ALC_DEVICE_SPECIFIER));
    return 1;
}
void gex_native_audio_close(void) {
    alcMakeContextCurrent(NULL);
    alcDestroyContext(context);
    alcCloseDevice(device);
    context = NULL; device = NULL;
}
gex_native_voice *gex_native_voice_create(const uint8_t *pcm, uint32_t frames,
                                         int rate, int channels, int bits) {
    gex_native_voice *v = calloc(1, sizeof *v);
    if (!v) { fprintf(stderr, "[audio] cannot allocate OpenAL voice\n"); return NULL; }
    v->pcm = pcm; v->frames = frames; v->rate = v->frequency = rate;
    v->channels = channels; v->bits = bits;
    alGenSources(2, v->source);
    if (alGetError() != AL_NO_ERROR) {
        fprintf(stderr, "[audio] OpenAL voice channels exhausted\n"); free(v); return NULL;
    }
    for (int c = 0; c < 2; ++c) {
        alGenBuffers(QUEUES, v->buffer[c]);
        alSourcei(v->source[c], AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(v->source[c], AL_POSITION, c ? 1 : -1, 0, 0);
        alSourcef(v->source[c], AL_ROLLOFF_FACTOR, 0);
    }
    check("allocate voice buffers");
    v->next = voices; voices = v;
    return v;
}
static void clear(gex_native_voice *v) {
    alSourceStopv(2, v->source);
    for (int c = 0; c < 2; ++c) alSourcei(v->source[c], AL_BUFFER, 0);
    v->head = v->queued = 0;
    v->next_frame = v->cursor;
    check("clear voice queue");
}
void gex_native_voice_destroy(gex_native_voice *v) {
    gex_native_voice **link = &voices;
    while (*link != v) link = &(*link)->next;
    *link = v->next;
    clear(v);
    alDeleteSources(2, v->source);
    for (int c = 0; c < 2; ++c) alDeleteBuffers(QUEUES, v->buffer[c]);
    check("destroy voice"); free(v);
}
static void poll(gex_native_voice *v) {
    if (!v->playing) return;
    ALint done;
    alGetSourcei(v->source[0], AL_BUFFERS_PROCESSED, &done);
    ALint right_done;
    alGetSourcei(v->source[1], AL_BUFFERS_PROCESSED, &right_done);
    if (right_done < done) done = right_done;
    if (v->queued && done == v->queued && (v->looping || v->next_frame < v->frames)) {
        ++v->underruns;
        if ((v->underruns & (v->underruns - 1)) == 0)
            fprintf(stderr, "[audio] OpenAL queue underrun (%u), playback rate=%d\n",
                    v->underruns, v->frequency);
    }
    while (done-- > 0) {
        ALuint buffer;
        for (int c = 0; c < 2; ++c) alSourceUnqueueBuffers(v->source[c], 1, &buffer);
        uint32_t n = v->length[v->head];
        v->cursor = (v->cursor + n) % v->frames;
        v->played += n;
        v->head = (v->head + 1) % QUEUES; --v->queued;
    }
    while (v->queued < (v->frequency < 200 ? 2 : QUEUES)) {
        if (!v->looping && v->next_frame >= v->frames) break;
        uint32_t n = (uint32_t)(v->frequency / 200);
        if (n < 1) n = 1;
        if (n > CHUNK) n = CHUNK;
        if (!v->looping && n > v->frames - v->next_frame) n = v->frames - v->next_frame;
        int slot = (v->head + v->queued) % QUEUES;
        uint8_t mono[CHUNK * 2];
        for (int c = 0; c < 2; ++c) {
            for (uint32_t f = 0; f < n; ++f) {
                uint32_t at = (v->next_frame + f) % v->frames;
                const uint8_t *sample = v->pcm + ((size_t)at * v->channels +
                    (v->channels > 1 ? c : 0)) * (v->bits / 8);
                memcpy(mono + f * (v->bits / 8), sample, (size_t)v->bits / 8);
            }
            alBufferData(v->buffer[c][slot], v->bits == 8 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16,
                         mono, (ALsizei)(n * (v->bits / 8)), v->rate);
            alSourceQueueBuffers(v->source[c], 1, &v->buffer[c][slot]);
        }
        v->length[slot] = n; ++v->queued;
        v->next_frame += n;
        if (v->looping) v->next_frame %= v->frames;
    }
    if (v->queued) {
        ALint state;
        alGetSourcei(v->source[0], AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) alSourcePlayv(2, v->source);
    } else { v->playing = 0; v->next_frame = v->cursor; }
    check("service voice");
}
void gex_native_audio_poll(void) { for (gex_native_voice *v = voices; v; v = v->next) poll(v); }
gex_audio_position gex_native_voice_position(gex_native_voice *v) {
    poll(v);
    ALint offset = 0;
    if (v->playing) alGetSourcei(v->source[0], AL_SAMPLE_OFFSET, &offset);
    check("read cursor");
    uint32_t chunk = (uint32_t)(v->frequency / 200);
    if (!chunk) chunk = 1;
    if (chunk > CHUNK) chunk = CHUNK;
    uint32_t lead = (QUEUES + 2) * chunk;
    uint32_t committed = 0;
    for (int i = 0; i < v->queued; ++i)
        committed += v->length[(v->head + i) % QUEUES];
    if (committed + 2 * chunk > lead) lead = committed + 2 * chunk;
    return (gex_audio_position){(v->cursor + (uint32_t)offset) % v->frames,
                                v->played + (uint32_t)offset, v->playing, lead};
}
void gex_native_voice_stop(gex_native_voice *v) {
    gex_audio_position p = gex_native_voice_position(v);
    v->cursor = p.frame; v->played = p.played; v->playing = 0;
    clear(v);
}
void gex_native_voice_play(gex_native_voice *v, int looping) {
    if (v->playing && v->looping == looping) return;
    if (v->playing) gex_native_voice_stop(v);
    v->looping = looping; v->playing = 1;
    poll(v);
}
void gex_native_voice_seek(gex_native_voice *v, uint32_t frame) {
    int playing = v->playing;
    gex_native_voice_stop(v);
    v->cursor = v->next_frame = frame; v->played = 0;
    if (playing) gex_native_voice_play(v, v->looping);
}
void gex_native_voice_update(gex_native_voice *v, uint32_t frame, uint32_t frames) {
    /* Queued packets are immutable. Later packets observe the updated PCM.
     * The queue is bounded to 40 ms at the selected playback rate. */
    (void)v; (void)frame; (void)frames;
}
void gex_native_voice_gain(gex_native_voice *v, float left, float right) {
    alSourcef(v->source[0], AL_GAIN, left);
    alSourcef(v->source[1], AL_GAIN, right);
    check("set gain");
}
void gex_native_voice_rate(gex_native_voice *v, int rate) {
    if (v->frequency == rate) return;
    v->frequency = rate;
    alSourcef(v->source[0], AL_PITCH, (float)rate / v->rate);
    alSourcef(v->source[1], AL_PITCH, (float)rate / v->rate);
    check("set frequency");
}
