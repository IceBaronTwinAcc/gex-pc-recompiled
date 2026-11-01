#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsound.c"
#include "host.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, NULL}};
const uint32_t recomp_dispatch_count = 0;
static int failures;
void test_audio_render(uint8_t *output, int bytes);
void test_audio_write_lead(gex_audio_voice *voice, uint32_t frames);

static void expect(int condition, const char *description) {
    if (!condition) {
        fprintf(stderr, "%s\n", description);
        ++failures;
    }
}

static uint32_t method(uint32_t object, int index, int count, uint32_t *args) {
    uint32_t call[9] = {object};
    if (count) memcpy(call + 1, args, (size_t)count * sizeof *args);
    return hle_call_guest(MEM32(MEM32(object) + (uint32_t)index * 4), count + 1, call);
}
static void guest_buffers(void) {
    mach_enter();
    uint32_t memory = hle_va(hle_alloc(128));
    uint32_t args[8] = {0, memory, 0};
    expect(!hle_call_guest(hle_resolve("DirectSoundCreate"), 3, args), "create guest device");
    uint32_t device = MEM32(memory);
    uint32_t desc = memory + 8, format = memory + 32, result = memory + 64;
    MEM32(desc) = 20; MEM32(desc + 4) = 0xE0; MEM32(desc + 8) = 128; MEM32(desc + 16) = format;
    MEM16(format) = 1; MEM16(format + 2) = 1; MEM32(format + 4) = 44100;
    MEM32(format + 8) = 44100; MEM16(format + 12) = 1; MEM16(format + 14) = 8;
    args[0] = desc; args[1] = result; args[2] = 0;
    expect(!method(device, 3, 3, args), "create native-backed U8 mono guest buffer");
    uint32_t original = MEM32(result);
    args[0] = 0; args[1] = 128; args[2] = result; args[3] = result + 4;
    args[4] = result + 8; args[5] = result + 12; args[6] = 2;
    expect(!method(original, 11, 7, args), "lock entire guest buffer");
    uint32_t samples = MEM32(result);
    memset(hle_ptr(samples), 192, 128);
    args[0] = samples; args[1] = 128; args[2] = args[3] = 0;
    expect(!method(original, 19, 4, args), "unlock uploads PCM");
    args[0] = original; args[1] = result;
    expect(!method(device, 5, 2, args), "duplicate guest sample storage with independent native playback");
    uint32_t duplicate = MEM32(result);
    args[0] = args[1] = args[2] = 0;
    expect(!method(original, 12, 3, args), "start guest one-shot");
    int16_t output[32 * 2];
    test_audio_render((uint8_t *)output, sizeof output);
    expect(output[0] == 16384 && output[1] == 16384, "guest PCM reaches native voice unchanged");
    args[0] = result; args[1] = 0;
    expect(!method(original, 4, 2, args) && MEM32(result) == 32, "guest cursor follows native voice");
    test_audio_write_lead(buf_of(original)->voice, 0);
    args[0] = result; args[1] = result + 4;
    expect(!method(original, 4, 2, args) && MEM32(result + 4) == (32 + 882) % 128,
           "default guest write cursor retains its 20 ms lead");
    test_audio_write_lead(buf_of(original)->voice, 1000);
    expect(!method(original, 4, 2, args) && MEM32(result + 4) == (32 + 1000) % 128,
           "guest write cursor respects longer native queue reservation");
    args[0] = 0; args[1] = 1; args[2] = result; args[3] = result + 4;
    args[4] = result + 8; args[5] = result + 12; args[6] = 1;
    expect(!method(original, 11, 7, args) && MEM32(result) == samples + (32 + 1000) % 128,
           "lock from write cursor also respects the native queue reservation");
    test_audio_write_lead(buf_of(original)->voice, 0);
    args[0] = result;
    args[1] = 0;
    expect(!method(duplicate, 4, 2, args) && MEM32(result) == 0, "duplicate cursor is independent");
    expect(!method(original, 18, 0, args), "stop guest voice");
    args[0] = 0;
    expect(!method(duplicate, 13, 1, args), "seek duplicate");
    args[0] = args[1] = 0; args[2] = 1;
    expect(!method(duplicate, 12, 3, args), "start looping duplicate");
    args[0] = 112; args[1] = 32; args[2] = result; args[3] = result + 4;
    args[4] = result + 8; args[5] = result + 12; args[6] = 0;
    expect(!method(original, 11, 7, args), "lock across ring wrap");
    expect(MEM32(result + 4) == 16 && MEM32(result + 12) == 16, "wrapped lock returns two regions");
    uint32_t first = MEM32(result), second = MEM32(result + 8);
    memset(hle_ptr(first), 160, 16); memset(hle_ptr(second), 144, 16);
    args[0] = first; args[1] = 16; args[2] = second; args[3] = 16;
    expect(!method(original, 19, 4, args), "unlock updates every duplicate");
    args[0] = 112;
    expect(!method(duplicate, 13, 1, args), "seek to wrapped upload");
    test_audio_render((uint8_t *)output, sizeof output);
    expect(output[0] == 8192 && output[32] == 4096, "duplicate hears both wrapped regions");
    args[0] = 22050;
    expect(!method(duplicate, 17, 1, args), "set native playback frequency");
    args[0] = 0;
    expect(!method(duplicate, 17, 1, args), "restore original frequency");
    dsbuf *b = buf_of(duplicate);
    expect(b->rate == 44100, "frequency zero restores the original rate");
    args[0] = 1;
    expect(method(duplicate, 17, 1, args) == DSERR_INVALIDPARAM, "reject unsupported frequency");
    expect(!method(original, 2, 0, args), "release original without destroying duplicate samples");
    test_audio_render((uint8_t *)output, sizeof output);
    expect(output[0] == 16384, "duplicate survives original release");
    expect(!method(duplicate, 2, 0, args), "release duplicate");
    expect(!method(device, 2, 0, args), "release guest device");
    hle_free(hle_ptr(memory));
    mach_leave();
}
int main(void) {
    recomp_host_init();
    expect(hle_audio_stream_open(0, 2) == -1, "reject zero sample rate");
    expect(hle_audio_stream_open(44100, 3) == -1, "reject unsupported channel count");
    int id = hle_audio_stream_open(44100, 1);
    if (id < 0) return 1;
    gex_audio_pause(1);
    int16_t input[32], output[32];
    for (int i = 0; i < 32; ++i) input[i] = 10000;
    expect(hle_audio_stream_write(id, input, 32) == 32, "write mono frames");
    hle_audio_stream_pause(id, 1);
    test_audio_render((uint8_t *)output, sizeof output);
    expect(hle_audio_stream_queued(id) == 32, "pause preserves queued frames");
    int silent = 1;
    for (int i = 0; i < 32; ++i) if (output[i]) silent = 0;
    expect(silent, "paused stream emits silence");
    hle_audio_stream_pause(id, 0);
    test_audio_render((uint8_t *)output, sizeof output);
    expect(hle_audio_stream_queued(id) == 16, "resume advances exact number of frames");
    expect(output[0] > 9900 && output[0] == output[1], "mono duplicates to both channels");
    hle_audio_stream_gain(id, 0);
    test_audio_render((uint8_t *)output, sizeof output);
    expect(hle_audio_stream_queued(id) == 0, "mute consumes samples, unlike pause");
    expect(output[0] == 0, "mute emits silence");
    hle_audio_stream_close(id);
    id = hle_audio_stream_open(44100, 2);
    if (id < 0) return 1;
    int16_t *large = calloc((STREAM_FRAMES + 100) * 2, sizeof *large);
    if (!large) return 1;
    expect(hle_audio_stream_write(id, large, STREAM_FRAMES + 100) == STREAM_FRAMES - 1,
           "write reports partial acceptance at ring capacity");
    expect(hle_audio_stream_write(id, large, 1) == 0, "full ring applies backpressure");
    test_audio_render((uint8_t *)output, sizeof output);
    expect(hle_audio_stream_write(id, large, 16) == 16, "retry writes newly available space");
    free(large);
    hle_audio_stream_close(id);
    for (int i = 0; i < MAX_STREAMS * 2; ++i) {
        id = hle_audio_stream_open(11025, 2);
        expect(id >= 0, "closed stream slots can be reused");
        hle_audio_stream_close(id);
    }
    guest_buffers();
    gex_audio_close();
    return failures ? 1 : 0;
}
