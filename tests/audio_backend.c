/* Deterministic native-voice double, linked only into contract tests. Real
 * device integration is exercised separately against the platform backend. */
#include "../src/platform/audio.c"

struct gex_native_voice {
    struct gex_native_voice *next;
    const uint8_t *pcm;
    uint32_t frames, frame, write_lead;
    uint64_t played;
    int channels, bits, rate, playing, looping;
    float left, right;
};
static gex_native_voice *test_voices;
int gex_native_audio_open(void) { return 1; }
void gex_native_audio_close(void) {}
void gex_native_audio_poll(void) {}
gex_native_voice *gex_native_voice_create(const uint8_t *pcm, uint32_t frames,
                                         int rate, int channels, int bits) {
    gex_native_voice *v = calloc(1, sizeof *v);
    if (!v) abort();
    v->pcm = pcm; v->frames = frames; v->rate = rate; v->channels = channels; v->bits = bits;
    v->left = v->right = 1;
    v->next = test_voices; test_voices = v;
    return v;
}
void gex_native_voice_destroy(gex_native_voice *v) {
    gex_native_voice **link = &test_voices;
    while (*link != v) link = &(*link)->next;
    *link = v->next; free(v);
}
void gex_native_voice_update(gex_native_voice *v, uint32_t frame, uint32_t frames) {
    (void)v; (void)frame; (void)frames;
}
void gex_native_voice_play(gex_native_voice *v, int looping) { v->playing = 1; v->looping = looping; }
void gex_native_voice_stop(gex_native_voice *v) { v->playing = 0; }
void gex_native_voice_seek(gex_native_voice *v, uint32_t frame) { v->frame = frame; v->played = 0; }
gex_audio_position gex_native_voice_position(gex_native_voice *v) {
    return (gex_audio_position){v->frame, v->played, v->playing, v->write_lead};
}
void test_audio_write_lead(gex_audio_voice *voice, uint32_t frames) {
    gex_mutex_lock(&audio_lock);
    voice->native->write_lead = frames;
    voice->native->playing = 1;
    gex_mutex_unlock(&audio_lock);
}
void gex_native_voice_gain(gex_native_voice *v, float left, float right) { v->left = left; v->right = right; }
void gex_native_voice_rate(gex_native_voice *v, int rate) { v->rate = rate; }

/* Device pause disables real-time delivery; fixtures drive consumption here. */
void test_audio_render(uint8_t *output, int bytes) {
    memset(output, 0, (size_t)bytes);
    gex_mutex_lock(&audio_lock);
    for (gex_audio_voice *voice = voices; voice; voice = voice->next) {
        gex_native_voice *v = voice->native;
        if (!voice->playing) continue;
        for (int f = 0; f < bytes / 4; ++f) {
            for (int c = 0; c < 2; ++c) {
                uint32_t at = (v->frame * (uint32_t)v->channels +
                               (v->channels > 1 ? (uint32_t)c : 0)) * (uint32_t)(v->bits / 8);
                int16_t sample = v->bits == 8 ? (int16_t)(((int)v->pcm[at] - 128) * 256) :
                    (int16_t)((uint16_t)v->pcm[at] | (uint16_t)v->pcm[at + 1] << 8);
                float gain = c ? v->right : v->left;
                ((int16_t *)output)[f * 2 + c] = (int16_t)(sample * gain);
            }
            ++v->played;
            if (++v->frame == v->frames) {
                v->frame = 0;
                if (!voice->looping) { v->playing = voice->playing = 0; break; }
            }
        }
    }
    for (int i = 0; i < STREAMS; ++i) stream_poll(i);
    gex_mutex_unlock(&audio_lock);
}
