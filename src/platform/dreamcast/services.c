#include <kos.h>
#include <dc/vmu_fb.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "platform/host.h"
#include "platform/filesystem.h"
#include "vmu_image.xbm"

uint32_t gex_host_ticks_ms(void) { return (uint32_t)timer_ms_gettime64(); }
void gex_host_delay(uint32_t milliseconds) { thd_sleep(milliseconds); }
int gex_host_screensaver(void) { return 0; }
void gex_host_set_screensaver(int enabled) { (void)enabled; }
int gex_host_enter_directory(const char *path) {
    if (!gex_fs_enter_directory(path)) return 0;
    fprintf(stderr, "[dreamcast] cannot enter %s: %s\n", path, strerror(errno));
    return -1;
}
int gex_host_working_directory(char *path, size_t size) {
    if (gex_fs_current_directory(path, size)) return 0;
    fprintf(stderr, "[dreamcast] cannot read current directory: %s\n", strerror(errno));
    return -1;
}
static void show_vmu_image(maple_device_t *dev) {
    vmufb_t fb;
    vmufb_clear(&fb);
    vmufb_paint_xbm(&fb, 0, 0, vmu_image_width, vmu_image_height,
                    vmu_image_bits);
    vmufb_present(&fb, dev);
}

int gex_host_skip_movies_by_default(void) { return 0; }
int gex_host_prepare_playback(void) { return 0; }
int gex_host_renderer_preference(int renderer, int explicit_renderer) {
    (void)explicit_renderer;
    return renderer;
}
void (*gex_host_game_exit_callback(void))(int code) { return NULL; }

int gex_host_settings_path(char *path, size_t size) {
    if (size) path[0] = 0;
    fprintf(stderr, "[dreamcast] settings are session-only; VMU persistence is not implemented\n");
    for (int port = 0; port < MAPLE_PORT_COUNT; ++port) {
        for (int unit = 0; unit < MAPLE_UNIT_COUNT; ++unit) {
            maple_device_t *dev = maple_enum_dev(port, unit);
            if (!dev || !(dev->info.functions & MAPLE_FUNC_LCD)) continue;
            show_vmu_image(dev);
            fprintf(stderr, "[dreamcast] VMU startup image sent\n");
            return 1;
        }
    }
    fprintf(stderr, "[dreamcast] no VMU LCD found\n");
    return 1;
}
void gex_host_install_diagnostics(void) {
    fprintf(stderr, "[dreamcast] KOS exception diagnostics enabled; use the matching ELF for GDB\n");
}
int gex_host_screenshot(const char *path, const uint8_t *pixels,
                        int width, int height, int pitch) {
    (void)path; (void)pixels; (void)width; (void)height; (void)pitch;
    fprintf(stderr, "[dreamcast] capture the presented framebuffer with Flycast\n");
    return -1;
}

static uint32_t little32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static unsigned little16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }

int gex_host_load_wave(const char *path, const void *data, size_t bytes,
                       gex_host_wave *wave) {
    uint8_t *owned = NULL;
    if (!!path == !!data) goto invalid;
    if (path) {
        FILE *file = fopen(path, "rb");
        if (!file) {
            fprintf(stderr, "[dreamcast] cannot open WAV %s: %s\n", path, strerror(errno));
            return -1;
        }
        long length;
        if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 12 ||
            length > 4 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
            fclose(file);
            goto invalid;
        }
        bytes = (size_t)length;
        owned = malloc(bytes);
        if (!owned) { fclose(file); hle_fatal("no memory for WAV input"); }
        size_t count = fread(owned, 1, bytes, file);
        int closed = fclose(file);
        if (count != bytes || closed) { free(owned); goto invalid; }
        data = owned;
    }
    const uint8_t *source = data, *pcm = NULL;
    unsigned channels = 0, bits = 0, rate = 0;
    size_t pcm_bytes = 0;
    if (bytes < 12 || memcmp(source, "RIFF", 4) || memcmp(source + 8, "WAVE", 4))
        goto failed;
    size_t end = (size_t)little32(source + 4);
    if (end > bytes - 8 || end < 4) goto failed;
    end += 8;
    for (size_t at = 12; at + 8 <= end;) {
        uint32_t length = little32(source + at + 4);
        if (length > end - at - 8) goto failed;
        const uint8_t *chunk = source + at + 8;
        if (!memcmp(source + at, "fmt ", 4)) {
            if (length < 16 || little16(chunk) != 1) goto failed;
            channels = little16(chunk + 2);
            rate = little32(chunk + 4);
            bits = little16(chunk + 14);
        } else if (!memcmp(source + at, "data", 4)) {
            pcm = chunk;
            pcm_bytes = length;
        }
        at += 8 + length + (length & 1);
    }
    if (!pcm || !pcm_bytes || (channels != 1 && channels != 2) ||
        (bits != 8 && bits != 16) || rate == 0 || rate > 192000 ||
        pcm_bytes % (channels * (bits / 8))) goto failed;
    size_t samples = pcm_bytes / (bits / 8);
    if (samples > INT32_MAX / 2) goto failed;
    int16_t *output = malloc(samples * sizeof *output);
    if (!output) hle_fatal("no memory for WAV conversion");
    for (size_t i = 0; i < samples; ++i)
        output[i] = bits == 8 ? (int16_t)(((int)pcm[i] - 128) * 256) :
                               (int16_t)little16(pcm + i * 2);
    *wave = (gex_host_wave){output, (int)(samples / channels), (int)channels, (int)rate};
    free(owned);
    return 0;
failed:
    free(owned);
invalid:
    fprintf(stderr, "[dreamcast] invalid or unsupported PCM WAV (requires mono/stereo 8/16-bit)\n");
    return -1;
}
