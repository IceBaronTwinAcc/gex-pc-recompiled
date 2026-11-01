#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/audio_backend.h"
#include "win32hle.h"

struct gex_native_voice {
    LPDIRECTSOUNDBUFFER buffer;
    const uint8_t *pcm;
    uint32_t frames, last;
    int block, playing, looping;
    uint64_t played;
};
static LPDIRECTSOUND device;
static void check(HRESULT result, const char *operation) {
    if (FAILED(result)) hle_fatal("DirectSound %s failed (0x%08lx)", operation, (unsigned long)result);
}
int gex_native_audio_open(void) {
    HRESULT result = DirectSoundCreate(NULL, &device, NULL);
    if (FAILED(result)) {
        fprintf(stderr, "[audio] DirectSound device unavailable (0x%08lx)\n", (unsigned long)result);
        device = NULL; return 0;
    }
    result = IDirectSound_SetCooperativeLevel(device, GetDesktopWindow(), DSSCL_NORMAL);
    if (FAILED(result)) {
        fprintf(stderr, "[audio] DirectSound cooperative level failed (0x%08lx)\n", (unsigned long)result);
        IDirectSound_Release(device); device = NULL; return 0;
    }
    fprintf(stderr, "[audio] Windows DirectSound native voices\n");
    return 1;
}
void gex_native_audio_close(void) { IDirectSound_Release(device); device = NULL; }
void gex_native_audio_poll(void) {}
void gex_native_voice_update(gex_native_voice *v, uint32_t at, uint32_t frames) {
    if (!frames) return;
    void *first, *second;
    DWORD n1, n2;
    check(IDirectSoundBuffer_Lock(v->buffer, at * v->block, frames * v->block,
                                 &first, &n1, &second, &n2, 0), "lock");
    memcpy(first, v->pcm + (size_t)at * v->block, n1);
    if (n2) memcpy(second, v->pcm, n2);
    check(IDirectSoundBuffer_Unlock(v->buffer, first, n1, second, n2), "unlock");
}
gex_native_voice *gex_native_voice_create(const uint8_t *pcm, uint32_t frames,
                                         int rate, int channels, int bits) {
    gex_native_voice *v = calloc(1, sizeof *v);
    if (!v) { fprintf(stderr, "[audio] cannot allocate DirectSound voice\n"); return NULL; }
    v->pcm = pcm; v->frames = frames; v->block = channels * (bits / 8);
    WAVEFORMATEX format = {WAVE_FORMAT_PCM, (WORD)channels, (DWORD)rate,
                           (DWORD)(rate * v->block), (WORD)v->block, (WORD)bits, 0};
    DSBUFFERDESC desc = {0};
    desc.dwSize = sizeof desc;
    desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY |
                   DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
    desc.dwBufferBytes = frames * v->block;
    desc.lpwfxFormat = &format;
    HRESULT result = IDirectSound_CreateSoundBuffer(device, &desc, &v->buffer, NULL);
    if (FAILED(result)) {
        fprintf(stderr, "[audio] cannot allocate DirectSound buffer (0x%08lx)\n", (unsigned long)result);
        free(v); return NULL;
    }
    gex_native_voice_update(v, 0, frames);
    return v;
}
void gex_native_voice_destroy(gex_native_voice *v) {
    IDirectSoundBuffer_Stop(v->buffer);
    IDirectSoundBuffer_Release(v->buffer); free(v);
}
gex_audio_position gex_native_voice_position(gex_native_voice *v) {
    DWORD play, status;
    check(IDirectSoundBuffer_GetCurrentPosition(v->buffer, &play, NULL), "read cursor");
    check(IDirectSoundBuffer_GetStatus(v->buffer, &status), "read status");
    uint32_t frame = play / (uint32_t)v->block;
    if (v->playing) {
        if (!(status & DSBSTATUS_PLAYING) && !v->looping)
            v->played += v->frames - v->last;
        else v->played += (frame + v->frames - v->last) % v->frames;
    }
    v->last = frame;
    v->playing = (status & DSBSTATUS_PLAYING) != 0;
    return (gex_audio_position){frame, v->played, v->playing, 0};
}
void gex_native_voice_play(gex_native_voice *v, int looping) {
    check(IDirectSoundBuffer_Play(v->buffer, 0, 0, looping ? DSBPLAY_LOOPING : 0), "play");
    v->playing = 1; v->looping = looping;
}
void gex_native_voice_stop(gex_native_voice *v) {
    (void)gex_native_voice_position(v);
    check(IDirectSoundBuffer_Stop(v->buffer), "stop");
    v->playing = 0;
}
void gex_native_voice_seek(gex_native_voice *v, uint32_t frame) {
    check(IDirectSoundBuffer_SetCurrentPosition(v->buffer, frame * v->block), "seek");
    v->last = frame; v->played = 0;
}
static LONG decibels(float gain) {
    if (gain <= 0.00001f) return -10000;
    if (gain >= 1) return 0;
    return (LONG)(2000 * log10f(gain));
}
void gex_native_voice_gain(gex_native_voice *v, float left, float right) {
    float volume = left > right ? left : right;
    LONG pan = left > right ? decibels(right / (left ? left : 1)) :
                             -decibels(left / (right ? right : 1));
    check(IDirectSoundBuffer_SetVolume(v->buffer, decibels(volume)), "set volume");
    check(IDirectSoundBuffer_SetPan(v->buffer, pan), "set pan");
}
void gex_native_voice_rate(gex_native_voice *v, int rate) {
    check(IDirectSoundBuffer_SetFrequency(v->buffer, (DWORD)rate), "set frequency");
}
