#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>
#include "../src/platform/linux/audio.c"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;
static int failures;
#define EXPECT(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; } } while (0)
static int16_t output[8192 * 2];
static void render(int frames) {
    for (int at = 0; at < frames;) {
        int n = frames - at;
        if (n > 64) n = 64;
        gex_native_audio_poll();
        alcRenderSamplesSOFT(device, output + at * 2, n);
        at += n;
    }
}
static uint64_t energy(int channel, int begin, int end) {
    uint64_t value = 0;
    for (int i = begin; i < end; ++i) {
        int sample = output[i * 2 + channel];
        value += (uint64_t)(sample < 0 ? -sample : sample);
    }
    return value;
}
static float overlap_peak(int limiter) {
    device = alcLoopbackOpenDeviceSOFT(NULL);
    if (!device) { ++failures; return 0; }
    const ALCint attributes[] = {
        ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
        ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT,
        ALC_FREQUENCY, 44100, ALC_OUTPUT_LIMITER_SOFT, limiter, 0
    };
    context = alcCreateContext(device, attributes);
    if (!context || !alcMakeContextCurrent(context)) { ++failures; return 0; }
    alDistanceModel(AL_NONE);
    int16_t pcm[4096];
    for (int i = 0; i < 4096; ++i) pcm[i] = i % 128 < 64 ? 24000 : -24000;
    gex_native_voice *sounds[2];
    for (int i = 0; i < 2; ++i) {
        sounds[i] = gex_native_voice_create((const uint8_t *)pcm, 4096, 44100, 1, 16);
        if (!sounds[i]) { ++failures; return 0; }
        gex_native_voice_play(sounds[i], 1);
    }
    float samples[128 * 2], peak = 0;
    for (int at = 0; at < 8192; at += 128) {
        gex_native_audio_poll();
        alcRenderSamplesSOFT(device, samples, 128);
        if (at < 2048) continue;
        for (int i = 0; i < 128 * 2; ++i) {
            float sample = samples[i] < 0 ? -samples[i] : samples[i];
            if (sample > peak) peak = sample;
        }
    }
    for (int i = 0; i < 2; ++i) gex_native_voice_destroy(sounds[i]);
    gex_native_audio_close();
    return peak;
}
int main(void) {
    EXPECT(alcIsExtensionPresent(NULL, "ALC_SOFT_loopback"));
    device = alcLoopbackOpenDeviceSOFT(NULL);
    if (!device) return 1;
    const ALCint attributes[] = {
        ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
        ALC_FORMAT_TYPE_SOFT, ALC_SHORT_SOFT,
        ALC_FREQUENCY, 44100, 0
    };
    context = alcCreateContext(device, attributes);
    if (!context || !alcMakeContextCurrent(context)) return 1;
    alDistanceModel(AL_NONE);
    int16_t pcm[4096 * 2];
    for (int i = 0; i < 4096; ++i) { pcm[2 * i] = 10000; pcm[2 * i + 1] = -5000; }
    gex_native_voice *v = gex_native_voice_create((const uint8_t *)pcm, 4096, 44100, 2, 16);
    if (!v) return 1;
    gex_native_voice_play(v, 1);
    render(1024);
    EXPECT(energy(0, 256, 1024) > 4000000);
    EXPECT(energy(1, 256, 1024) > 2000000);
    gex_audio_position p = gex_native_voice_position(v);
    EXPECT(p.playing && p.frame >= 768 && p.frame <= 1280);
    /* Service once per 20 ms instead of the idealized 1.5 ms loop. */
    for (int at = 0; at < 4410; at += 882) {
        gex_native_audio_poll();
        alcRenderSamplesSOFT(device, output + at * 2, 882);
    }
    int gaps = 0;
    for (int i = 512; i < 4410; ++i)
        if (abs(output[i * 2]) < 1000) ++gaps;
    EXPECT(gaps == 0);
    gex_native_audio_poll();
    EXPECT(v->underruns == 0);
    EXPECT(gex_native_voice_position(v).write_lead >= 1760);
    gex_native_voice_gain(v, 1, 0);
    render(1024);
    EXPECT(energy(0, 512, 1024) > 2000000);
    EXPECT(energy(1, 512, 1024) < 10000);
    gex_native_voice_gain(v, 0, 0);
    render(1024);
    EXPECT(energy(0, 512, 1024) + energy(1, 512, 1024) < 20000);
    uint64_t before = gex_native_voice_position(v).played;
    int queued = v->queued;
    gex_native_voice_rate(v, 88200);
    EXPECT(v->queued == queued);
    ALint state;
    alGetSourcei(v->source[0], AL_SOURCE_STATE, &state);
    EXPECT(state == AL_PLAYING);
    gex_native_voice_rate(v, 88200);
    EXPECT(v->queued == queued);
    render(1024);
    uint64_t after = gex_native_voice_position(v).played;
    EXPECT(after - before >= 1792 && after - before <= 2304);
    gex_native_voice_stop(v);
    p = gex_native_voice_position(v);
    render(512);
    EXPECT(!gex_native_voice_position(v).playing);
    EXPECT(gex_native_voice_position(v).played == p.played);
    gex_native_voice_rate(v, 44100);
    gex_native_voice_seek(v, 3072);
    gex_native_voice_gain(v, 1, 1);
    gex_native_voice_play(v, 0);
    render(2048);
    p = gex_native_voice_position(v);
    EXPECT(!p.playing && p.played == 1024 && p.frame == 0);
    gex_native_voice_play(v, 0);
    render(1024);
    EXPECT(gex_native_voice_position(v).playing);
    gex_native_voice_destroy(v);
    uint8_t mono[1024];
    memset(mono, 192, sizeof mono);
    v = gex_native_voice_create(mono, 1024, 22050, 1, 8);
    if (!v) return 1;
    gex_native_voice_play(v, 1);
    render(8192);
    p = gex_native_voice_position(v);
    EXPECT(p.playing && p.played >= 3840 && p.played <= 4352);
    EXPECT(energy(0, 1024, 8192) > 10000000 && energy(1, 1024, 8192) > 10000000);
    memset(mono, 128, sizeof mono);
    gex_native_voice_update(v, 0, 1024);
    render(4096);
    EXPECT(energy(0, 3584, 4096) + energy(1, 3584, 4096) < 20000);
    gex_native_voice_destroy(v);
    int16_t loud[4096];
    for (int i = 0; i < 4096; ++i) loud[i] = i % 128 < 64 ? 24000 : -24000;
    v = gex_native_voice_create((const uint8_t *)loud, 4096, 44100, 1, 16);
    if (!v) return 1;
    gex_native_voice_play(v, 1);
    render(4096);
    int peak = 0, clipped = 0;
    for (int i = 1024; i < 4096 * 2; ++i) {
        int sample = abs(output[i]);
        if (sample > peak) peak = sample;
        if (sample >= 32760) ++clipped;
    }
    fprintf(stderr, "[audio-test] mono input peak=24000 output peak=%d clipped samples=%d\n", peak, clipped);
    EXPECT(peak <= 24500);
    gex_native_voice_destroy(v);
    gex_native_audio_close();
    float unbounded = overlap_peak(ALC_FALSE), limited = overlap_peak(ALC_TRUE);
    fprintf(stderr, "[audio-test] overlapping SFX float peaks: unbounded=%.4f limited=%.4f\n",
            unbounded, limited);
    EXPECT(unbounded > 1.4f);
    EXPECT(limited > 0.8f && limited <= 1.0f);
    EXPECT(gex_native_audio_open());
    if (context) {
        ALCint limiter = ALC_FALSE;
        alcGetIntegerv(device, ALC_OUTPUT_LIMITER_SOFT, 1, &limiter);
        EXPECT(limiter == ALC_TRUE);
        gex_native_audio_close();
    }
    return failures ? 1 : 0;
}
