#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>
#define gex_native_audio_open gex_native_audio_open_device
#include "../src/platform/linux/audio.c"
#undef gex_native_audio_open

static int stream_loopback;
int gex_native_audio_open(void) {
    if (!stream_loopback) return gex_native_audio_open_device();
    device = alcLoopbackOpenDeviceSOFT(NULL);
    if (!device) return 0;
    const ALCint attributes[] = {
        ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
        ALC_FORMAT_TYPE_SOFT, ALC_SHORT_SOFT, ALC_FREQUENCY, 44100, 0
    };
    context = alcCreateContext(device, attributes);
    if (!context || !alcMakeContextCurrent(context)) {
        if (context) alcDestroyContext(context);
        alcCloseDevice(device);
        context = NULL; device = NULL;
        return 0;
    }
    alDistanceModel(AL_NONE);
    return 1;
}

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

static void movie_stream(int rate, int channels) {
    const int steps = 120, step_frames = 44100 / 15;
    int total = rate * 9;
    size_t count = (size_t)steps * step_frames * 2;
    int16_t *pcm = malloc((size_t)total * channels * sizeof *pcm);
    int16_t *reference = malloc(count * sizeof *reference);
    int16_t *streamed = malloc(count * sizeof *streamed);
    EXPECT(pcm && reference && streamed);
    if (!pcm || !reference || !streamed) {
        free(pcm); free(reference); free(streamed);
        return;
    }
    for (int i = 0; i < total; ++i)
        for (int c = 0; c < channels; ++c)
            pcm[i * channels + c] = (int16_t)(c ? (i % 311 - 155) * 40 : (i % 200 - 100) * 100);
    stream_loopback = 1;
    for (int pass = 0; pass < 2; ++pass) {
        EXPECT(gex_audio_open());
        int stream = -1, written = 0;
        ALuint sources[2] = {0}, buffers[2] = {0};
        if (pass) {
            stream = gex_audio_stream_open(rate, channels);
            EXPECT(stream >= 0);
            written = rate * 11 / 15;
            EXPECT(gex_audio_stream_write(stream, pcm, written) == written);
        } else {
            int16_t *mono = malloc((size_t)total * sizeof *mono);
            EXPECT(mono);
            if (!mono) { gex_audio_close(); break; }
            alGenSources(2, sources);
            alGenBuffers(2, buffers);
            for (int c = 0; c < 2; ++c) {
                for (int i = 0; i < total; ++i)
                    mono[i] = pcm[i * channels + (channels > 1 ? c : 0)];
                alBufferData(buffers[c], AL_FORMAT_MONO16, mono,
                             (ALsizei)((size_t)total * sizeof *mono), rate);
                alSourcei(sources[c], AL_SOURCE_RELATIVE, AL_TRUE);
                alSource3f(sources[c], AL_POSITION, c ? 1 : -1, 0, 0);
                alSourcef(sources[c], AL_ROLLOFF_FACTOR, 0);
                alSourcei(sources[c], AL_BUFFER, (ALint)buffers[c]);
            }
            free(mono);
            alSourcePlayv(2, sources);
            EXPECT(alGetError() == AL_NO_ERROR);
        }
        for (int step = 0; step < steps; ++step) {
            if (pass && step) {
                int next = rate * (step + 11) / 15;
                int frames = next - written;
                EXPECT(gex_audio_stream_write(stream, pcm + written * channels, frames) == frames);
                written = next;
            }
            int16_t *destination = (pass ? streamed : reference) + step * step_frames * 2;
            for (int at = 0; at < step_frames;) {
                if (pass) gex_audio_stream_queued(stream);
                int n = step_frames - at;
                if (n > 64) n = 64;
                alcRenderSamplesSOFT(device, destination + at * 2, n);
                at += n;
            }
        }
        if (!pass) {
            alDeleteSources(2, sources);
            alDeleteBuffers(2, buffers);
            EXPECT(alGetError() == AL_NO_ERROR);
        }
        gex_audio_close();
    }
    unsigned mismatches = 0;
    for (size_t i = 0; i < count; ++i)
        if (abs((int)reference[i] - streamed[i]) > 5) ++mismatches;
    fprintf(stderr, "[audio-test] movie stream %d Hz/%d channels: %u differing samples\n",
            rate, channels, mismatches);
    EXPECT(mismatches == 0);
    free(pcm); free(reference); free(streamed);
    stream_loopback = 0;
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
    movie_stream(11025, 2);
    movie_stream(22050, 1);
    return failures ? 1 : 0;
}
