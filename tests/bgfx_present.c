#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/bgfx/backend.h"
#include "packet_gpu.h"
#include "preferences.h"

int main(void) {
    char error[512] = {0};
    SDL_Window *window = NULL;
    const int hidden = getenv("GEX_TEST_BGFX_HIDDEN_WINDOW") != NULL;
    if (hidden) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) ||
            !(window = SDL_CreateWindow("hidden swapchain regression", 0, 0, 3, 2, SDL_WINDOW_HIDDEN))) {
            fprintf(stderr, "hidden SDL window: %s\n", SDL_GetError());
            return 1;
        }
    }
    if (gex_bgfx_open(window, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        gex_packet_gpu_shutdown_bgfx();
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    int status = 0;
    const unsigned colors[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0, 0x7bef};
    for (int pass = 0; pass < 4; ++pass) {
        int width = pass & 1 ? 7 : 3, height = pass & 1 ? 3 : 2;
        gex_prefs.triple = pass & 1;
        if (window) SDL_SetWindowSize(window, width, height);
        uint8_t pixels[3 * 16] = {0}, actual[7 * 3 * 4] = {0};
        gex_render_frame frame = {.pixels = pixels, .width = width, .height = height,
                                  .pitch = (width * 2 + 3) & ~3};
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                unsigned value = colors[(y * width + x) % 6];
                pixels[y * frame.pitch + x * 2] = (uint8_t)value;
                pixels[y * frame.pitch + x * 2 + 1] = (uint8_t)(value >> 8);
            }
        if (gex_bgfx_configure(&frame, error, sizeof error)) {
            fprintf(stderr, "%s\n", error); status = 1; break;
        }
        gex_render_frame invalid = frame;
        invalid.pitch = width * 2 - 1;
        if (!gex_bgfx_configure(&invalid, error, sizeof error) || !error[0]) {
            fprintf(stderr, "bgfx accepted an invalid presentation pitch\n"); status = 1; break;
        }
        if (gex_bgfx_present(&frame, width, height, error, sizeof error)) {
            fprintf(stderr, "%s\n", error); status = 1; break;
        }
        const gex_packet_gpu *gpu = gex_packet_gpu_bgfx();
        void *target = gpu->create(width, height, error, sizeof error);
        if (!target) {
            fprintf(stderr, "%s\n", error); status = 1; break;
        }
        uint16_t packets[7 * 3] = {0};
        gex_gpu_span spans[3] = {0};
        for (int y = 0; y < height; ++y)
            spans[y] = (gex_gpu_span){.y = y, .count = width, .scale = 1};
        gex_gpu_material material = {.opcode = 0x60, .rgb = 0x204080};
        int failed = gpu->upload(target, packets, error, sizeof error) ||
            gpu->draw(target, spans, height, &material, error, sizeof error) ||
            gpu->download(target, packets, error, sizeof error);
        gpu->destroy(target);
        if (failed) {
            fprintf(stderr, "%s\n", error); status = 1; break;
        }
        for (int i = 0; i < width * height; ++i)
            if (packets[i] != 0x1110) {
                fprintf(stderr, "bgfx packet/presentation interleave mismatch pass=%d pixel=%d\n", pass, i);
                status = 1; break;
            }
        if (status) break;
        if (!window && gex_bgfx_read_present(actual, error, sizeof error)) {
            fprintf(stderr, "%s\n", error); status = 1; break;
        }
        for (int i = 0; !window && i < width * height; ++i) {
            unsigned rgb = colors[i % 6];
            uint8_t expected[] = {(uint8_t)(((rgb >> 11) * 255) / 31),
                (uint8_t)((((rgb >> 5) & 63) * 255) / 63),
                (uint8_t)(((rgb & 31) * 255) / 31), 255};
            if (memcmp(expected, actual + i * 4, 4)) {
                fprintf(stderr, "bgfx presentation mismatch pass=%d pixel=%d: %u,%u,%u,%u\n",
                    pass, i, actual[i * 4], actual[i * 4 + 1], actual[i * 4 + 2], actual[i * 4 + 3]);
                status = 1;
                break;
            }
        }
        if (status) break;
        if (pass == 1) {
            gex_packet_gpu_shutdown_bgfx();
            if (gex_bgfx_open(window, error, sizeof error)) {
                fprintf(stderr, "%s\n", error); status = 1; break;
            }
        }
    }
    gex_packet_gpu_shutdown_bgfx();
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    if (!status) fprintf(stderr, hidden ?
        "[bgfx] hidden native swapchain resize, double/triple buffering and reopen PASS\n" :
        "[bgfx] real GPU presentation, packet isolation, RGB565 conversion, orientation, resize and reopen PASS\n");
    return status;
}
