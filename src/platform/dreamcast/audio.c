#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/aica_comm.h>
#include <dc/spu.h>
#include <dc/g2bus.h>
#include "arm/aica_cmd_iface.h"
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include "platform/audio_backend.h"
#include "win32hle.h"

#define WINDOW 32768u
#define TRANSFER_BYTES 16384u
#define COMMAND_TIMEOUT_US 100000u
struct gex_native_voice {
    struct gex_native_voice *next;
    const uint8_t *pcm;
    uint32_t frames, capacity, origin, last, cursor;
    uint32_t ram[2];
    uint32_t command_tail, command_bytes;
    int channel[2], channels, bits, rate, playing, looping, windowed, half;
    uint64_t played, epoch, started, half_source[2];
    float left, right;
};
static gex_native_voice *voices;
static uint8_t *transfer;
static uint32_t command_queue_size;
static int logged_voice, logged_cursor;
static uint32_t queue_read(size_t field) {
    return g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CMD_QUEUE + field);
}
static int command_consumed(gex_native_voice *v) {
    if (!v->command_bytes) return 1;
    uint32_t tail = queue_read(offsetof(aica_queue_t, tail));
    uint32_t consumed = (tail + command_queue_size - v->command_tail) % command_queue_size;
    v->command_tail = tail;
    v->command_bytes = consumed >= v->command_bytes ? 0 : v->command_bytes - consumed;
    return !v->command_bytes;
}
static void command(gex_native_voice *v, uint32_t operation) {
    AICA_CMDSTR_CHANNEL(packet, cmd, ch);
    memset(packet, 0, sizeof packet);
    int sync = v->channel[0] < 32 && (v->channels == 1 || v->channel[1] < 32);
    snd_sh4_to_aica_stop();
    /* Refresh fences on submission too, before later traffic can lap the queue. */
    for (gex_native_voice *pending = voices; pending; pending = pending->next)
        command_consumed(pending);
    int tracked = operation == AICA_CH_CMD_START || operation == AICA_CH_CMD_STOP;
    if (tracked) v->command_tail = queue_read(offsetof(aica_queue_t, tail));
    for (int c = 0; c < v->channels; ++c) {
        cmd->cmd = AICA_CMD_CHAN; cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
        cmd->cmd_id = v->channel[c];
        ch->cmd = operation;
        if (operation == AICA_CH_CMD_START && sync) ch->cmd |= AICA_CH_START_DELAY;
        ch->base = v->ram[c]; ch->type = v->bits == 8 ? AICA_SM_8BIT : AICA_SM_16BIT;
        ch->length = v->capacity;
        ch->loop = v->windowed || v->looping;
        ch->loopend = v->capacity;
        ch->freq = v->rate;
        float volume;
        if (v->channels == 2) {
            volume = c ? v->right : v->left;
            ch->pan = c ? 255 : 0;
        } else {
            volume = v->left > v->right ? v->left : v->right;
            ch->pan = v->left > v->right ?
                (uint32_t)(128 * v->right / (v->left ? v->left : 1)) :
                (uint32_t)(255 - 127 * v->left / (v->right ? v->right : 1));
        }
        ch->vol = volume <= 0 ? 0 : volume >= 1 ? 255 : (uint32_t)(volume * 255);
        snd_sh4_to_aica(packet, cmd->size);
    }
    if (operation == AICA_CH_CMD_START && sync) {
        ch->cmd = AICA_CH_CMD_START | AICA_CH_START_SYNC;
        cmd->cmd_id = 0;
        for (int c = 0; c < v->channels; ++c) {
            uint64_t mask = 1ULL << v->channel[c];
            cmd->cmd_id |= (uint32_t)mask;
        }
        snd_sh4_to_aica(packet, cmd->size);
    }
    if (tracked) {
        uint32_t head = queue_read(offsetof(aica_queue_t, head));
        v->command_bytes = (head + command_queue_size - v->command_tail) % command_queue_size;
    }
    snd_sh4_to_aica_start();
}
/* AICA performs mixing, gain and rate conversion. G2 copies need four-byte
 * alignment; store-queue destinations need 32-byte alignment. */
static void upload_chunk(gex_native_voice *v, uint32_t at, uint32_t n, uint64_t source) {
    uint32_t width = (uint32_t)v->bits / 8;
    uint32_t begin = at * width & ~3u;
    uint32_t end = ((at + n) * width + 3u) & ~3u;
    uint32_t count = (end - begin) / width;
    int64_t first = (int64_t)source + begin / width - at;
    uint32_t index = 0, leading = 0;
    /* Include the neighbouring samples covered by the rounded G2 transfer.
     * Normalize once, then split at source wraps instead of dividing per sample. */
    if (v->looping) {
        first %= v->frames;
        if (first < 0) first += v->frames;
        index = (uint32_t)first;
    } else if (first < 0) {
        leading = (uint32_t)(-first);
        if (leading > count) leading = count;
    } else if ((uint64_t)first >= v->frames) {
        leading = count;
    } else index = (uint32_t)first;
    for (int c = 0; c < v->channels; ++c) {
        uint32_t frame = index, pos = leading;
        memset(transfer, 0, leading * width);
        while (pos < count) {
            uint32_t take = v->frames - frame;
            if (take > count - pos) take = count - pos;
            const uint8_t *p = v->pcm + ((size_t)frame * v->channels + c) * width;
            uint8_t *out = transfer + pos * width;
            if (width == 2 && v->channels == 1) {
                memcpy(out, p, take * 2);
            } else if (width == 1) {
                /* AICA PCM8 is signed; keep both source strides constant. */
                if (v->channels == 1) {
                    for (uint32_t i = 0; i < take; ++i) out[i] = p[i] ^ 128;
                } else {
                    for (uint32_t i = 0; i < take; ++i) out[i] = p[i * 2] ^ 128;
                }
            } else {
                for (uint32_t i = 0; i < take; ++i) {
                    out[i * 2] = p[i * 4];
                    out[i * 2 + 1] = p[i * 4 + 1];
                }
            }
            pos += take;
            if (!v->looping) {
                memset(transfer + pos * width, 0, (count - pos) * width);
                break;
            }
            frame = 0;
        }
        uint32_t prefix = (32 - ((v->ram[c] + begin) & 31u)) & 31u;
        if (prefix > end - begin) prefix = end - begin;
        if (prefix) spu_memload(v->ram[c] + begin, transfer, prefix);
        if (end - begin > prefix)
            spu_memload_sq(v->ram[c] + begin + prefix, transfer + prefix,
                           end - begin - prefix);
    }
}
static void upload(gex_native_voice *v, uint32_t at, uint32_t n, uint64_t source) {
    uint32_t width = (uint32_t)v->bits / 8;
    while (n) {
        uint32_t count = (TRANSFER_BYTES - (at * width & 31u)) / width;
        if (count > n) count = n;
        upload_chunk(v, at, count, source);
        at += count;
        source += count;
        n -= count;
    }
}
int gex_native_audio_open(void) {
    if (snd_init() < 0) { fprintf(stderr, "[audio] cannot initialize AICA\n"); return 0; }
    command_queue_size = queue_read(offsetof(aica_queue_t, size));
    if (command_queue_size < 3 * AICA_CMDSTR_CHANNEL_SIZE * sizeof(uint32_t))
        hle_fatal("Invalid AICA command queue size");
    transfer = memalign(32, TRANSFER_BYTES);
    if (!transfer) { fprintf(stderr, "[audio] cannot allocate AICA transfer buffer\n"); return 0; }
    fprintf(stderr, "[audio] AICA hardware voices, no SH4 sample mixer\n");
    return 1;
}
void gex_native_audio_close(void) { free(transfer); transfer = NULL; }
gex_native_voice *gex_native_voice_create(const uint8_t *pcm, uint32_t frames,
                                         int rate, int channels, int bits) {
    gex_native_voice *v = calloc(1, sizeof *v);
    if (!v) { fprintf(stderr, "[audio] cannot allocate AICA voice\n"); return NULL; }
    v->pcm = pcm; v->frames = frames; v->rate = rate; v->channels = channels; v->bits = bits;
    v->left = v->right = 1;
    v->channel[0] = v->channel[1] = -1;
    v->next = voices; voices = v;
    return v;
}
static void release(gex_native_voice *v) {
    if (v->channel[0] >= 0) {
        command(v, AICA_CH_CMD_STOP);
        /* The ARM must consume STOP before RAM or channels can be reused. */
        uint64_t deadline = timer_us_gettime64() + COMMAND_TIMEOUT_US;
        while (!command_consumed(v)) {
            if (timer_us_gettime64() >= deadline)
                hle_fatal("AICA STOP command timed out on channel %d", v->channel[0]);
            thd_sleep(1);
        }
        for (int c = 0; c < v->channels; ++c) {
            snd_sfx_chn_free(v->channel[c]); v->channel[c] = -1;
            snd_mem_free(v->ram[c]); v->ram[c] = 0;
        }
    }
}
static void poll(gex_native_voice *v) {
    if (!v->playing) return;
    /* The queue tail advances only after START, including stereo sync, executes. */
    if (!command_consumed(v)) {
        if (timer_us_gettime64() - v->started >= COMMAND_TIMEOUT_US)
            hle_fatal("AICA START command timed out on channel %d", v->channel[0]);
        return;
    }
    uint32_t frame = snd_get_pos((unsigned)v->channel[0]);
    if (frame >= v->capacity) frame %= v->capacity;
    uint32_t n = (frame + v->capacity - v->last) % v->capacity;
    if (n && !logged_cursor) {
        fprintf(stderr, "[audio] AICA hardware cursor advancing (%u frames)\n", n);
        logged_cursor = 1;
    }
    v->last = frame;
    v->played += n;
    uint64_t elapsed = v->played - v->epoch;
    v->cursor = (v->origin + (uint32_t)(elapsed % v->frames)) % v->frames;
    if ((!v->looping && elapsed >= v->frames - v->origin) ||
        (!v->windowed && !v->looping && !snd_is_playing((unsigned)v->channel[0]))) {
        v->played = v->epoch + v->frames - v->origin; v->cursor = 0; v->playing = 0;
        release(v);
    } else if (v->windowed) {
        int half = frame >= v->capacity / 2;
        if (half != v->half) {
            uint32_t at = half ? 0 : v->capacity / 2;
            uint64_t next = (uint64_t)v->origin + elapsed - frame + at +
                            (half ? v->capacity : 0);
            upload(v, at, v->capacity / 2, next);
            v->half_source[at ? 1 : 0] = next;
            v->half = half;
        }
    }
}
void gex_native_audio_poll(void) { for (gex_native_voice *v = voices; v; v = v->next) poll(v); }
gex_audio_position gex_native_voice_position(gex_native_voice *v) {
    poll(v);
    return (gex_audio_position){v->cursor, v->played, v->playing, 0};
}
void gex_native_voice_stop(gex_native_voice *v) {
    poll(v); v->playing = 0; release(v);
}
void gex_native_voice_play(gex_native_voice *v, int looping) {
    poll(v);
    if (v->playing && v->looping == looping) return;
    if (v->playing) gex_native_voice_stop(v);
    v->looping = looping; v->origin = v->cursor;
    v->windowed = v->frames > 65535 || v->origin != 0;
    v->capacity = v->windowed ? WINDOW : v->frames;
    v->half = 0; v->last = 0;
    for (int c = 0; c < v->channels; ++c) {
        v->channel[c] = snd_sfx_chn_alloc();
        v->ram[c] = snd_mem_malloc((v->capacity * (v->bits / 8) + 31) & ~31u);
        if (v->channel[c] < 0 || !v->ram[c])
            hle_fatal("AICA voice resources exhausted (%u frames, %d channels)", v->frames, v->channels);
    }
    v->epoch = v->played;
    v->half_source[0] = v->origin;
    v->half_source[1] = v->origin + v->capacity / 2;
    upload(v, 0, v->capacity, v->origin);
    command(v, AICA_CH_CMD_START);
    v->playing = 1; v->started = timer_us_gettime64();
    if (!logged_voice) {
        fprintf(stderr, "[audio] AICA voice: %d Hz, %d-bit, %d channel(s), %u frames\n",
                v->rate, v->bits, v->channels, v->frames);
        logged_voice = 1;
    }
}
void gex_native_voice_seek(gex_native_voice *v, uint32_t frame) {
    int playing = v->playing;
    gex_native_voice_stop(v);
    v->cursor = frame; v->played = 0;
    if (playing) gex_native_voice_play(v, v->looping);
}
void gex_native_voice_destroy(gex_native_voice *v) {
    gex_native_voice_stop(v);
    gex_native_voice **link = &voices;
    while (*link != v) link = &(*link)->next;
    *link = v->next; free(v);
}
void gex_native_voice_update(gex_native_voice *v, uint32_t frame, uint32_t frames) {
    if (!v->playing || !frames) return;
    if (!v->windowed) upload(v, frame, frames, frame);
    else {
        for (int half = 0; half < 2; ++half) {
          uint64_t first = v->half_source[half];
          uint32_t size = v->capacity / 2;
          for (uint32_t at = 0; at < size;) {
            uint32_t source = (uint32_t)((first + at) % v->frames);
            uint32_t n = v->frames - source;
            if (n > size - at) n = size - at;
            uint32_t begin = source > frame ? source : frame;
            uint32_t end = source + n < frame + frames ? source + n : frame + frames;
            if (begin < end) upload(v, (uint32_t)half * size + at + begin - source,
                                   end - begin, first + at + begin - source);
            at += n;
          }
        }
    }
}
void gex_native_voice_gain(gex_native_voice *v, float left, float right) {
    v->left = left; v->right = right;
    if (v->playing) command(v, AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_VOL | AICA_CH_UPDATE_SET_PAN);
}
void gex_native_voice_rate(gex_native_voice *v, int rate) {
    poll(v); v->rate = rate;
    if (v->playing) command(v, AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_FREQ);
}
