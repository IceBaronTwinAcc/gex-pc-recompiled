#ifndef GEX_PLATFORM_AUDIO_BACKEND_H
#define GEX_PLATFORM_AUDIO_BACKEND_H

#include "platform/audio.h"

typedef struct gex_native_voice gex_native_voice;
/* Called under the shared audio lock. PCM remains live through destruction.
 * poll services native queues only; it must never execute guest code.
 * played is cumulative since the last seek, including complete loop wraps. */
int gex_native_audio_open(void);
void gex_native_audio_close(void);
void gex_native_audio_poll(void);
gex_native_voice *gex_native_voice_create(const uint8_t *pcm, uint32_t frames,
                                         int rate, int channels, int bits);
void gex_native_voice_destroy(gex_native_voice *voice);
void gex_native_voice_update(gex_native_voice *voice, uint32_t frame, uint32_t frames);
void gex_native_voice_play(gex_native_voice *voice, int looping);
void gex_native_voice_stop(gex_native_voice *voice);
void gex_native_voice_seek(gex_native_voice *voice, uint32_t frame);
gex_audio_position gex_native_voice_position(gex_native_voice *voice);
void gex_native_voice_gain(gex_native_voice *voice, float left, float right);
void gex_native_voice_rate(gex_native_voice *voice, int rate);

#endif
