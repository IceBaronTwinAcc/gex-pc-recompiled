#include "dsound.c"
#include "../src/platform_shims.c"
#include "host.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;
static int failures;
void test_audio_render(uint8_t *output, int bytes);

static void expect(int condition, const char *description) {
    if (!condition) {
        fprintf(stderr, "%s\n", description);
        ++failures;
    }
}

static uint32_t sound(uint32_t source, uint32_t flags) {
    uint32_t args[] = {source, flags};
    return hle_call_guest(hle_resolve("sndPlaySoundA"), 2, args);
}

int main(void) {
    recomp_host_init();
    gex_register_platform_shims();
    int warmup = hle_audio_stream_open(44100, 1);
    if (warmup < 0) return 1;
    gex_audio_pause(1);
    hle_audio_stream_close(warmup);
    mach_enter();
    unsigned samples = STREAM_FRAMES + 4096;
    uint32_t wav = hle_va(hle_alloc(44 + samples * 2));
    uint8_t *bytes = hle_ptr(wav);
    memcpy(bytes, "RIFF", 4);
    MEM32(wav + 4) = 36 + samples * 2;
    memcpy(bytes + 8, "WAVEfmt ", 8);
    MEM32(wav + 16) = 16;
    MEM16(wav + 20) = 1; MEM16(wav + 22) = 1;
    MEM32(wav + 24) = 44100; MEM32(wav + 28) = 88200;
    MEM16(wav + 32) = 2; MEM16(wav + 34) = 16;
    memcpy(bytes + 36, "data", 4);
    MEM32(wav + 40) = samples * 2;
    for (unsigned i = 0; i < samples; ++i) MEM16(wav + 44 + i * 2) = 10000;
    expect(sound(wav, 5) == 1, "start async memory WAV larger than the ring");
    expect(wave_samples != NULL && wave_offset < wave_frames, "retain unqueued WAV tail");
    expect(sound(wav, 0x15) == 0, "SND_NOSTOP preserves active playback");
    unsigned rendered = 0;
    int16_t output[4096 * 2];
    for (unsigned i = 0; i < 100 && wave_stream >= 0; ++i) {
        gex_platform_pump();
        int count = hle_audio_stream_queued(wave_stream);
        if (count > 4096) count = 4096;
        if (count) {
            test_audio_render((uint8_t *)output, count * 4);
            for (int j = 0; j < count * 2; ++j)
                expect(output[j] > 9900, "WAV samples remain intact across refills");
            rendered += count;
        }
    }
    expect(rendered == samples, "render the entire WAV without ring truncation");
    expect(wave_stream == -1 && wave_samples == NULL, "release completed WAV resources");
    expect(sound(wav, 13) == 1, "start looping async WAV");
    for (int i = 0; i < 80; ++i) {
        gex_platform_pump();
        test_audio_render((uint8_t *)output, sizeof output);
    }
    expect(wave_stream >= 0 && wave_samples != NULL, "loop retains source beyond one pass");
    expect(sound(0, 0) == 1, "stop looping WAV");
    expect(wave_stream == -1 && wave_samples == NULL, "stop releases WAV resources");
    expect(sound(wav, 12) == 0, "reject synchronous looping WAV");
    MEM16(wav + 22) = 3;
    MEM32(wav + 28) = 44100 * 6;
    MEM16(wav + 32) = 6;
    expect(sound(wav, 5) == 0, "reject multichannel memory WAV");
    expect(wave_stream == -1 && wave_samples == NULL, "invalid WAV leaves no active source");
    MEM32(wav + 4) = 0;
    expect(sound(wav, 5) == 0, "reject undersized RIFF payload");
    expect(wave_stream == -1 && wave_samples == NULL, "invalid RIFF leaves no stream");
    mach_leave();
    gex_audio_close();
    return failures ? 1 : 0;
}
