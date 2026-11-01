#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "platform_shims.h"
#include "platform/host.h"

static void reg_flush(void) {
    uint32_t key = A32(0);
    if (!((key >= 0x80000000u && key <= 0x80000005u) ||
          hle_handle_obj(key, HLE_H_REGKEY))) RET(6, 1);
    RET(hle_reg_flush(), 1);
}

static void system_parameters(void) {
    switch (A32(0)) {
    case 0x10:
        if (!APTR(2)) RET(0, 4);
        MEM32(A32(2)) = gex_host_screensaver();
        RET(1, 4);
    case 0x11:
        gex_host_set_screensaver(A32(1) != 0);
        RET(1, 4);
    default:
        hle_fatal("SystemParametersInfoA action 0x%X is not implemented", A32(0));
    }
}

static void is_iconic(void) {
    RET((hle_window_style(A32(0)) & 0x20000000u) != 0, 1);
}

static void get_window_placement(void) {
    uint32_t *p = APTR(1);
    int l, t, r, b;
    if (!p || p[0] != 44 || !hle_window_screen_rect(A32(0), &l, &t, &r, &b)) RET(0, 2);
    memset(p + 1, 0, 40);
    p[2] = (hle_window_style(A32(0)) & 0x20000000u) ? 2 : 1;
    p[7] = l; p[8] = t; p[9] = r; p[10] = b;
    RET(1, 2);
}

static void set_window_placement(void) {
    uint32_t hwnd = A32(0);
    uint32_t *p = APTR(1);
    if (!p || p[0] != 44 || !hle_is_window(hwnd)) RET(0, 2);
    uint32_t show[] = {hwnd, p[2]};
    uint32_t move[] = {hwnd, p[7], p[8], p[9] - p[7], p[10] - p[8], 1};
    hle_call_guest(hle_resolve("MoveWindow"), 6, move);
    hle_call_guest(hle_resolve("ShowWindow"), 2, show);
    RET(1, 2);
}

static void check_radio_button(void) {
    uint32_t dlg = A32(0), first = A32(1), last = A32(2), selected = A32(3);
    if (last < first || last - first > 65535) RET(0, 4);
    for (uint32_t id = first; id <= last; ++id) {
        uint32_t control = hle_dialog_item(dlg, id);
        if (control) hle_send(control, 0xF1, id == selected, 0);
    }
    RET(1, 4);
}

static void set_map_mode(void) {
    if (A32(1) != 1) hle_fatal("Gex requested unsupported GDI mapping mode %u", A32(1));
    RET(1, 2);
}

static void gdi_flush(void) {
    RET(1, 0);
}

static void stretch_mode(void) {
    if (A32(1) != 3) hle_fatal("Gex requested unsupported stretch mode %u", A32(1));
    RET(3, 2);
}

static void create_dibitmap(void) {
    const uint8_t *info = APTR(4);
    uint32_t *pixels = NULL;
    if (!info) RET(0, 6);
    int32_t w, h;
    uint16_t bpp;
    memcpy(&w, info + 4, 4);
    memcpy(&h, info + 8, 4);
    memcpy(&bpp, info + 14, 2);
    if (w <= 0 || !h || h == INT32_MIN ||
        (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32))
        hle_fatal("invalid CreateDIBitmap dimensions or depth");
    uint32_t bitmap = hle_gdi_dib_section(info, &pixels);
    if (bitmap && (A32(2) & 4) && APTR(3)) {
        size_t pitch = ((size_t)w * bpp + 31) / 32 * 4;
        memcpy(pixels, APTR(3), pitch * (size_t)(h < 0 ? -h : h));
    }
    RET(bitmap, 6);
}

static void terminate_thread(void) {
    if (hle_thread_done(A32(0))) RET(1, 2);
    fprintf(stderr, "[gex] TerminateThread: forced cancellation of a running guest thread is unsupported\n");
    hle_set_last_error(120); /* ERROR_CALL_NOT_IMPLEMENTED */
    RET(0, 2);
}

static void property_sheet(void) {
    fprintf(stderr, "[gex] PropertySheetA: the legacy settings dialog is not implemented on this host\n");
    hle_set_last_error(120);
    RET(UINT32_MAX, 1);
}

static int wave_stream = -1;
static int16_t *wave_samples;
static int wave_frames, wave_offset, wave_channels, wave_loop;
static unsigned wave_generation;

static void stop_wave(void) {
    if (wave_stream >= 0) hle_audio_stream_close(wave_stream);
    wave_stream = -1;
    free(wave_samples);
    wave_samples = NULL;
    wave_frames = wave_offset = wave_loop = 0;
    ++wave_generation;
}

void gex_platform_pump(void) {
    if (wave_stream < 0) return;
    if (wave_samples) {
        int written = hle_audio_stream_write(wave_stream,
            wave_samples + (size_t)wave_offset * wave_channels, wave_frames - wave_offset);
        wave_offset += written;
        if (wave_offset == wave_frames) {
            if (wave_loop) wave_offset = 0;
            else {
                free(wave_samples);
                wave_samples = NULL;
            }
        }
    }
    if (!wave_samples && !hle_audio_stream_queued(wave_stream)) stop_wave();
}

static void play_sound(void) {
    uint32_t source = A32(0), flags = A32(1);
    if ((flags & 0x10) && wave_stream >= 0) RET(0, 2); /* SND_NOSTOP */
    if ((flags & 8) && !(flags & 1)) {
        fprintf(stderr, "[audio] looping sndPlaySoundA requires SND_ASYNC\n");
        RET(0, 2);
    }
    stop_wave();
    if (!source) RET(1, 2);
    gex_host_wave wave;
    if (flags & 4) {
        const uint8_t *riff = hle_ptr(source);
        uint32_t size;
        memcpy(&size, riff + 4, 4);
        if (memcmp(riff, "RIFF", 4) || size < 4 || size > 16u * 1024u * 1024u) {
            fprintf(stderr, "[audio] invalid memory WAV header or size\n");
            RET(0, 2);
        }
        if (gex_host_load_wave(NULL, riff, (size_t)size + 8, &wave)) RET(0, 2);
    } else {
        char path[1024];
        if (!hle_host_path(hle_ptr(source), path, sizeof path)) {
            fprintf(stderr, "[audio] cannot resolve WAV path\n");
            RET(0, 2);
        }
        if (gex_host_load_wave(path, NULL, 0, &wave)) RET(0, 2);
    }
    wave_stream = hle_audio_stream_open(wave.rate, wave.channels);
    if (wave_stream < 0) {
        free(wave.samples);
        fprintf(stderr, "[audio] cannot open WAV output\n");
        RET(0, 2);
    }
    wave_samples = wave.samples;
    wave_frames = wave.frames;
    wave_channels = wave.channels;
    wave_loop = (flags & 8) != 0;
    unsigned generation = wave_generation;
    gex_platform_pump();
    if (!(flags & 1)) {
        while (generation == wave_generation && wave_stream >= 0) {
            gex_platform_pump();
            hle_block_begin();
            gex_host_delay(2);
            hle_block_end();
        }
    }
    RET(1, 2);
}

static void obsolete_directdraw(void) {
    hle_fatal("obsolete DirectDraw import reached; graphics callsite rewrite is incomplete");
}

static const win32hle_shim shims[] = {
    {"DirectDrawCreate", obsolete_directdraw},
    {"RegFlushKey", reg_flush},
    {"SystemParametersInfoA", system_parameters},
    {"IsIconic", is_iconic},
    {"GetWindowPlacement", get_window_placement},
    {"SetWindowPlacement", set_window_placement},
    {"CheckRadioButton", check_radio_button},
    {"SetMapMode", set_map_mode},
    {"GdiFlush", gdi_flush},
    {"SetStretchBltMode", stretch_mode},
    {"CreateDIBitmap", create_dibitmap},
    {"sndPlaySoundA", play_sound},
    {"TerminateThread", terminate_thread},
    {"PropertySheetA", property_sheet},
    {NULL, NULL}
};

void gex_register_platform_shims(void) {
    win32hle_register(shims);
}
