#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "win32hle.h"
#include "platform/host.h"
#include "platform/filesystem.h"

uint32_t gex_host_ticks_ms(void) { return SDL_GetTicks(); }
void gex_host_delay(uint32_t milliseconds) { SDL_Delay(milliseconds); }
int gex_host_screensaver(void) { return SDL_IsScreenSaverEnabled() != SDL_FALSE; }
void gex_host_set_screensaver(int enabled) {
    if (enabled) SDL_EnableScreenSaver();
    else SDL_DisableScreenSaver();
}

int gex_host_enter_directory(const char *path) {
    if (!gex_fs_enter_directory(path)) return 0;
    fprintf(stderr, "[gex] cannot enter %s: %s\n", path, strerror(errno));
    return -1;
}

int gex_host_working_directory(char *path, size_t size) {
    if (gex_fs_current_directory(path, size)) return 0;
    fprintf(stderr, "[gex] cannot resolve working directory: %s\n", strerror(errno));
    return -1;
}

int gex_host_skip_movies_by_default(void) {
#ifdef __EMSCRIPTEN__
    return 1;
#else
    return 0;
#endif
}

int gex_host_settings_path(char *path, size_t size) {
#if defined(__EMSCRIPTEN__)
    const char *directory = "/save";
    if (gex_directory_create(directory) && gex_fs_error() != GEX_IO_EXISTS) {
        fprintf(stderr, "[gex] cannot create browser session settings directory: %s\n",
                gex_fs_error_text());
        return -1;
    }
    int length = snprintf(path, size, "%s/settings.reg", directory);
    if (length < 0 || (size_t)length >= size) {
        fprintf(stderr, "[gex] browser session settings path is too long\n");
        return -1;
    }
    fprintf(stderr, "[gex] browser settings are session-only\n");
    return 1;
#else
#if defined(__APPLE__) || defined(__ANDROID__)
    const char *root = getenv("XDG_DATA_HOME");
    if (root && *root) {
        int length = snprintf(path, size, "%s/gex-recomp/Gex/settings.reg", root);
        if (length < 0 || (size_t)length >= size) {
            fprintf(stderr, "[gex] settings path is too long\n");
            return -1;
        }
        for (char *separator = path + 1; *separator; ++separator) {
            if (*separator != '/') continue;
            *separator = '\0';
            int failed = gex_directory_create(path) && gex_fs_error() != GEX_IO_EXISTS;
            if (failed)
                fprintf(stderr, "[gex] cannot create settings directory %s: %s\n",
                        path, gex_fs_error_text());
            *separator = '/';
            if (failed) return -1;
        }
        return 0;
    }
#endif
    char *preferences = SDL_GetPrefPath("gex-recomp", "Gex");
    if (!preferences) {
        fprintf(stderr, "[gex] cannot locate writable settings directory: %s\n", SDL_GetError());
        return -1;
    }
    int length = snprintf(path, size, "%ssettings.reg", preferences);
    SDL_free(preferences);
    if (length < 0 || (size_t)length >= size) {
        fprintf(stderr, "[gex] settings path is too long\n");
        return -1;
    }
    return 0;
#endif
}

static void crash(int sig) {
    fprintf(stderr, "\n[gex] signal %d in guest function 0x%08X, import %s\n",
            sig, g_cur_func, g_cur_import ? g_cur_import : "(none)");
    for (unsigned i = 0; i < 16 && i < g_icall_trace_idx; ++i) {
        unsigned slot = (g_icall_trace_idx - 1 - i) & (ICALL_TRACE_SIZE - 1);
        fprintf(stderr, "[gex] 0x%08X -> 0x%08X\n", g_icall_from[slot], g_icall_trace[slot]);
    }
    _Exit(128 + sig);
}

void gex_host_install_diagnostics(void) {
    signal(SIGSEGV, crash);
    signal(SIGABRT, crash);
    signal(SIGFPE, crash);
}

int gex_host_screenshot(const char *path, const uint8_t *pixels,
                        int width, int height, int pitch) {
    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom((void *)pixels, width,
        height, 16, pitch, 0xF800, 0x7E0, 0x1F, 0);
    int failed = !surface || SDL_SaveBMP(surface, path);
    SDL_FreeSurface(surface);
    if (failed) fprintf(stderr, "[gex] cannot save screenshot %s: %s\n", path, SDL_GetError());
    return failed ? -1 : 0;
}

int gex_host_load_wave(const char *path, const void *data, size_t bytes,
                       gex_host_wave *wave) {
    SDL_AudioSpec spec;
    Uint8 *samples = NULL;
    Uint32 length = 0;
    if (!!path == !!data || (data && bytes > INT32_MAX)) {
        fprintf(stderr, "[audio] invalid WAV source\n");
        return -1;
    }
    SDL_RWops *rw = path ? SDL_RWFromFile(path, "rb") : SDL_RWFromConstMem(data, (int)bytes);
    if (!rw || !SDL_LoadWAV_RW(rw, 1, &spec, &samples, &length)) {
        fprintf(stderr, "[audio] cannot load WAV: %s\n", SDL_GetError());
        return -1;
    }
    unsigned bits = SDL_AUDIO_BITSIZE(spec.format);
    if ((spec.channels != 1 && spec.channels != 2) || spec.freq <= 0 ||
        !bits || bits % 8 || !length || length > INT32_MAX ||
        length % ((bits / 8) * spec.channels)) {
        fprintf(stderr, "[audio] unsupported WAV format or length\n");
        SDL_FreeWAV(samples);
        return -1;
    }
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                         AUDIO_S16SYS, spec.channels, spec.freq) < 0) {
        SDL_FreeWAV(samples);
        fprintf(stderr, "[audio] cannot convert WAV: %s\n", SDL_GetError());
        return -1;
    }
    if (cvt.len_mult <= 0 || length > INT32_MAX / cvt.len_mult) {
        SDL_FreeWAV(samples);
        fprintf(stderr, "[audio] WAV conversion buffer exceeds supported size\n");
        return -1;
    }
    cvt.len = (int)length;
    cvt.buf = malloc(length * (size_t)cvt.len_mult);
    if (!cvt.buf) hle_fatal("cannot allocate WAV conversion buffer");
    memcpy(cvt.buf, samples, length);
    SDL_FreeWAV(samples);
    if (SDL_ConvertAudio(&cvt) < 0) {
        free(cvt.buf);
        fprintf(stderr, "[audio] WAV conversion failed: %s\n", SDL_GetError());
        return -1;
    }
    *wave = (gex_host_wave){(int16_t *)cvt.buf,
        cvt.len_cvt / (2 * spec.channels), spec.channels, spec.freq};
    return 0;
}
