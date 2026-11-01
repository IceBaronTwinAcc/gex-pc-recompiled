#include <stdio.h>
#include <stdint.h>
#include "platform/audio.h"
#include "platform/runtime.h"
#include "win32hle.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;
static int failures;
#define EXPECT(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; } } while (0)
int main(void) {
    if (!gex_audio_open()) return 77;
    int16_t pcm[4410 * 2] = {0};
    gex_audio_voice *v = gex_audio_voice_create(pcm, 4410, 44100, 2, 16);
    if (!v) { gex_audio_close(); return 1; }
    gex_audio_voice_play(v, 1);
    gex_runtime_sleep_us(40000);
    gex_audio_position before = gex_audio_voice_position(v);
    EXPECT(before.playing && before.played > 0);
    gex_audio_pause(1);
    before = gex_audio_voice_position(v);
    gex_runtime_sleep_us(30000);
    EXPECT(gex_audio_voice_position(v).played == before.played);
    gex_audio_pause(0);
    gex_runtime_sleep_us(30000);
    EXPECT(gex_audio_voice_position(v).played > before.played);
    gex_audio_voice_gain(v, 0.25f, 0);
    gex_audio_voice_rate(v, 22050);
    gex_audio_voice_seek(v, 0);
    gex_audio_voice_play(v, 0);
    gex_runtime_sleep_us(250000);
    EXPECT(!gex_audio_voice_position(v).playing);
    gex_audio_voice_destroy(v);
    int stream = gex_audio_stream_open(44100, 2);
    EXPECT(stream >= 0);
    if (stream >= 0) {
        gex_audio_stream_pause(stream, 1);
        EXPECT(gex_audio_stream_write(stream, pcm, 4410) == 4410);
        gex_runtime_sleep_us(30000);
        EXPECT(gex_audio_stream_queued(stream) == 4410);
        gex_audio_stream_pause(stream, 0);
        gex_runtime_sleep_us(150000);
        EXPECT(gex_audio_stream_queued(stream) == 0);
        EXPECT(gex_audio_stream_write(stream, pcm, 4410) == 4410);
        gex_runtime_sleep_us(150000);
        EXPECT(gex_audio_stream_queued(stream) == 0);
        gex_audio_stream_close(stream);
    }
    gex_audio_close();
    return failures ? 1 : 0;
}
