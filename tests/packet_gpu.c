#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "raster.h"

static uint8_t atlas[0x100000], table[1024], packet[40], picture[320 * 224 * 2], banner[138 * 27 * 2];
static uint32_t random_state = 1;
static unsigned next(void) { random_state = random_state * 1664525u + 1013904223u; return random_state; }
static void word(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

typedef struct { gex_raster *cpu, *gpu, *copy_cpu, *copy_gpu; int width, height; } targets;

static int equal(gex_raster *cpu, gex_raster *gpu, const targets *t) {
    return !memcmp(gex_raster_pixels(cpu), gex_raster_pixels(gpu),
                   (size_t)t->width * t->height * 2);
}

#ifdef GEX_TEST_BGFX
static int batched(targets *t) {
    gex_raster_clear(t->cpu);
    gex_raster_clear(t->gpu);
    gex_raster_state state = {atlas, table, 0x90, 0, 8, 320, 232, 0};
    for (unsigned i = 0; i < 1100; ++i) {
        memset(packet, 0, sizeof packet);
        unsigned code = i % 3 ? 0x64 : 0x60;
        packet[7] = (uint8_t)(code | (i % 2 ? 2 : 0));
        packet[4] = (uint8_t)(i * 13);
        packet[5] = (uint8_t)(i * 29);
        packet[6] = (uint8_t)(i * 7);
        word(packet + 8, (int)(i * 5 % 160));
        word(packet + 10, 8 + (int)(i * 3 % 112));
        if (code == 0x64) {
            packet[12] = (uint8_t)i;
            packet[13] = (uint8_t)(i * 3);
            word(packet + 14, 0xF0000 / 32);
            word(packet + 16, 24);
            word(packet + 18, 12);
        } else {
            word(packet + 12, 24);
            word(packet + 14, 12);
            if (i % 60 == 0) {
                word(packet + 8, 0); word(packet + 10, 8);
                word(packet + 12, 320); word(packet + 14, 224);
            }
        }
        for (unsigned i = 0; i < 600; ++i) {
            memset(packet, 0, sizeof packet);
            packet[7] = 0x6a;
            packet[4] = (uint8_t)(i * 13);
            packet[5] = (uint8_t)(i * 29);
            packet[6] = (uint8_t)(i * 7);
            word(packet + 8, 11 + (int)(i % 64)); word(packet + 10, 19);
            gex_raster_packet(t->cpu, packet, &state);
            gex_raster_packet(t->gpu, packet, &state);
        }
        for (unsigned i = 0; i < 140; ++i) {
            word(packet + 8, 11);
            gex_raster_packet(t->cpu, packet, &state);
            gex_raster_packet(t->gpu, packet, &state);
        }
        for (unsigned format = 1; format <= 2; ++format) {
            state.page = format == 2 ? 0x11f : 0x80;
            unsigned address = format == 2 ? (0x80000 + 15 * 128 + 255 * 2048 + 510) & 0xfffff : 8;
            if (format == 1) atlas[0] = 20;
            for (unsigned pass = 0; pass < 2; ++pass) {
                word(atlas + address, pass ? 0x801f : 0x83e0);
                memset(packet, 0, sizeof packet);
                packet[7] = 0x64;
                packet[4] = packet[5] = packet[6] = 128;
                packet[12] = packet[13] = format == 2 ? 255 : 0;
                word(packet + 8, 200 + (int)(format * 4 + pass));
                word(packet + 10, 28);
                word(packet + 14, format == 1 ? 32767 : 0);
                word(packet + 16, 1); word(packet + 18, 1);
                gex_raster_packet(t->cpu, packet, &state);
                gex_raster_packet(t->gpu, packet, &state);
            }
        }
        if (i == 530) {
            atlas[0xF0002] ^= 0x1f;
            table[31 * 32 + 17] ^= 7;
        }
        gex_raster_packet(t->cpu, packet, &state);
        gex_raster_packet(t->gpu, packet, &state);
        if (i % 137 == 136) {
            gex_raster_packet(t->copy_cpu, packet, &state);
            gex_raster_packet(t->copy_gpu, packet, &state);
        }
    }
    if (!equal(t->cpu, t->gpu, t) || !equal(t->copy_cpu, t->copy_gpu, t)) {
        fprintf(stderr, "ordered batch mismatch at %dx%d\n", t->width, t->height);
        return 1;
    }
    return 0;
}
#endif

static int differential(void *data) {
    targets *t = data;
    gex_raster *cpu = t->cpu, *gpu = t->gpu;
    for (size_t i = 0; i < sizeof atlas; ++i) atlas[i] = (uint8_t)(next() >> 24);
    for (unsigned rgb = 0; rgb < 32; ++rgb)
        for (unsigned c = 0; c < 32; ++c) {
            unsigned value = (rgb * c) >> 4;
            table[rgb * 32 + c] = (uint8_t)(value > 31 ? 31 : value);
        }
    gex_raster_state state = {atlas, table, 0, 0, 8, 320, 232, 0};
    for (int iteration = 0; iteration < 648; ++iteration) {
        memset(packet, 0, sizeof packet);
        unsigned codes[] = {0x2c, 0x64, 0x60, 0x68, 0x70, 0x78};
        unsigned code = codes[iteration % 6];
        int epoch = iteration / 6;
        packet[7] = (uint8_t)(code | (epoch % 2 ? 2 : 0));
        packet[4] = epoch / 18 % 3 ? 128 : 80;
        packet[5] = epoch / 18 % 3 ? 128 : 192;
        packet[6] = epoch / 18 % 3 ? 128 : 48;
        int x = (int)(next() % 480) - 80, y = (int)(next() % 340) - 40;
        int w = (int)(next() % 100) + 1, h = (int)(next() % 80) + 1;
        state.page = (next() % 16) | ((epoch / 2 % 3) << 7) | (epoch % 2 ? 16 : 0);
        state.layout = epoch / 6 % 3;
        state.pixel_scale = iteration % 17 == 0 ? 2 : 0;
        state.offset_x = state.pixel_scale ? 19 : 0;
        state.world_clip = iteration % 5 == 0;
        state.world_left = 25; state.world_right = 510;
        state.world_top = 15; state.world_bottom = 360;
        word(packet + 8, x); word(packet + 10, y);
        if (code == 0x2c) {
            for (int i = 0; i < 4; ++i) {
                word(packet + 8 + i * 8, x + ((i & 1) ? w : 0) + (iteration % 4 == 0 && (i & 2) ? 7 : 0));
                word(packet + 10 + i * 8, y + ((i & 2) ? h : 0));
                packet[12 + i * 8] = (uint8_t)((i & 1) ? 160 : 12);
                packet[13 + i * 8] = (uint8_t)((i & 2) ? 100 : 4);
            }
            word(packet + 22, (int)state.page);
            if (iteration % 12 == 6) {
                word(packet + 8, x + w); word(packet + 24, x + w);
                word(packet + 16, x); word(packet + 32, x);
            }
        } else if (code == 0x64) {
            packet[12] = (uint8_t)(next() % 256); packet[13] = (uint8_t)(next() % 256);
            word(packet + 16, w); word(packet + 18, h);
        } else {
            word(packet + 12, w); word(packet + 14, h);
        }
        if (code == 0x2c || code == 0x64) word(packet + 14, (int)(next() & 32767));
        if (iteration % 23 == 0) {
            gex_raster_clear(cpu); gex_raster_clear(gpu);
        }
        /* Include texture reuse and CLUT mutation while GPU targets persist. */
        atlas[next() % sizeof atlas] ^= 0xff;
        if (iteration % 67 == 0) {
            const uint8_t *pixels = gex_raster_pixels(cpu);
            for (int row = 0; row < 224; ++row)
                memcpy(picture + row * 640, pixels + row * t->width * 2, 640);
            gex_raster_remember(cpu, picture, 640);
            gex_raster_remember(gpu, picture, 640);
        }
        gex_raster_packet(cpu, packet, &state);
        gex_raster_packet(gpu, packet, &state);
        if (iteration % 67 == 0) {
            word(picture + 17 * 640 + 29 * 2, (int)(next() & 32767));
            gex_raster_sync(cpu, picture, 640);
            gex_raster_sync(gpu, picture, 640);
        }
        const uint8_t *reference = gex_raster_pixels(cpu), *actual = gex_raster_pixels(gpu);
        if (memcmp(reference, actual, (size_t)t->width * t->height * 2)) {
            for (int i = 0; i < t->width * t->height * 2; i += 2)
                if (memcmp(reference + i, actual + i, 2)) {
                    fprintf(stderr, "packet %d opcode %02x page %x layout %d at %d,%d CPU=%02x%02x GPU=%02x%02x\n",
                            iteration, packet[7], state.page, state.layout, (i / 2) % t->width, (i / 2) / t->width,
                            reference[i + 1], reference[i], actual[i + 1], actual[i]);
                    break;
                }
            return 1;
        }
        if (iteration % 101 == 0) {
            gex_raster_copy(t->copy_cpu, cpu); gex_raster_copy(t->copy_gpu, gpu);
            if (!equal(t->copy_cpu, t->copy_gpu, t)) return 1;
            gex_raster_pause(t->copy_cpu, cpu, banner);
            gex_raster_pause(t->copy_gpu, gpu, banner);
            if (!equal(t->copy_cpu, t->copy_gpu, t)) return 1;
            gex_raster_packet(t->copy_cpu, packet, &state);
            gex_raster_packet(t->copy_gpu, packet, &state);
            if (!equal(t->copy_cpu, t->copy_gpu, t)) return 1;
        }
    }
#ifdef GEX_TEST_BGFX
    return batched(t);
#else
    return 0;
#endif
}

int main(void) {
#ifdef GEX_TEST_BGFX
    if (gex_packet_gpu_select_bgfx()) return 1;
#else
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[packet-gpu] headless GL prerequisite unavailable: %s\n", SDL_GetError());
        return 77;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *window = SDL_CreateWindow("GPU test", 0, 0, 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
    if (!context) {
        fprintf(stderr, "[packet-gpu] headless GL 4.3 prerequisite unavailable: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return 77;
    }
#endif
    const int sizes[][2] = {{320, 240}, {427, 240}, {640, 448}, {854, 480}};
    int status = 0;
    for (unsigned size = 0; size < sizeof sizes / sizeof *sizes; ++size) {
        int width = sizes[size][0], height = sizes[size][1];
        targets t = {gex_raster_create(width, height), gex_raster_create(width, height),
                     gex_raster_create(width, height), gex_raster_create(width, height),
                     width, height};
        int result = 1;
        if (t.cpu && t.gpu && t.copy_cpu && t.copy_gpu &&
            !gex_raster_enable_gpu(t.gpu, gex_packet_gpu_get()) &&
            !gex_raster_enable_gpu(t.copy_gpu, gex_packet_gpu_get())
#ifndef GEX_TEST_BGFX
            && SDL_GL_GetCurrentContext() == context
#endif
            ) {
            SDL_Thread *thread = SDL_CreateThread(differential, "packet-test", &t);
            if (thread) {
                SDL_WaitThread(thread, &result);
                if (!equal(t.cpu, t.gpu, &t)) result = 1;
#ifndef GEX_TEST_BGFX
                if (SDL_GL_GetCurrentContext() != context) result = 1;
#endif
                gex_raster_disable_gpu(t.gpu);
                if (!equal(t.cpu, t.gpu, &t) || gex_raster_enable_gpu(t.gpu, gex_packet_gpu_get())) result = 1;
            } else fprintf(stderr, "[packet-gpu] cannot start worker: %s\n", SDL_GetError());
        }
        gex_raster_destroy(t.copy_gpu); gex_raster_destroy(t.copy_cpu);
        gex_raster_destroy(t.gpu); gex_raster_destroy(t.cpu);
        if (result) { status = result; break; }
    }
#ifdef GEX_TEST_BGFX
    gex_packet_gpu_shutdown_bgfx();
#else
    if (SDL_GL_GetCurrentContext() != context) status = 1;
    SDL_GL_DeleteContext(context); SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif
    if (!status) {
#ifdef GEX_TEST_BGFX
        fprintf(stderr, "[packet-gpu] 2592 ordered packets plus 7360 batched packets at four sizes, worker migration, copy/pause and CPU edits PASS\n");
#else
        fprintf(stderr, "[packet-gpu] 2592 ordered packets at four sizes, worker migration, copy/pause and CPU edits PASS\n");
#endif
    }
    return status;
}
