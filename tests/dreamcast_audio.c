#include <stdarg.h>
#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/aica_comm.h>
#include <dc/spu.h>
#include <dc/g2bus.h>
#include "arm/aica_cmd_iface.h"
static uint32_t delayed_operation;
static int delayed_command(void *packet, uint32_t size) {
    aica_cmd_t *cmd = packet;
    aica_channel_t *ch = (aica_channel_t *)cmd->cmd_data;
    if (cmd->cmd == AICA_CMD_CHAN && delayed_operation &&
        (ch->cmd & AICA_CH_CMD_MASK) == delayed_operation)
        cmd->timestamp = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK) + 200;
    return snd_sh4_to_aica(packet, size);
}
#define snd_sh4_to_aica delayed_command
#include "../src/platform/dreamcast/audio.c"
#undef snd_sh4_to_aica

KOS_INIT_FLAGS(INIT_DEFAULT);
static int failures;
#define EXPECT(expr) do { if (!(expr)) { printf("AICA FAIL line %d: %s\n", __LINE__, #expr); ++failures; } } while (0)
void hle_fatal(const char *format, ...) {
    va_list args;
    va_start(args, format); vfprintf(stderr, format, args); va_end(args);
    abort();
}
static void check_ram(gex_native_voice *v) {
    for (int c = 0; c < v->channels; ++c) {
        for (int half = 0; half < (v->windowed ? 2 : 1); ++half) {
            uint32_t at = v->windowed ? (uint32_t)half * v->capacity / 2 : 0;
            uint64_t frame = v->windowed ? v->half_source[half] : at;
            uint8_t bytes[32] __attribute__((aligned(32)));
            spu_memread(bytes, v->ram[c] + at * (v->bits / 8), sizeof bytes);
            for (unsigned i = 0; i < sizeof bytes / (unsigned)(v->bits / 8); ++i) {
                uint64_t source = frame + i;
                uint16_t expected = 0;
                if (v->looping || source < v->frames) {
                    source %= v->frames;
                    const uint8_t *p = v->pcm + (source * v->channels + c) * (v->bits / 8);
                    expected = p[0];
                    if (v->bits == 16) expected |= (uint16_t)p[1] << 8;
                    else expected ^= 128;
                }
                uint16_t actual = bytes[i * (v->bits / 8)];
                if (v->bits == 16) actual |= (uint16_t)bytes[i * 2 + 1] << 8;
                EXPECT(actual == expected);
            }
        }
    }
}
static void advance(gex_native_voice *v, uint64_t count) {
    uint64_t target = v->played + count;
    uint64_t deadline = timer_us_gettime64() + 8000000;
    while (v->playing && v->played < target && timer_us_gettime64() < deadline) {
        thd_sleep(5);
        gex_native_audio_poll();
        if (v->playing) check_ram(v);
    }
    EXPECT(!v->playing || v->played >= target);
}
static void check_uploads(void) {
    uint8_t storage[17 * 4 + 1];
    uint8_t *pcm = storage + 1;
    for (unsigned i = 0; i < sizeof storage - 1; ++i) pcm[i] = (uint8_t)(i * 37 + 11);
    static const uint32_t lengths[] = {1, 3, 17};
    static const uint64_t sources[] = {0, 1, 16, 17, 35, UINT64_C(0x100000005)};
    for (int channels = 1; channels <= 2; ++channels) {
        for (int bits = 8; bits <= 16; bits += 8) {
            uint32_t width = (uint32_t)bits / 8;
            gex_native_voice v = {.pcm = pcm, .channels = channels, .bits = bits};
            for (int c = 0; c < channels; ++c) {
                v.ram[c] = snd_mem_malloc(96);
                EXPECT(v.ram[c] != 0);
                if (!v.ram[c]) abort();
            }
            for (unsigned l = 0; l < sizeof lengths / sizeof lengths[0]; ++l) {
                v.frames = lengths[l];
                for (v.looping = 0; v.looping <= 1; ++v.looping) {
                    for (unsigned s = 0; s < sizeof sources / sizeof sources[0]; ++s) {
                        for (uint32_t at = 0; at < 4; ++at) {
                            uint8_t before[96] __attribute__((aligned(32)));
                            uint8_t actual[96] __attribute__((aligned(32)));
                            memset(before, 0x5A, sizeof before);
                            for (int c = 0; c < channels; ++c)
                                spu_memload_sq(v.ram[c], before, sizeof before);
                            uint32_t n = at ? at : 37;
                            upload(&v, at, n, sources[s]);
                            uint32_t begin = at * width & ~3u;
                            uint32_t end = ((at + n) * width + 3u) & ~3u;
                            for (int c = 0; c < channels; ++c) {
                                spu_memread(actual, v.ram[c], sizeof actual);
                                for (uint32_t pos = 0; pos < sizeof actual; ++pos) {
                                    uint8_t expected = 0x5A;
                                    if (pos >= begin && pos < end) {
                                        int64_t frame = (int64_t)sources[s] + pos / width - at;
                                        expected = 0;
                                        if (v.looping || (frame >= 0 && (uint64_t)frame < v.frames)) {
                                            uint32_t index = (uint32_t)((frame % v.frames + v.frames) % v.frames);
                                            expected = pcm[(index * channels + c) * width + pos % width];
                                            if (bits == 8) expected ^= 128;
                                        }
                                    }
                                    EXPECT(actual[pos] == expected);
                                }
                            }
                        }
                    }
                }
            }
            for (int c = 0; c < channels; ++c) snd_mem_free(v.ram[c]);
        }
    }
}
static void check_large_uploads(void) {
    const uint32_t frames = 65535, capacity = ((frames * 2 + 31) & ~31u) + 32;
    uint8_t *pcm = malloc(frames * 4);
    uint8_t *actual = memalign(32, capacity);
    if (!pcm || !actual) abort();
    for (uint32_t i = 0; i < frames * 4; ++i) pcm[i] = (uint8_t)(i * 37 + 11);
    for (int channels = 1; channels <= 2; ++channels) {
        for (int bits = 8; bits <= 16; bits += 8) {
            uint32_t width = (uint32_t)bits / 8;
            gex_native_voice v = {.pcm = pcm, .frames = frames,
                                  .channels = channels, .bits = bits};
            for (int c = 0; c < channels; ++c) {
                v.ram[c] = snd_mem_malloc(capacity);
                if (!v.ram[c]) abort();
            }
            for (v.looping = 0; v.looping <= 1; ++v.looping) {
                for (uint32_t at = 0; at <= 3; at += 3) {
                    memset(actual, 0x5A, capacity);
                    for (int c = 0; c < channels; ++c)
                        spu_memload_sq(v.ram[c], actual, capacity);
                    uint64_t source = v.looping ? frames - 3 : 0;
                    uint32_t begin = at * width & ~3u;
                    uint32_t end = (frames * width + 3u) & ~3u;
                    upload(&v, at, frames - at, source);
                    for (int c = 0; c < channels; ++c) {
                        spu_memread(actual, v.ram[c], capacity);
                        for (uint32_t pos = 0; pos < capacity; ++pos) {
                            uint8_t expected = 0x5A;
                            if (pos >= begin && pos < end) {
                                int64_t frame = (int64_t)source + pos / width - at;
                                expected = 0;
                                if (v.looping || (frame >= 0 && (uint64_t)frame < frames)) {
                                    uint32_t index = (uint32_t)((frame % frames + frames) % frames);
                                    expected = pcm[(index * channels + c) * width + pos % width];
                                    if (bits == 8) expected ^= 128;
                                }
                            }
                            if (actual[pos] != expected) {
                                printf("AICA large upload: channels=%d bits=%d loop=%d at=%lu channel=%d byte=%lu actual=%u expected=%u\n",
                                       channels, bits, v.looping, at, c, pos,
                                       actual[pos], expected);
                                EXPECT(actual[pos] == expected);
                                for (int channel = 0; channel < channels; ++channel)
                                    snd_mem_free(v.ram[channel]);
                                free(actual);
                                free(pcm);
                                return;
                            }
                        }
                    }
                }
            }
            for (int c = 0; c < channels; ++c) snd_mem_free(v.ram[c]);
        }
    }
    free(actual);
    free(pcm);
}
static void benchmark_uploads(void) {
    uint8_t *pcm = malloc(100000 * 4);
    if (!pcm) abort();
    for (unsigned i = 0; i < 100000 * 4; ++i) pcm[i] = (uint8_t)(i * 37 + 11);
    for (int channels = 1; channels <= 2; ++channels) {
        int bits = channels == 1 ? 8 : 16;
        uint32_t frames = channels == 1 ? 22050 : 100000;
        gex_native_voice *v = gex_native_voice_create(pcm, frames, 22050, channels, bits);
        if (!v) abort();
        uint64_t at = thd_get_cpu_time(thd_get_current());
        gex_native_voice_play(v, 1);
        uint64_t start_ns = thd_get_cpu_time(thd_get_current()) - at;
        at = thd_get_cpu_time(thd_get_current());
        for (unsigned i = 0; i < 8; ++i)
            gex_native_voice_update(v, 1, frames - 2);
        uint64_t update_ns = thd_get_cpu_time(thd_get_current()) - at;
        check_ram(v);
        printf("AICA upload benchmark: channels=%d bits=%d frames=%lu start-cpu-ns=%llu eight-updates-cpu-ns=%llu\n",
               channels, bits, frames, start_ns, update_ns);
        gex_native_voice_destroy(v);
    }
    free(pcm);
}
static void check_pending_starts(void) {
    uint8_t pcm[2205];
    memset(pcm, 192, sizeof pcm);
    gex_native_voice *v = gex_native_voice_create(pcm, sizeof pcm, 22050, 1, 8);
    if (!v) abort();
    unsigned missed = 0, wraps = 0;
    uint32_t previous_head = queue_read(offsetof(aica_queue_t, head));
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        gex_native_voice_seek(v, 0);
        thd_sleep(attempt % 11 + 1);
        gex_native_voice_play(v, 0);
        uint64_t deadline = timer_us_gettime64() + 40000;
        while (v->playing && timer_us_gettime64() < deadline) {
            thd_sleep(2);
            gex_native_audio_poll();
        }
        if (!v->playing) {
            printf("AICA missed start: attempt=%u elapsed-us=%llu played=%llu\n",
                   attempt, timer_us_gettime64() - v->started, v->played);
            ++missed;
        } else {
            EXPECT(snd_is_playing((unsigned)v->channel[0]));
            EXPECT(v->played > 0);
        }
        gex_native_voice_stop(v);
        uint32_t head = queue_read(offsetof(aica_queue_t, head));
        if (head < previous_head) ++wraps;
        previous_head = head;
    }
    printf("AICA pending starts: %u/128 missed, %u queue wraps\n", missed, wraps);
    EXPECT(missed == 0 && wraps > 0);
    gex_native_voice_destroy(v);
}
static void check_deferred_commands(void) {
    const uint32_t frames = 2205;
    uint8_t pcm[2205 * 2];
    memset(pcm, 192, sizeof pcm);
    for (int channels = 1; channels <= 2; ++channels) {
        gex_native_voice *v = gex_native_voice_create(pcm, frames, 22050, channels, 8);
        if (!v) abort();
        delayed_operation = AICA_CH_CMD_START;
        gex_native_voice_play(v, 0);
        delayed_operation = 0;
        while (timer_us_gettime64() - v->started < 6000) thd_pass();
        gex_native_audio_poll();
        EXPECT(v->playing && v->played == 0);
        gex_native_voice_gain(v, 0.25f, 0.75f);
        gex_native_voice_rate(v, 22050);
        thd_sleep(60);
        gex_native_audio_poll();
        EXPECT(v->playing && v->played > 0 && v->played < frames);
        thd_sleep(80);
        gex_native_voice_destroy(v);
        v = gex_native_voice_create(pcm, frames, 22050, channels, 8);
        if (!v) abort();
        gex_native_voice_play(v, 0);
        thd_sleep(20);
        gex_native_audio_poll();
        EXPECT(v->playing);
        if (v->channel[0] >= 0) {
            int channel[2] = {v->channel[0], v->channel[1]};
            for (int c = 0; c < channels; ++c)
                EXPECT(snd_is_playing((unsigned)channel[c]));
            delayed_operation = AICA_CH_CMD_STOP;
            gex_native_voice_stop(v);
            delayed_operation = 0;
            for (int c = 0; c < channels; ++c)
                EXPECT(!snd_is_playing((unsigned)channel[c]));
        }
        thd_sleep(80);
        gex_native_voice_destroy(v);
    }
}
int main(void) {
    if (!gex_native_audio_open()) return 1;
    check_deferred_commands();
    check_pending_starts();
    check_uploads();
    check_large_uploads();
    if (failures) return 1;
    benchmark_uploads();
    uint32_t frames = 100000;
    int16_t *pcm = malloc((size_t)frames * 4);
    if (!pcm) return 1;
    for (uint32_t i = 0; i < frames; ++i) {
        pcm[i * 2] = (int16_t)(i * 13);
        pcm[i * 2 + 1] = (int16_t)(i * 29);
    }
    gex_native_voice *v = gex_native_voice_create((const uint8_t *)pcm, frames, 44100, 2, 16);
    EXPECT(v != NULL);
    gex_native_voice_play(v, 1);
    check_ram(v);
    advance(v, 125000);
    EXPECT(v->playing && v->played >= 125000);
    EXPECT(v->cursor == v->played % frames);
    gex_native_voice_stop(v);
    uint64_t played = v->played;
    uint32_t cursor = v->cursor;
    thd_sleep(30);
    EXPECT(gex_native_voice_position(v).played == played);
    gex_native_voice_play(v, 1);
    advance(v, 10000);
    EXPECT(v->played > played && v->cursor == (cursor + v->played - played) % frames);
    gex_native_voice_seek(v, 50000);
    gex_native_voice_stop(v);
    gex_native_voice_seek(v, 50000);
    gex_native_voice_play(v, 0);
    advance(v, 51000);
    EXPECT(!v->playing && v->played == 50000);
    gex_native_voice_destroy(v);
    free(pcm);
    uint8_t *mono = malloc(150000);
    if (!mono) return 1;
    for (uint32_t i = 0; i < 150000; ++i) mono[i] = (uint8_t)(i * 7);
    v = gex_native_voice_create(mono, 150000, 22050, 1, 8);
    gex_native_voice_play(v, 1);
    advance(v, 70000);
    memset(mono + 60000, 192, 40000);
    gex_native_voice_update(v, 60000, 40000);
    check_ram(v);
    gex_native_voice_gain(v, 0, 0.5f);
    gex_native_voice_rate(v, 44100);
    advance(v, 40000);
    gex_native_voice_destroy(v);
    v = gex_native_voice_create(mono, 1023, 22050, 1, 8);
    gex_native_voice_play(v, 0);
    advance(v, 2048);
    EXPECT(!v->playing && v->played == 1023);
    gex_native_voice_play(v, 0);
    advance(v, 2048);
    EXPECT(!v->playing && v->played == 2046);
    gex_native_voice_destroy(v);
    v = gex_native_voice_create(mono, 1023, 22050, 1, 8);
    gex_native_voice_play(v, 0);
    uint64_t started = v->started;
    thd_sleep(150);
    EXPECT(v->playing);
    EXPECT(!snd_is_playing((unsigned)v->channel[0]));
    gex_native_voice_play(v, 0);
    EXPECT(v->playing && v->started > started);
    advance(v, 2048);
    EXPECT(!v->playing && v->played == 2046);
    gex_native_voice_destroy(v);
    free(mono);
    gex_native_audio_close();
    printf("AICA native voice regression: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
