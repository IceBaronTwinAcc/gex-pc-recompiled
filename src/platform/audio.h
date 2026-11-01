#ifndef GEX_PLATFORM_AUDIO_H
#define GEX_PLATFORM_AUDIO_H

#include <stdint.h>

typedef struct gex_audio_voice gex_audio_voice;
typedef struct {
    uint32_t frame;
    uint64_t played;
    int playing;
    uint32_t write_lead; /* Frames already committed to native playback. */
} gex_audio_position;

/* PCM is interleaved unsigned U8 or little-endian S16. All offsets are frames.
 * The implementation owns a copy; callers may release or change their input.
 * There is no sample mixer or resampler in the shared audio layer. */
int gex_audio_open(void);
void gex_audio_pause(int paused);
void gex_audio_close(void);
gex_audio_voice *gex_audio_voice_create(const void *pcm, uint32_t frames,
                                       int rate, int channels, int bits);
void gex_audio_voice_destroy(gex_audio_voice *voice);
void gex_audio_voice_update(gex_audio_voice *voice, uint32_t frame,
                            const void *pcm, uint32_t frames);
void gex_audio_voice_play(gex_audio_voice *voice, int looping);
void gex_audio_voice_stop(gex_audio_voice *voice);
void gex_audio_voice_seek(gex_audio_voice *voice, uint32_t frame);
gex_audio_position gex_audio_voice_position(gex_audio_voice *voice);
void gex_audio_voice_gain(gex_audio_voice *voice, float left, float right);
void gex_audio_voice_rate(gex_audio_voice *voice, int rate);
void gex_audio_master(float gain);

#define GEX_AUDIO_STREAM_FRAMES 16384
int gex_audio_stream_open(int rate, int channels);
int gex_audio_stream_write(int id, const int16_t *pcm, int frames);
int gex_audio_stream_queued(int id);
void gex_audio_stream_gain(int id, float gain);
void gex_audio_stream_pause(int id, int paused);
void gex_audio_stream_close(int id);

#endif
