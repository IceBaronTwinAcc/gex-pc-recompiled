#include <kos.h>
#include <arch/mmu.h>
#include <dc/pvr.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "packet_gpu.h"
#include "raster.h"

KOS_INIT_FLAGS(INIT_DEFAULT);
static uint8_t atlas[0x100000] __attribute__((aligned(65536)));
static uint8_t table[1024];
static uint16_t pixels[320 * 240], other[320 * 240];
static int failures;
static void *raster_device;
static void *remember_device(int width, int height, char *error, size_t size) {
    raster_device = gex_packet_gpu_get()->create(width, height, error, size);
    return raster_device;
}
#define EXPECT(e) do { if (!(e)) { printf("PVR FAIL line %d: %s\n", __LINE__, #e); ++failures; } } while (0)
static void put(unsigned at, unsigned value) { atlas[at] = value; atlas[at + 1] = value >> 8; }
static void check(const gex_packet_gpu *gpu, void *t, gex_gpu_material *m,
                  int x, int y, int count, int u, int v) {
    char error[128] = {0};
    gex_gpu_span span = {x, y, count, 1, u * 65536.0, v * 65536.0, 65536, 0, 1, 0, {0, 0}};
    if (gpu->draw(t, &span, 1, m, error, sizeof error) ||
        gpu->download(t, pixels, error, sizeof error)) {
        printf("PVR operation failed: %s\n", error);
        ++failures;
    }
}

static void shared_palette_regression(const gex_packet_gpu *gpu, int rectangular_reset) {
    const gex_gpu_native_ops *native = gpu->native;
    char error[256] = {0};
    void *t = gpu->create(320, 240, error, sizeof error);
    void *copy = gpu->create(320, 240, error, sizeof error);
    if (!t || !copy) {
        printf("PVR shared palette targets failed: %s\n", error);
        ++failures;
        if (copy) gpu->destroy(copy);
        if (t) gpu->destroy(t);
        return;
    }
    native->bind(t, atlas, 0);
    native->atlas_write(atlas, 0, sizeof atlas);
    memset(atlas, 0, sizeof atlas);
    for (unsigned y = 0; y < 8; ++y) atlas[y * 2048 + 512] = 0x21;
    for (unsigned i = 0; i < 288; ++i)
        for (unsigned j = 0; j < 16; ++j) {
            unsigned c = (i * 17 + j) & 255;
            put(0xc0000 + i * 32 + j * 2,
                !j ? 0 : j == 2 ? 0x801f : 1 + c % 31 + (c / 31) * 32);
        }
    gex_gpu_material material = {4, 0x6000, 0x2c, 0x808080, atlas, table};
    const float invalid_values[] = {NAN, INFINITY, -INFINITY};
    const size_t coordinates[] = {offsetof(gex_gpu_vertex, x), offsetof(gex_gpu_vertex, y),
                                  offsetof(gex_gpu_vertex, u), offsetof(gex_gpu_vertex, v)};
    for (unsigned format = 0; format < 3; ++format) {
        material.page = 4 | format << 7;
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 0; j < 4; ++j) {
                if (format) EXPECT(!native->clear(t, error, sizeof error));
                gex_gpu_vertex invalid[] = {{10, 10, 0, 0}, {12, 10, 2, 0},
                                           {11, 12, 0, 2}, {13, 12, 2, 2}};
                memcpy((uint8_t *)&invalid[3] + coordinates[j], &invalid_values[i], sizeof(float));
                EXPECT(native->polygon(t, invalid, 0, 0, 320, 240,
                                       &material, error, sizeof error) != 0);
                EXPECT(strstr(error, "invalid PVR polygon vertex") != NULL);
            }
    }
    material.page = 4;
    gex_raster_state state = {atlas, table, 4, 0, 8, 320, 248, 0};
    int64_t warmed_storage = 0;
    for (unsigned frame = 0; frame < 400; ++frame) {
        size_t before = pvr_mem_available();
        EXPECT(!native->clear(t, error, sizeof error));
        /* Keep this a redraw test: a retained prefix needs no texture warming. */
        gex_gpu_material marker = {.opcode = 0x60, .rgb = frame & 1 ? 1 : 2};
        gex_gpu_vertex marker_quad[] = {{300, 200, 0, 0}, {301, 200, 0, 0},
                                       {300, 201, 0, 0}, {301, 201, 0, 0}};
        EXPECT(!native->polygon(t, marker_quad, 0, 0, 320, 240, &marker, error, sizeof error));
        if (frame >= 4) {
            size_t after = pvr_mem_available();
            EXPECT((before >= after ? before - after : after - before) <= 4 * (128 + 32));
        }
        uint32_t first_group[16];
        for (unsigned i = 0; i < 288; ++i) {
            uint8_t packet[40] = {0};
            packet[4] = packet[5] = packet[6] = frame == 299 ? 64 : 128;
            packet[7] = i & 1 ? 0x2e : 0x2c;
            packet[14] = (uint8_t)i; packet[15] = (uint8_t)(0x60 + (i >> 8));
            packet[22] = 4;
            float x = (float)(i % 48) * 3, y = 180 + i / 48;
            gex_gpu_vertex cell[] = {{x, y, 0, 0}, {x + 3, y, 3, 0},
                                    {x, y + 1, 0, 1}, {x + 3, y + 1, 3, 1}};
            for (unsigned j = 0; j < 4; ++j) {
                unsigned x = (unsigned)cell[j].x;
                packet[8 + j * 8] = (uint8_t)x;
                packet[9 + j * 8] = (uint8_t)(x >> 8);
                packet[10 + j * 8] = (uint8_t)(cell[j].y + 8);
                packet[12 + j * 8] = (uint8_t)cell[j].u;
                packet[13 + j * 8] = (uint8_t)cell[j].v;
            }
            int consumed = native->packet(t, packet, &state, 0, 0, error, sizeof error);
            EXPECT(consumed >= 0);
            material.clut = 0x6000 + i;
            material.opcode = i & 1 ? 0x2e : 0x2c;
            material.rgb = frame == 299 ? 0x404040 : 0x808080;
            if (!consumed)
                EXPECT(!native->polygon(t, cell, 0, 0, 320, 240,
                                        &material, error, sizeof error));
            if (frame == 299 && i == 1)
                for (unsigned j = 0; j < 16; ++j)
                    first_group[j] = PVR_GET(PVR_PALETTE_TABLE_BASE + j * 4);
        }
        if (frame == 299)
            for (unsigned j = 0; j < 16; ++j)
                EXPECT(PVR_GET(PVR_PALETTE_TABLE_BASE + j * 4) == first_group[j]);
        if (frame >= 4) {
            size_t after = pvr_mem_available();
            EXPECT((before >= after ? before - after : after - before) <= 4 * (128 + 32));
            warmed_storage += (int64_t)before - (int64_t)after;
        }
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        for (unsigned i = 0; i < 288; ++i) {
            unsigned c = (i * 17 + 1) & 255;
            unsigned expected = 1 + c % 31 + (c / 31) * 32;
            unsigned at = (180 + i / 48) * 320 + i % 48 * 3;
            if (frame != 299) EXPECT(pixels[at] == expected);
            else {
                EXPECT(abs((int)(pixels[at] & 31) - (int)((expected & 31) / 2)) <= 1);
                EXPECT(abs((int)(pixels[at] >> 5 & 31) -
                           (int)((expected >> 5 & 31) / 2)) <= 1);
            }
            unsigned red = frame == 299 ? 15 : 31;
            if (i & 1) red /= 2;
            EXPECT(abs((int)(pixels[at + 1] & 31) - (int)red) <= 1);
            EXPECT(pixels[at + 2] == 0);
        }
    }
    EXPECT(warmed_storage >= 236 * 96 && warmed_storage <= 256 * 160);
    /* Game initialization invalidates, clears GPU targets, then resets CPU
     * atlas data. Deferred warming must not rebuild from the old pixels. */
    if (rectangular_reset)
        native->atlas_write_rect(atlas, 0, 2048, 2048, 512);
    else
        native->atlas_write(atlas, 0, sizeof atlas);
    EXPECT(!native->clear(t, error, sizeof error));
    for (unsigned y = 0; y < 8; ++y) atlas[y * 2048 + 512] = 0;
    for (unsigned i = 0; i < 288; ++i) {
        float x = (float)(i % 48) * 3, y = 180 + i / 48;
        gex_gpu_vertex cell[] = {{x, y, 0, 0}, {x + 3, y, 3, 0},
                                {x, y + 1, 0, 1}, {x + 3, y + 1, 3, 1}};
        material.clut = 0x6000 + i;
        material.opcode = i & 1 ? 0x2e : 0x2c;
        material.rgb = 0x808080;
        EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
    }
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    for (unsigned i = 0; i < 288; ++i)
        EXPECT(pixels[(180 + i / 48) * 320 + i % 48 * 3] == 0);
    native->atlas_write(atlas, 0, sizeof atlas);
    for (unsigned y = 0; y < 8; ++y) atlas[y * 2048 + 512] = 0x21;
    material.clut = 0x6000; material.rgb = 0x808080; material.opcode = 0x2c;
    gex_gpu_vertex mirrored[] = {{110, 188, 0, 0}, {107, 188, 3, 0},
                                 {110, 189, 0, 1}, {107, 189, 3, 1}};
    EXPECT(!native->polygon(t, mirrored, 108, 0, 110, 240, &material, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    memcpy(other, pixels, sizeof pixels);
    EXPECT(!native->clear(copy, error, sizeof error));
    EXPECT(!native->polygon(copy, mirrored, 108, 0, 110, 240, &material, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    for (unsigned x = 107; x <= 110; ++x)
        EXPECT(pixels[188 * 320 + x] == other[188 * 320 + x]);
    /* A full PAL8 palette coexists with the warmed PAL4 layouts. */
    native->atlas_write(atlas, 640, 8);
    memset(atlas + 640, 1, 8);
    native->atlas_write(atlas, 0xc2000, 512);
    for (unsigned i = 0; i < 256; ++i) put(0xc2000 + i * 2, 0x7c00);
    gex_gpu_material large = {5 | 128, 0x6100, 0x2c, 0x808080, atlas, table};
    gex_gpu_vertex dot[] = {{100, 190, 0, 0}, {101, 190, 1, 0},
                           {100, 191, 0, 1}, {101, 191, 1, 1}};
    EXPECT(!native->polygon(t, dot, 0, 0, 320, 240, &large, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[190 * 320 + 100] == 0x7c00);
    material.clut = 0x6000; material.rgb = 0x808080;
    material.opcode = 0x2c;
    dot[0].x = dot[2].x = 101; dot[1].x = dot[3].x = 102;
    EXPECT(!native->polygon(t, dot, 0, 0, 320, 240, &material, error, sizeof error));
    native->atlas_write(atlas, 0xc0002, 2);
    put(0xc0002, 0x3e0);
    dot[0].x = dot[2].x = 102; dot[1].x = dot[3].x = 103;
    EXPECT(!native->polygon(t, dot, 0, 0, 320, 240, &material, error, sizeof error));
    native->atlas_write(atlas, 512, 1);
    atlas[512] = 0;
    dot[0].x = dot[2].x = 103; dot[1].x = dot[3].x = 104;
    EXPECT(!native->polygon(t, dot, 0, 0, 320, 240, &material, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[190 * 320 + 101] == 2);
    EXPECT(pixels[190 * 320 + 102] == 0x3e0);
    EXPECT(pixels[190 * 320 + 103] == 0);
    material.clut = 0x6001; material.opcode = 0x2e;
    gex_gpu_vertex row_cell[] = {{104, 190, 0, 3}, {105, 190, 1, 3},
                                {104, 191, 0, 4}, {105, 191, 1, 4}};
    for (unsigned frame = 0; frame < 4; ++frame) {
        EXPECT(!native->clear(t, error, sizeof error));
        for (unsigned i = 0; i < 65; ++i) {
            row_cell[0].x = row_cell[2].x = 104 + i;
            row_cell[1].x = row_cell[3].x = 105 + i;
            EXPECT(!native->polygon(t, row_cell, 0, 0, 320, 240, &material, error, sizeof error));
        }
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        EXPECT(pixels[190 * 320 + 104] == 19);
    }
    EXPECT(!native->clear(t, error, sizeof error));
    row_cell[0].x = row_cell[2].x = 104; row_cell[1].x = row_cell[3].x = 105;
    EXPECT(!native->polygon(t, row_cell, 0, 0, 320, 240, &material, error, sizeof error));
    native->atlas_write_rect(atlas, 3 * 2048 + 512, 2048, 1, 2);
    atlas[3 * 2048 + 512] = atlas[4 * 2048 + 512] = 0;
    row_cell[0].x = row_cell[2].x = 105; row_cell[1].x = row_cell[3].x = 106;
    EXPECT(!native->polygon(t, row_cell, 0, 0, 320, 240, &material, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[190 * 320 + 104] == 19 && pixels[190 * 320 + 105] == 0);
    native->atlas_write_rect(atlas, 3 * 2048 + 512, 2048, 1, 2);
    atlas[3 * 2048 + 512] = atlas[4 * 2048 + 512] = 0x43;
    row_cell[0].x = row_cell[2].x = 106; row_cell[1].x = row_cell[3].x = 108;
    row_cell[1].u = row_cell[3].u = 2;
    EXPECT(!native->polygon(t, row_cell, 0, 0, 320, 240, &material, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[190 * 320 + 106] == 21 && pixels[190 * 320 + 107] == 22);
    gpu->destroy(copy); gpu->destroy(t);
}

static void shared_color_reclamation_regression(const gex_packet_gpu *gpu) {
    const gex_gpu_native_ops *native = gpu->native;
    char error[256] = {0};
    void *t = gpu->create(320, 240, error, sizeof error);
    void *copy = gpu->create(320, 240, error, sizeof error);
    if (!t || !copy) {
        printf("PVR shared color reclamation targets failed: %s\n", error);
        ++failures;
        if (copy) gpu->destroy(copy);
        if (t) gpu->destroy(t);
        return;
    }
    native->bind(t, atlas, 0);
    native->atlas_write(atlas, 0, sizeof atlas);
    memset(atlas, 0, sizeof atlas);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 8; ++x)
            atlas[y * 2048 + 512 + x] = (uint8_t)(x * 2 | (x * 2 + 1) << 4);
    for (unsigned i = 0; i < 128; ++i)
        for (unsigned j = 0; j < 16; ++j)
            put(0xc0000 + i * 32 + j * 2, 1 + i * 16 + j);
    gex_gpu_material material = {4, 0x6000, 0x2e, 0x808080, atlas, table};
    /* Fill both shared groups, then replace the scene without changing its
     * atlas. Cold remaps must stop pinning the old scene's 512 colors. */
    for (unsigned frame = 0; frame < 121; ++frame) {
        unsigned palettes = frame < 40 ? 96 : 32;
        unsigned first = frame < 40 ? 0 : 96;
        EXPECT(!native->clear(t, error, sizeof error));
        gex_gpu_material marker = {.opcode = 0x60, .rgb = frame & 1 ? 1 : 2};
        gex_gpu_vertex marker_quad[] = {{300, 200, 0, 0}, {301, 200, 0, 0},
                                       {300, 201, 0, 0}, {301, 201, 0, 0}};
        EXPECT(!native->polygon(t, marker_quad, 0, 0, 320, 240, &marker, error, sizeof error));
        for (unsigned i = 0; i < palettes * 3; ++i) {
            unsigned palette = i % palettes;
            float x = palette % 20 * 16, y = 20 + palette / 20;
            gex_gpu_vertex cell[] = {{x, y, 1, 0}, {x + 16, y, 17, 0},
                                    {x, y + 1, 1, 1}, {x + 16, y + 1, 17, 1}};
            material.clut = 0x6000 + first + palette;
            EXPECT(!native->polygon(t, cell, 0, 0, 320, 240,
                                    &material, error, sizeof error));
        }
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        for (unsigned i = 0; i < palettes; ++i)
            for (unsigned j = 0; j < 16; ++j)
                EXPECT(pixels[(20 + i / 20) * 320 + i % 20 * 16 + j] ==
                       1 + (first + i) * 16 + ((j + 1) & 15));
        EXPECT(!native->present(t, error, sizeof error));
    }
    gpu->destroy(copy); gpu->destroy(t);
}

static void command_stream_regression(const gex_packet_gpu *gpu) {
    char error[256] = {0};
    const gex_gpu_native_ops *native = gpu->native;
    void *t = gpu->create(320, 240, error, sizeof error);
    void *copy = gpu->create(320, 240, error, sizeof error);
    if (!t || !copy) {
        printf("PVR command stream target failed: %s\n", error);
        ++failures;
        if (copy) gpu->destroy(copy);
        if (t) gpu->destroy(t);
        return;
    }
    native->bind(t, atlas, 0);
    gex_gpu_material material = {.opcode = 0x60};
    /* Identical headers span a command-buffer drain; colors still belong
     * to individual strips, and the next full clear retires the old image. */
    for (unsigned frame = 0; frame < 2; ++frame) {
        EXPECT(!native->clear(t, error, sizeof error));
        for (unsigned i = 0; i < 600; ++i) {
            float x = i % 300, y = 10 + i / 300;
            gex_gpu_vertex cell[] = {{x, y, 0, 0}, {x + 1, y, 0, 0},
                                    {x, y + 1, 0, 0}, {x + 1, y + 1, 0, 0}};
            material.rgb = (i + frame) & 1 ? 0x00ff00 : 0x0000ff;
            EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
        }
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        for (unsigned i = 0; i < 600; ++i)
            EXPECT(pixels[(10 + i / 300) * 320 + i % 300] ==
                   (((i + frame) & 1) ? 0x3e0 : 31));
        EXPECT(pixels[10 * 320 + 300] == 0 && pixels[12 * 320] == 0);
    }
    gpu->destroy(copy); gpu->destroy(t);
}

static void refresh_texel(const gex_gpu_native_ops *native, unsigned format,
                          unsigned y, unsigned index) {
    const unsigned colors[] = {0, 31, 0x3e0, 0x7c00};
    unsigned at = 1024 + y * 2048;
    native->atlas_write(atlas, at, format == 2 ? 2 : 1);
    if (format == 2) put(at, colors[index]);
    else atlas[at] = (uint8_t)index;
}

static void presentation_palette_regression(const gex_packet_gpu *gpu) {
    const gex_gpu_native_ops *native = gpu->native;
    char error[256] = {0};
    void *t = gpu->create(320, 240, error, sizeof error);
    void *copy = gpu->create(320, 240, error, sizeof error);
    if (!t || !copy) {
        printf("PVR presentation palette targets failed: %s\n", error);
        ++failures;
        if (copy) gpu->destroy(copy);
        if (t) gpu->destroy(t);
        return;
    }
    native->bind(t, atlas, 0);
    native->atlas_write(atlas, 0, sizeof atlas);
    memset(atlas, 0, sizeof atlas);
    memset(atlas + 1024, 0x11, 16);
    put(0xc8002, 31);
    gex_gpu_material material = {8, 0x6400, 0x2c, 0x808080, atlas, table};
    EXPECT(!native->clear(t, error, sizeof error));
    for (unsigned i = 0; i < 600; ++i) {
        float x = i % 300, y = 20 + i / 300;
        gex_gpu_vertex cell[] = {{x, y, 0, 0}, {x + 1, y, 1, 0},
                                {x, y + 1, 0, 1}, {x + 1, y + 1, 1, 1}};
        EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
    }
    EXPECT(!native->present(t, error, sizeof error));
    /* The previous RTT may still be sampling palette bank zero when present
     * returns. Reusing that bank must not recolor its retained red pixels. */
    native->atlas_write(atlas, 0xc8002, 2);
    put(0xc8002, 0x3e0);
    gex_gpu_vertex cell[] = {{10, 22, 0, 0}, {11, 22, 1, 0},
                            {10, 23, 0, 1}, {11, 23, 1, 1}};
    EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    for (unsigned i = 0; i < 600; ++i)
        EXPECT(pixels[(20 + i / 300) * 320 + i % 300] == 31);
    EXPECT(pixels[22 * 320 + 10] == 0x3e0);
    EXPECT(!native->present(t, error, sizeof error));
    gpu->destroy(copy); gpu->destroy(t);
}

static void lazy_page_regression(const gex_packet_gpu *gpu) {
    const gex_gpu_native_ops *native = gpu->native;
    for (unsigned format = 0; format < 3; ++format) {
        char error[256] = {0};
        void *t = gpu->create(320, 240, error, sizeof error);
        void *copy = gpu->create(320, 240, error, sizeof error);
        if (!t || !copy) {
            printf("PVR lazy page targets failed: %s\n", error);
            ++failures;
            if (copy) gpu->destroy(copy);
            if (t) gpu->destroy(t);
            return;
        }
        native->bind(t, atlas, 0);
        native->atlas_write(atlas, 0, sizeof atlas);
        memset(atlas, 0, sizeof atlas);
        put(0xc8000, 0x7c00); put(0xc8002, 31); put(0xc8004, 0x3e0);
        if (format == 2) put(1024 + 2048, 31);
        else atlas[1024 + 2048] = format == 0 ? 0x11 : 1;
        gex_gpu_material material = {8 | format << 7, 0x6400, 0x2c, 0x808080, atlas, table};
        EXPECT(!native->clear(t, error, sizeof error));
        const unsigned u[] = {0, 8, 8, 40}, v[] = {1, 3, 3, 101};
        for (unsigned draw = 0; draw < 4; ++draw) {
            if (draw == 3) {
                unsigned at = 1024 + v[draw] * 2048 +
                              (format == 0 ? u[draw] / 2 : format == 1 ? u[draw] : u[draw] * 2);
                native->atlas_write(atlas, at, format == 2 ? 2 : 1);
                if (format == 2) put(at, 0x3e0);
                else atlas[at] = format == 0 ? 0x22 : 2;
            }
            float x = 10 + draw * 10;
            gex_gpu_vertex cell[] = {{x, 10, u[draw], v[draw]},
                                    {x + 1, 10, u[draw] + 1, v[draw]},
                                    {x, 11, u[draw], v[draw] + 1},
                                    {x + 1, 11, u[draw] + 1, v[draw] + 1}};
            EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
        }
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        EXPECT(pixels[10 * 320 + 10] == 31);
        EXPECT(pixels[10 * 320 + 20] == (format < 2 ? 0x7c00 : 0));
        EXPECT(pixels[10 * 320 + 30] == (format < 2 ? 0x7c00 : 0));
        EXPECT(pixels[10 * 320 + 40] == 0x3e0);
        gpu->destroy(copy); gpu->destroy(t);
    }
}

static unsigned probe_color(unsigned u, unsigned v, unsigned frame) {
    unsigned index = (u * 13 + v * 7 + frame * 3) & 7;
    return (index & 1 ? 31 : 0) | (index & 2 ? 0x3e0 : 0) |
           (index & 4 ? 0x7c00 : 0);
}

static void sparse_sampling_regression(const gex_packet_gpu *gpu) {
    const gex_gpu_native_ops *native = gpu->native;
    for (unsigned format = 0; format < 3; ++format) {
        char error[256] = {0};
        void *t = gpu->create(320, 240, error, sizeof error);
        void *copy = gpu->create(320, 240, error, sizeof error);
        if (!t || !copy) {
            printf("PVR sparse sampling targets failed: %s\n", error);
            ++failures;
            if (copy) gpu->destroy(copy);
            if (t) gpu->destroy(t);
            return;
        }
        native->bind(t, atlas, 0);
        native->atlas_write(atlas, 0, sizeof atlas);
        memset(atlas, 0, sizeof atlas);
        for (unsigned i = 0; i < 256; ++i)
            put(0xc8000 + i * 2, 0x8000 | (i & 1 ? 31 : 0) |
                (i & 2 ? 0x3e0 : 0) | (i & 4 ? 0x7c00 : 0));
        gex_gpu_material m = {8 | format << 7, 0x6400, 0x2c, 0x808080, atlas, table};
        for (unsigned frame = 0; frame < 8; ++frame) {
            unsigned row = 128u << format;
            native->atlas_write_rect(atlas, 1024, 2048, row, 256);
            for (unsigned v = 0; v < 256; ++v)
                for (unsigned u = 0; u < 256; ++u) {
                    unsigned at = 1024 + v * 2048;
                    unsigned index = (u * 13 + v * 7 + frame * 3) & 7;
                    if (format == 0) {
                        unsigned shift = (u & 1) * 4;
                        atlas[at + u / 2] = (atlas[at + u / 2] & ~(15u << shift)) |
                                            index << shift;
                    } else if (format == 1) atlas[at + u] = index;
                    else put(at + u * 2, 0x8000 | probe_color(u, v, frame));
                }
            EXPECT(!native->clear(t, error, sizeof error));
            memset(other, 0, sizeof other);
            for (unsigned draw = 0; draw < 96; ++draw) {
                float x0 = (draw % 8) * 40.0f - 3.25f;
                float y0 = (draw / 8) * 20.0f - 2.25f;
                float w = 19 + draw % 18, h = 12 + draw % 8;
                unsigned us = 1 + (draw * 17 + frame * 11) % 53;
                unsigned vs = 1 + (draw * 19 + frame * 7) % 47;
                float u0 = 2 + (draw * 13 + frame * 23) % (253 - us);
                float v0 = 2 + (draw * 29 + frame * 31) % (253 - vs);
                gex_gpu_vertex q[] = {{x0, y0, u0, v0}, {x0 + w, y0, u0 + us, v0},
                                     {x0, y0 + h, u0, v0 + vs},
                                     {x0 + w, y0 + h, u0 + us, v0 + vs}};
                EXPECT(!native->polygon(t, q, 0, 0, 320, 240, &m, error, sizeof error));
                float ou = -(float)us / w * 0.5f + 1.0f / 1024;
                float ov = -(float)vs / h * 0.5f + 1.0f / 1024;
                float du = ((u0 + us + ou) - (u0 + ou)) / w;
                float dv = ((v0 + vs + ov) - (v0 + ov)) / h;
                int left = (int)ceilf(x0 - 0.5f), right = (int)ceilf(x0 + w - 0.5f);
                int top = (int)ceilf(y0 - 0.5f), bottom = (int)ceilf(y0 + h - 0.5f);
                if (left < 0) left = 0;
                if (top < 0) top = 0;
                if (right > 320) right = 320;
                if (bottom > 240) bottom = 240;
                for (int y = top; y < bottom; ++y)
                    for (int x = left; x < right; ++x) {
                        unsigned u = (unsigned)floorf(u0 + ou + (x + 0.5f - x0) * du);
                        unsigned v = (unsigned)floorf(v0 + ov + (y + 0.5f - y0) * dv);
                        other[y * 320 + x] = (uint16_t)probe_color(u, v, frame);
                    }
            }
            EXPECT(!native->copy(copy, t, 255, error, sizeof error));
            EXPECT(!gpu->download(copy, pixels, error, sizeof error));
            for (unsigned i = 0; i < 320 * 240; ++i)
                if (pixels[i] != other[i]) {
                    printf("PVR sparse sampling mismatch: format=%u frame=%u x=%u y=%u actual=%x expected=%x\n",
                           format, frame, i % 320, i / 320, pixels[i], other[i]);
                    ++failures;
                    break;
                }
        }
        gpu->destroy(copy); gpu->destroy(t);
    }
}

static void lazy_alpha_regression(const gex_packet_gpu *gpu) {
    const gex_gpu_native_ops *native = gpu->native;
    char error[256] = {0};
    void *t = gpu->create(320, 240, error, sizeof error);
    void *copy = gpu->create(320, 240, error, sizeof error);
    if (!t || !copy) {
        printf("PVR lazy alpha targets failed: %s\n", error);
        ++failures;
        if (copy) gpu->destroy(copy);
        if (t) gpu->destroy(t);
        return;
    }
    native->bind(t, atlas, 0);
    native->atlas_write(atlas, 0, sizeof atlas);
    memset(atlas, 0, sizeof atlas);
    put(1024 + 3 * 2048 + 6, 0x3e0);
    put(1024 + 3 * 2048 + 8, 0x8000);
    put(1024 + 4 * 2048 + 6, 0xfc00);
    EXPECT(!native->clear(t, error, sizeof error));
    gex_gpu_material background = {.opcode = 0x60, .rgb = 0xff};
    gex_gpu_vertex fill[] = {{0, 0, 0, 0}, {320, 0, 0, 0},
                            {0, 240, 0, 0}, {320, 240, 0, 0}};
    EXPECT(!native->polygon(t, fill, 0, 0, 320, 240, &background, error, sizeof error));
    gex_gpu_material material = {8 | 2 << 7, 0, 0x2c, 0x808080, atlas, table};
    /* Cross paired columns and rows, then create alpha images while the
     * opaque image is still referenced by queued geometry. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        float x = 10 + pass * 10;
        gex_gpu_vertex block[] = {{x, 20, 3, 3}, {x + 2, 20, 5, 3},
                                 {x, 22, 3, 5}, {x + 2, 22, 5, 5}};
        material.opcode = pass ? 0x2e : 0x2c;
        EXPECT(!native->polygon(t, block, 0, 0, 320, 240, &material, error, sizeof error));
    }
    for (unsigned draw = 0; draw < 4; ++draw) {
        unsigned at = draw == 3 ? 1024 + 3 * 2048 + 6 : 1024 + 101 * 2048 + 82;
        if (draw) {
            native->atlas_write(atlas, at, 2);
            put(at, draw == 1 ? 0x8000 : draw == 2 ? 0x3e0 : 0);
        }
        float x = 30 + draw * 10, u = draw == 3 ? 3 : 41, v = draw == 3 ? 3 : 101;
        gex_gpu_vertex cell[] = {{x, 20, u, v}, {x + 1, 20, u + 1, v},
                                {x, 21, u, v + 1}, {x + 1, 21, u + 1, v + 1}};
        EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
    }
    material.page = 9 | 2 << 7;
    material.opcode = 0x2c;
    gex_gpu_vertex zero[] = {{80, 20, 120, 210}, {81, 20, 121, 210},
                            {80, 21, 120, 211}, {81, 21, 121, 211}};
    EXPECT(!native->polygon(t, zero, 0, 0, 320, 240, &material, error, sizeof error));
    material.opcode = 0x2e;
    for (unsigned draw = 0; draw < 2; ++draw) {
        float x = 70 + draw, u = draw ? 120 : 3, v = draw ? 210 : 3;
        gex_gpu_vertex cell[] = {{x, 20, u, v}, {x + 1, 20, u + 1, v},
                                {x, 21, u, v + 1}, {x + 1, 21, u + 1, v + 1}};
        EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
    }
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[20 * 320 + 10] == 0x3e0 && pixels[20 * 320 + 11] == 0);
    EXPECT(pixels[21 * 320 + 10] == 0x7c00 && pixels[21 * 320 + 11] == 31);
    EXPECT(pixels[20 * 320 + 20] == 0x3e0);
    EXPECT(pixels[20 * 320 + 21] >= 14 && pixels[20 * 320 + 21] <= 16);
    EXPECT((pixels[21 * 320 + 20] & 31) >= 14 && (pixels[21 * 320 + 20] & 31) <= 16);
    EXPECT((pixels[21 * 320 + 20] >> 10) >= 14 && (pixels[21 * 320 + 20] >> 10) <= 16);
    EXPECT(pixels[21 * 320 + 21] == 31 && pixels[20 * 320 + 30] == 31);
    EXPECT(pixels[20 * 320 + 40] >= 14 && pixels[20 * 320 + 40] <= 16);
    EXPECT(pixels[20 * 320 + 50] == 0x3e0 && pixels[20 * 320 + 60] == 31);
    EXPECT(pixels[20 * 320 + 70] == 31 && pixels[20 * 320 + 71] == 31);
    EXPECT(pixels[20 * 320 + 80] == 31);
    gpu->destroy(copy); gpu->destroy(t);
}

static void alpha_pressure_regression(const gex_packet_gpu *gpu) {
    const gex_gpu_native_ops *native = gpu->native;
    char error[256] = {0};
    void *t = gpu->create(320, 240, error, sizeof error);
    void *back = gpu->create(320, 240, error, sizeof error);
    void *copy = gpu->create(320, 240, error, sizeof error);
    if (!t || !back || !copy) {
        printf("PVR alpha pressure targets failed: %s\n", error);
        ++failures;
        if (copy) gpu->destroy(copy);
        if (back) gpu->destroy(back);
        if (t) gpu->destroy(t);
        return;
    }
    native->bind(t, atlas, 0);
    native->bind(back, atlas, 1);
    native->atlas_write(atlas, 0, sizeof atlas);
    memset(atlas, 0, sizeof atlas);
    gex_gpu_material background = {.opcode = 0x60, .rgb = 0xff};
    /* Retain a real background and create a self-feedback snapshot before
     * filling the cache: production VRAM is not exclusively page storage. */
    for (unsigned frame = 0; frame < 3; ++frame) {
        for (unsigned plane = 0; plane < 2; ++plane) {
            void *target = plane ? back : t;
            EXPECT(!native->clear(target, error, sizeof error));
            for (unsigned i = 0; i < 16; ++i) {
                gex_gpu_vertex tile[] = {{i * 4, 180, 0, 0}, {i * 4 + 4, 180, 0, 0},
                                         {i * 4, 184, 0, 0}, {i * 4 + 4, 184, 0, 0}};
                EXPECT(!native->polygon(target, tile, 0, 0, 320, 240, &background, error, sizeof error));
            }
            EXPECT(!native->copy(copy, target, 255, error, sizeof error));
        }
    }
    gex_gpu_material material = {256, 0, 0x2c, 0x808080, atlas, table};
    gex_gpu_vertex feedback_quad[] = {{200, 10, 0, 180}, {204, 10, 4, 180},
                                     {200, 14, 0, 184}, {204, 14, 4, 184}};
    for (unsigned plane = 0; plane < 2; ++plane) {
        void *target = plane ? back : t;
        material.page = 256 | (plane ? 16 : 0);
        EXPECT(!native->polygon(target, feedback_quad, 0, 0, 320, 240, &material, error, sizeof error));
        EXPECT(!native->copy(copy, target, 255, error, sizeof error));
    }
    printf("PVR alpha pressure available before pages: %lu\n", (unsigned long)pvr_mem_available());
    for (unsigned page = 0; page < 32; ++page)
        put(((page & 15) * 128 + (page & 16 ? 0x80000 : 0) + 240 * 2048) & 0xfffff, 0x801f);
    int failed = 0;
    for (unsigned pass = 0; pass < 2 && !failed; ++pass) {
        EXPECT(!native->clear(t, error, sizeof error));
        for (unsigned page = 0; page < 32 && !failed; ++page) {
            material.page = 256 | page;
            for (unsigned alpha = 0; alpha < 2; ++alpha) {
                float x = page * 3 + alpha, y = 40 + pass;
                material.opcode = alpha ? 0x2e : 0x2c;
                gex_gpu_vertex cell[] = {{x, y, 0, 240}, {x + 1, y, 1, 240},
                                         {x, y + 1, 0, 241}, {x + 1, y + 1, 1, 241}};
                int result = native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error);
                EXPECT(!result);
                if (result) {
                    printf("PVR alpha pressure page=%u pass=%u: %s\n", page, pass, error);
                    failed = 1;
                    break;
                }
            }
        }
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        if (!failed)
            for (unsigned page = 0; page < 32; ++page) {
                unsigned at = (40 + pass) * 320 + page * 3;
                EXPECT(pixels[at] == 31);
                EXPECT(pixels[at + 1] >= 14 && pixels[at + 1] <= 16);
            }
    }
    gpu->destroy(copy); gpu->destroy(back); gpu->destroy(t);
}

static void fragmented_alpha_regression(const gex_packet_gpu *gpu, int remap) {
    const gex_gpu_native_ops *native = gpu->native;
    char error[256] = {0};
    void *t = gpu->create(320, 240, error, sizeof error);
    void *copy = gpu->create(320, 240, error, sizeof error);
    if (!t || !copy) {
        printf("PVR fragmented alpha targets failed: %s\n", error);
        ++failures;
        if (copy) gpu->destroy(copy);
        if (t) gpu->destroy(t);
        return;
    }
    native->bind(t, atlas, 0);
    native->atlas_write(atlas, 0, sizeof atlas);
    memset(atlas, 0, sizeof atlas);
    const unsigned pages[] = {8, 9, 10, 15};
    gex_gpu_material material = {256, 0, 0x64, 0x808080, atlas, table};
    EXPECT(!native->clear(t, error, sizeof error));
    for (unsigned i = 0; i < 4; ++i) {
        unsigned at = pages[i] * 128 + 240 * 2048;
        put(at, 0x801f); put(at + 2, 0x3e0);
        material.page = 256 | pages[i];
        float x = 10 + i * 5;
        gex_gpu_vertex cell[] = {{x, 50, 0, 240}, {x + 3, 50, 3, 240},
                                 {x, 51, 0, 241}, {x + 3, 51, 3, 241}};
        EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
    }
    if (remap) {
        atlas[512] = 0x21;
        put(0xc0002, 0x801f); put(0xc0004, 0x3e0);
        material.page = 4; material.clut = 0x6000;
        gex_gpu_vertex cell[] = {{40, 50, 0, 0}, {168, 50, 128, 0},
                                 {40, 51, 0, 1}, {168, 51, 128, 1}};
        for (unsigned i = 0; i < 65; ++i)
            EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
        EXPECT(!native->clear(t, error, sizeof error));
        for (unsigned i = 0; i < 4; ++i) {
            material.page = 256 | pages[i];
            float x = 10 + i * 5;
            gex_gpu_vertex cell[] = {{x, 50, 0, 240}, {x + 3, 50, 3, 240},
                                     {x, 51, 0, 241}, {x + 3, 51, 3, 241}};
            EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
        }
    }
    /* Keep small live allocations between free holes. Total free VRAM is
     * ample, but no hole can hold the requested image without eviction. */
    struct { pvr_ptr_t hole, guard; } blocks[512] = {{0}};
    size_t hole_bytes = (remap ? 16 : 96) * 1024;
    unsigned count = 0;
    while (count < 512 && pvr_mem_available() > hole_bytes + 128) {
        blocks[count].hole = pvr_mem_malloc(hole_bytes);
        blocks[count].guard = pvr_mem_malloc(32);
        if (!blocks[count].hole || !blocks[count].guard) {
            EXPECT(0);
            if (blocks[count].hole) pvr_mem_free(blocks[count].hole);
            if (blocks[count].guard) pvr_mem_free(blocks[count].guard);
            break;
        }
        ++count;
    }
    for (unsigned i = 0; i < count; ++i) pvr_mem_free(blocks[i].hole);
    size_t available = pvr_mem_available();
    EXPECT(count > 2 && available > 256 * 1024);
    material.opcode = remap ? 0x64 : 0x66;
    if (remap) material.page = 4;
    float v = remap ? 0 : 240, extent = remap ? 128 : 3;
    gex_gpu_vertex alpha[] = {{40, 50, 0, v}, {40 + extent, 50, extent, v},
                              {40, 51, 0, v + 1}, {40 + extent, 51, extent, v + 1}};
    int result = native->polygon(t, alpha, 0, 0, 320, 240, &material, error, sizeof error);
    EXPECT(!result);
    if (result)
        printf("PVR fragmented alpha free=%lu: %s\n", (unsigned long)available, error);
    /* The oldest opaque page was queued before reclamation. Its old output
     * must survive; requesting it again must rebuild safely under pressure. */
    material.page = 256 | pages[0]; material.opcode = 0x64;
    gex_gpu_vertex revisit[] = {{50, 50, 0, 240}, {53, 50, 3, 240},
                                {50, 51, 0, 241}, {53, 51, 3, 241}};
    if (!result)
        EXPECT(!native->polygon(t, revisit, 0, 0, 320, 240, &material, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    for (unsigned i = 0; i < 4; ++i) {
        unsigned at = 50 * 320 + 10 + i * 5;
        EXPECT(pixels[at] == 31 && pixels[at + 1] == 0x3e0 && pixels[at + 2] == 0);
    }
    if (!result) {
        if (remap) EXPECT(pixels[50 * 320 + 40] == 31);
        else EXPECT(pixels[50 * 320 + 40] >= 14 && pixels[50 * 320 + 40] <= 16);
        EXPECT(pixels[50 * 320 + 41] == 0x3e0 && pixels[50 * 320 + 42] == 0);
        EXPECT(pixels[50 * 320 + 50] == 31 && pixels[50 * 320 + 51] == 0x3e0);
    }
    for (unsigned i = 0; i < count; ++i) pvr_mem_free(blocks[i].guard);
    gpu->destroy(copy); gpu->destroy(t);
}

static void presentation_pacing_regression(const gex_packet_gpu *gpu) {
    const gex_gpu_native_ops *native = gpu->native;
    char error[256] = {0};
    for (unsigned context = 0; context < 2; ++context) {
        void *t = gpu->create(320, 240, error, sizeof error);
        if (!t) {
            printf("PVR pacing target failed: %s\n", error);
            ++failures;
            return;
        }
        size_t seen = 0;
        unsigned observed = 0;
        for (unsigned frame = 0; frame < 8; ++frame) {
            EXPECT(!native->clear(t, error, sizeof error));
            EXPECT(!native->present(t, error, sizeof error));
            pvr_stats_t stats;
            int flags = irq_disable();
            int result = pvr_get_stats(&stats);
            irq_restore(flags);
            EXPECT(!result);
            if (result) break;
            if (seen && stats.frame_count != seen) {
                EXPECT(stats.frame_count == seen + 1);
                if (stats.frame_last_time < UINT64_C(30000000))
                    printf("PVR pacing context=%u frame=%u displayed=%lu interval_ns=%llu vblank=%lu\n",
                           context, frame, (unsigned long)stats.frame_count,
                           (unsigned long long)stats.frame_last_time, (unsigned long)stats.vbl_count);
                EXPECT(stats.frame_last_time >= UINT64_C(30000000));
                ++observed;
            }
            seen = stats.frame_count;
        }
        EXPECT(observed >= 4);
        gpu->destroy(t);
    }
}

static void opaque_region_regression(const gex_packet_gpu *gpu) {
    const gex_gpu_native_ops *native = gpu->native;
    char error[256] = {0};
    void *t = gpu->create(320, 240, error, sizeof error);
    void *copy = gpu->create(320, 240, error, sizeof error);
    if (!t || !copy) {
        printf("PVR opaque region targets failed: %s\n", error);
        ++failures;
        if (copy) gpu->destroy(copy);
        if (t) gpu->destroy(t);
        return;
    }
    native->bind(t, atlas, 0);
    native->atlas_write(atlas, 0, sizeof atlas);
    memset(atlas, 0, sizeof atlas);
    const unsigned colors[] = {0, 31, 0x3e0, 0x7c00, 0x8000, 0xffff};
    for (unsigned i = 0; i < 16; ++i) put(0xcc000 + i * 2, colors[i % 6]);
    for (unsigned y = 0; y < 128; ++y)
        for (unsigned x = 0; x < 64; ++x)
            atlas[y * 2048 + 1024 + x] = (uint8_t)((x * 2 + y) % 16 |
                                                    ((x * 2 + y + 1) % 16) << 4);
    gex_gpu_material material = {8, 0x6600, 0x2c, 0x808080, atlas, table};
    gex_gpu_vertex block[] = {{10, 20, 0, 0}, {138, 20, 128, 0},
                             {10, 148, 0, 128}, {138, 148, 128, 128}};
    for (unsigned frame = 0; frame < 3; ++frame) {
        size_t before_clear = pvr_mem_available();
        EXPECT(!native->clear(t, error, sizeof error));
        gex_gpu_material marker = {.opcode = 0x60, .rgb = frame + 1};
        gex_gpu_vertex marker_quad[] = {{300, 200, 0, 0}, {301, 200, 0, 0},
                                       {300, 201, 0, 0}, {301, 201, 0, 0}};
        EXPECT(!native->polygon(t, marker_quad, 0, 0, 320, 240, &marker, error, sizeof error));
        size_t before = pvr_mem_available();
        for (unsigned i = 0; i < 65; ++i)
            EXPECT(!native->polygon(t, block, 0, 0, 320, 240, &material, error, sizeof error));
        size_t after = pvr_mem_available();
        if (frame == 1) EXPECT(before_clear >= after && before_clear - after >= 128 * 128 * 2);
        if (frame == 2) EXPECT(before == after);
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        for (unsigned y = 0; y < 128; ++y)
            for (unsigned x = 0; x < 128; ++x)
                EXPECT(pixels[(20 + y) * 320 + 10 + x] == (colors[((x + y) % 16) % 6] & 0x7fff));
    }
    gpu->destroy(copy); gpu->destroy(t);
}

static void texture_refresh_regression(const gex_packet_gpu *gpu) {
    char error[256] = {0};
    const gex_gpu_native_ops *native = gpu->native;
    void *t = gpu->create(320, 240, error, sizeof error);
    void *copy = gpu->create(320, 240, error, sizeof error);
    if (!t || !copy) {
        printf("PVR texture refresh targets failed: %s\n", error);
        ++failures;
        if (copy) gpu->destroy(copy);
        if (t) gpu->destroy(t);
        return;
    }
    native->bind(t, atlas, 0);
    native->atlas_write(atlas, 0, sizeof atlas);
    memset(atlas, 0, sizeof atlas);
    put(0xc4002, 31); put(0xc4004, 0x3e0); put(0xc4006, 0x7c00);
    for (unsigned format = 0; format < 3; ++format) {
        gex_gpu_material material = {8 | format << 7, 0x6200, 0x2c, 0x808080, atlas, table};
        refresh_texel(native, format, 0, 1);
        refresh_texel(native, format, 1, 1);
        refresh_texel(native, format, 32, 1);
        EXPECT(!native->clear(t, error, sizeof error));
        const unsigned rows[] = {0, 32, 0, 1, 0, 32};
        const unsigned expected[] = {31, 0x3e0, 0x7c00, 0x7c00, 0x7c00, 31};
        for (unsigned i = 0; i < 6; ++i) {
            if (i == 1) refresh_texel(native, format, 32, 2);
            if (i == 2) refresh_texel(native, format, 0, 3);
            if (i == 3) refresh_texel(native, format, 1, 3);
            if (i == 4) refresh_texel(native, format, 32, 1);
            float x = (float)i, v = (float)rows[i];
            gex_gpu_vertex cell[] = {{x, 198, 0, v}, {x + 1, 198, 1, v},
                                    {x, 199, 0, v + 1}, {x + 1, 199, 1, v + 1}};
            EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
        }
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        for (unsigned i = 0; i < 6; ++i) EXPECT(pixels[198 * 320 + i] == expected[i]);
        EXPECT(!native->clear(t, error, sizeof error));
        /* The first strip after a capacity drain still owns its old texel. */
        for (unsigned i = 0; i < 600; ++i) {
            if (i == 511) refresh_texel(native, format, 32, 2);
            float x = i % 300, y = 198 + i / 300;
            gex_gpu_vertex cell[] = {{x, y, 0, 32}, {x + 1, y, 1, 32},
                                    {x, y + 1, 0, 33}, {x + 1, y + 1, 1, 33}};
            EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &material, error, sizeof error));
        }
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        for (unsigned i = 0; i < 600; ++i)
            EXPECT(pixels[(198 + i / 300) * 320 + i % 300] == (i < 511 ? 31 : 0x3e0));
        refresh_texel(native, format, 33, 2);
        EXPECT(!native->clear(t, error, sizeof error));
        gex_gpu_vertex block[] = {{10, 198, 0, 32}, {11, 198, 1, 32},
                                 {10, 200, 0, 34}, {11, 200, 1, 34}};
        EXPECT(!native->polygon(t, block, 0, 0, 320, 240, &material, error, sizeof error));
        native->atlas_write_rect(atlas, 1024 + 32 * 2048, 2048, format == 2 ? 2 : 1, 2);
        for (unsigned y = 32; y < 34; ++y) {
            if (format == 2) put(1024 + y * 2048, 0x7c00);
            else atlas[1024 + y * 2048] = 3;
        }
        block[0].x = block[2].x = 11; block[1].x = block[3].x = 12;
        EXPECT(!native->polygon(t, block, 0, 0, 320, 240, &material, error, sizeof error));
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        for (unsigned y = 198; y < 200; ++y) {
            EXPECT(pixels[y * 320 + 10] == 0x3e0);
            EXPECT(pixels[y * 320 + 11] == 0x7c00);
        }
    }
    gpu->destroy(copy); gpu->destroy(t);
}

int main(void) {
    mmu_init_basic();
    EXPECT(!mmu_page_map_static(0x400000, (uintptr_t)atlas & 0x1fffffff,
                                PAGE_SIZE_64K, MMU_KERNEL_RDWR, true));
    vid_set_mode(DM_320x240, PM_RGB565);
    for (unsigned rgb = 0; rgb < 32; ++rgb)
        for (unsigned c = 0; c < 32; ++c) {
            unsigned value = rgb * c >> 4;
            table[rgb * 32 + c] = value > 31 ? 31 : value;
        }
    char error[128] = {0};
    unsigned initial_scheduler_hz = thd_get_hz();
    const gex_packet_gpu *gpu = gex_packet_gpu_get();
    /* Compatibility regressions explicitly exercise the diagnostic readback
     * contract. Production PVR targets use the native operations below. */
    gex_packet_gpu compatibility = *gpu;
    compatibility.native = NULL;
    void *t = gpu->create(320, 240, error, sizeof error);
    if (!t) { printf("PVR create failed: %s\n", error); return 1; }
    void *copy = gpu->create(320, 240, error, sizeof error);
    if (!copy) { printf("PVR second target failed: %s\n", error); gpu->destroy(t); return 1; }
    EXPECT(thd_get_hz() == 1000);
    uint16_t scanout[16] __attribute__((aligned(32)));
    for (unsigned i = 0; i < 16; ++i) scanout[i] = 0x07e0;
    /* Repeated scanout/TA remaps must not accumulate stale emulator mappings. */
    for (unsigned i = 0; i < 40000; ++i) {
        uint32_t *destination = sq_lock(vram_s);
        sq_fast_cpy(destination, scanout, 1);
        sq_wait();
        sq_unlock();
        if (memcmp(vram_s, scanout, sizeof scanout)) {
            printf("PVR FAIL: scanout SQ remap at iteration %u\n", i);
            ++failures;
            break;
        }
        sq_lock((void *)PVR_TA_INPUT);
        sq_unlock();
    }
    for (unsigned i = 0; i < 320 * 240; ++i) pixels[i] = 31;
    EXPECT(!gpu->upload(t, pixels, error, sizeof error));
    EXPECT(!gpu->download(t, other, error, sizeof error));
    EXPECT(!memcmp(pixels, other, sizeof pixels));
    gex_gpu_material m = {4, 0x1000, 0x64, 0x808080, atlas, table};
    put(0x20000, 0); put(0x20002, 0x3e0); put(0x20004, 0xfc00);
    atlas[512] = 0x21; atlas[513] = 0;
    check(gpu, t, &m, 10, 10, 3, 0, 0);
    EXPECT(pixels[10 * 320 + 10] == 0x3e0);
    EXPECT(pixels[10 * 320 + 11] == 0x7c00);
    EXPECT(pixels[10 * 320 + 12] == 31);
    m.opcode = 0x66;
    check(gpu, t, &m, 10, 11, 3, 0, 0);
    EXPECT(pixels[11 * 320 + 10] == 0x3e0); /* Unmasked texels still overwrite. */
    unsigned blended = pixels[11 * 320 + 11];
    EXPECT((blended & 31) >= 14 && (blended & 31) <= 16);
    EXPECT((blended >> 10 & 31) >= 14 && (blended >> 10 & 31) <= 16);
    EXPECT(pixels[11 * 320 + 12] == 31);
    m.page = 4 | 128; m.opcode = 0x64;
    atlas[512] = 1; atlas[513] = 2; atlas[514] = 0;
    check(gpu, t, &m, 20, 20, 3, 0, 0);
    EXPECT(pixels[20 * 320 + 20] == 0x3e0);
    EXPECT(pixels[20 * 320 + 21] == 0x7c00);
    EXPECT(pixels[20 * 320 + 22] == 31);
    put(0x20002, 31); /* CLUT mutation must not reuse stale palette colors. */
    check(gpu, t, &m, 20, 21, 3, 0, 0);
    EXPECT(pixels[21 * 320 + 20] == 31);
    atlas[512] = 2; /* Page mutation must not reuse stale indexed textures. */
    check(gpu, t, &m, 20, 22, 1, 0, 0);
    EXPECT(pixels[22 * 320 + 20] == 0x7c00);
    m.page = 4 | 256; m.opcode = 0x66;
    put(512, 0x3e0); put(514, 0xfc00); put(516, 0);
    check(gpu, t, &m, 30, 30, 3, 0, 0);
    EXPECT(pixels[30 * 320 + 30] == 0x3e0);
    EXPECT((pixels[30 * 320 + 31] & 31) >= 14);
    EXPECT(pixels[30 * 320 + 32] == 31);
    m.opcode = 0x64; m.rgb = 0x404040;
    check(gpu, t, &m, 30, 31, 1, 0, 0);
    EXPECT(pixels[31 * 320 + 30] == 15 * 32);
    table[8 * 32 + 31] = 7;
    check(gpu, t, &m, 30, 32, 1, 0, 0);
    EXPECT(pixels[32 * 320 + 30] == 7 * 32);
    table[8 * 32 + 31] = 15;
    m.rgb = 0x808080;
    EXPECT(!gpu->upload(copy, pixels, error, sizeof error));
    EXPECT(!gpu->download(copy, other, error, sizeof error));
    EXPECT(!memcmp(pixels, other, sizeof pixels));
    EXPECT(!gpu->download(t, other, error, sizeof error));
    EXPECT(!memcmp(pixels, other, sizeof pixels));
    /* Queue multiple materials before readback: palette snapshots and order
     * must survive subsequent CLUT changes and native alpha compositing. */
    m.page = 4 | 128; m.opcode = 0x64;
    atlas[512] = 1;
    gex_gpu_span span = {40, 40, 1, 0, 0, 0, 0, 0, 1, 0, {0, 0}};
    put(0x20002, 0x3e0);
    EXPECT(!gpu->draw(t, &span, 1, &m, error, sizeof error));
    span.x = 41; put(0x20002, 0x7c00);
    EXPECT(!gpu->draw(t, &span, 1, &m, error, sizeof error));
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    EXPECT(pixels[40 * 320 + 40] == 0x3e0);
    EXPECT(pixels[40 * 320 + 41] == 0x7c00);
    m.page = 4 | 128; m.rgb = 0x808080; m.opcode = 0x64;
    for (unsigned y = 0; y < 16; ++y) {
        put(0x20000 + (y + 1) * 2, y + 1);
        memset(atlas + y * 2048 + 512, y + 1, 16);
    }
    gex_gpu_span rows[16];
    for (unsigned y = 0; y < 16; ++y)
        rows[y] = (gex_gpu_span){80, 80 + (int)y, 16, 1, 0, y * 65536.0, 65536, 0, 1, 0, {0, 0}};
    EXPECT(!gpu->draw(t, rows, 16, &m, error, sizeof error));
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
            EXPECT(pixels[(80 + y) * 320 + 80 + x] == y + 1);
    rows[0] = (gex_gpu_span){140, 80, 16, 1, 0, 0, 65536, 0, 1, 0, {16, 65536}};
    EXPECT(!gpu->draw(t, rows, 1, &m, error, sizeof error));
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
            EXPECT(pixels[(80 + y) * 320 + 140 + x] == y + 1);
    for (unsigned y = 0; y < 16; ++y)
        memset(atlas + ((248 + y) & 255) * 2048 + 512, y + 1, 16);
    rows[0] = (gex_gpu_span){160, 80, 16, 1, 0, 248 * 65536.0, 65536, 0, 1, 0, {16, 65536}};
    EXPECT(!gpu->draw(t, rows, 1, &m, error, sizeof error));
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
            EXPECT(pixels[(80 + y) * 320 + 160 + x] == y + 1);
    for (unsigned x = 0; x < 16; ++x) atlas[512 + x] = x + 1;
    rows[0] = (gex_gpu_span){110, 80, 16, 1, 0, 0, 32768, 0, 1, 0, {0, 0}};
    rows[1] = (gex_gpu_span){110, 81, 16, 1, 15 * 65536.0, 0, -65536, 0, 1, 0, {0, 0}};
    EXPECT(!gpu->draw(t, rows, 2, &m, error, sizeof error));
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    for (unsigned x = 0; x < 16; ++x) {
        EXPECT(pixels[80 * 320 + 110 + x] == x / 2 + 1);
        EXPECT(pixels[81 * 320 + 110 + x] == 16 - x);
    }
    const int steps[] = {49152, 65536, 98304, 131072, -49152, -65536, -98304, -131072};
    for (unsigned x = 0; x < 32; ++x) {
        atlas[512 + x] = x + 1;
        put(0x20000 + (x + 1) * 2, x % 31 + 1);
    }
    for (unsigned i = 0; i < sizeof steps / sizeof *steps; ++i)
        rows[i] = (gex_gpu_span){110, 82 + (int)i, 16, 1,
            (steps[i] < 0 ? 31 * 65536.0 : 0) + 49152, 0, steps[i], 0, 1, 0, {0, 0}};
    EXPECT(!gpu->draw(t, rows, sizeof steps / sizeof *steps, &m, error, sizeof error));
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    for (unsigned i = 0; i < sizeof steps / sizeof *steps; ++i)
        for (unsigned x = 0; x < 16; ++x) {
            int u = ((steps[i] < 0 ? 31 * 65536 : 0) + 49152 + (int)x * steps[i]) / 65536;
            EXPECT(pixels[(82 + i) * 320 + 110 + x] == u % 31 + 1);
        }
    for (unsigned i = 0; i < 320 * 240; ++i) other[i] = (uint16_t)(i * 73 & 0x7fff);
    EXPECT(!gpu->upload(t, other, error, sizeof error));
    m.opcode = 0x60; m.rgb = 0xff0000;
    span = (gex_gpu_span){319, 239, 1, 0, 0, 0, 0, 0, 1, 0, {0, 0}};
    EXPECT(!gpu->draw(t, &span, 1, &m, error, sizeof error));
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    other[320 * 240 - 1] = 0x7c00;
    for (unsigned i = 0; i < 320 * 240; ++i)
        for (unsigned shift = 0; shift < 15; shift += 5) {
            /* Native framebuffer packing may round a channel by one level;
             * every spatial sample must still correspond to its own pixel. */
            int difference = (int)(pixels[i] >> shift & 31) - (int)(other[i] >> shift & 31);
            EXPECT(abs(difference) <= 1);
        }
    EXPECT(pixels[320 * 240 - 1] == 0x7c00);
    m.opcode = 0x64; m.page = 4 | 128;
    atlas[512] = 1;
    for (unsigned i = 0; i < 9; ++i) {
        m.rgb = 0x808080; m.clut = 0x1000 + i * 16;
        put(m.clut * 32 + 2, i + 1);
        span.x = (int)i; span.y = 50;
        EXPECT(!gpu->draw(t, &span, 1, &m, error, sizeof error));
    }
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    for (unsigned i = 0; i < 9; ++i) EXPECT(pixels[50 * 320 + i] == i + 1);
    m.opcode = 0x60; m.rgb = 0xff00;
    for (unsigned i = 0; i < 600; ++i) {
        span.x = (int)(i % 320); span.y = 60 + (int)(i / 320);
        EXPECT(!gpu->draw(t, &span, 1, &m, error, sizeof error));
    }
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    for (unsigned i = 0; i < 600; ++i) EXPECT(pixels[60 * 320 + i] == 0x3e0);
    m.opcode = 0x64; m.rgb = 0x808080;
    for (unsigned i = 0; i < 32; ++i) {
        unsigned base = (i & 15) * 128 + (i & 16 ? 0x80000 : 0);
        put(base, i + 1);
    }
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < 32; ++i) {
            m.page = 256 | i;
            span.x = (int)i; span.y = 70;
            EXPECT(!gpu->draw(t, &span, 1, &m, error, sizeof error));
            EXPECT(!gpu->draw(t, &span, 1, &m, error, sizeof error));
        }
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    for (unsigned i = 0; i < 32; ++i) EXPECT(pixels[70 * 320 + i] == i + 1);
    memset(other, 0, sizeof other);
    EXPECT(!gpu->upload(t, other, error, sizeof error));
    m.page = 4 | 256;
    for (unsigned i = 0; i < 32; ++i) put(512 + i * 2, i | i << 5 | i << 10);
    check(gpu, t, &m, 0, 72, 32, 0, 0);
    m.opcode = 0x60; m.rgb = 0xff00;
    span.x = 100; span.y = 74;
    for (unsigned i = 0; i < 16; ++i) {
        EXPECT(!gpu->draw(t, &span, 1, &m, error, sizeof error));
        EXPECT(!gpu->download(t, pixels, error, sizeof error));
        for (unsigned x = 0; x < 32; ++x)
            EXPECT(pixels[72 * 320 + x] == (x | x << 5 | x << 10));
    }
    EXPECT(gpu->native != NULL);
    const gex_gpu_native_ops *native = gpu->native;
    EXPECT(!native->clear(t, error, sizeof error));
    gex_gpu_vertex quad[] = {{10, 10, 0, 0}, {20, 10, 0, 0},
                             {10, 20, 0, 0}, {20, 20, 0, 0}};
    gex_gpu_material solid = {.opcode = 0x60, .rgb = 0xff};
    EXPECT(!native->polygon(t, quad, 0, 0, 320, 240, &solid, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[10 * 320 + 10] == 31 && pixels[19 * 320 + 19] == 31);
    EXPECT(pixels[9 * 320 + 10] == 0 && pixels[20 * 320 + 10] == 0);
    EXPECT(!native->copy(copy, t, 127, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT((pixels[10 * 320 + 10] & 31) >= 14 && (pixels[10 * 320 + 10] & 31) <= 16);
    const uint8_t banner[] = {0xe0, 3, 0, 0, 0, 0x7c};
    EXPECT(!native->blit(t, 10, 10, 3, 1, banner, 6, 0, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[10 * 320 + 10] == 0x3e0);
    EXPECT(pixels[10 * 320 + 11] == 31);
    EXPECT(pixels[10 * 320 + 12] == 0x7c00);
    for (unsigned row = 0; row < 16; ++row) {
        put(0x20000 + (row + 1) * 2, row + 1);
        memset(atlas + row * 2048 + 512, row + 1, 17);
    }
    gex_gpu_material indexed = {4 | 128, 0x1000, 0x2c, 0x808080, atlas, table};
    gex_gpu_vertex scaled[] = {{80, 80, 0, 0}, {96, 80, 16, 0},
                               {80, 112, 0, 16}, {96, 112, 16, 16}};
    EXPECT(!native->polygon(t, scaled, 83, 80, 94, 112, &indexed, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    for (unsigned row = 0; row < 32; ++row)
        for (unsigned col = 83; col < 94; ++col)
            EXPECT(pixels[(80 + row) * 320 + col] == row / 2 + 1);
    EXPECT(pixels[80 * 320 + 82] == 0 && pixels[80 * 320 + 94] == 0);
    for (unsigned row = 0; row < 16; ++row)
        memset(atlas + ((248 + row) & 255) * 2048 + 512, row + 1, 17);
    gex_gpu_vertex wrapped[] = {{128, 120, 0, 248}, {112, 120, 16, 248},
                                {128, 136, 0, 264}, {112, 136, 16, 264}};
    EXPECT(!native->polygon(t, wrapped, 0, 0, 320, 240, &indexed, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    for (unsigned row = 0; row < 16; ++row)
        for (unsigned col = 112; col < 128; ++col)
            EXPECT(pixels[(120 + row) * 320 + col] == row + 1);
    indexed.page = 4;
    indexed.rgb = 0x404040;
    put(0x20002, 0x3e0);
    memset(atlas + 512, 0x11, 128);
    gex_gpu_vertex tinted[] = {{150, 150, 0, 0}, {166, 150, 16, 0},
                               {150, 151, 0, 1}, {166, 151, 16, 1}};
    EXPECT(!native->polygon(t, tinted, 0, 0, 320, 240, &indexed, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT((pixels[150 * 320 + 150] >> 5 & 31) >= 14);
    EXPECT((pixels[150 * 320 + 150] >> 5 & 31) <= 16);
    /* Opt-in write tracking must retain exact mutation checks, including
     * newly sampled gaps between previously validated ranges. */
    gex_gpu_material tracked = {4 | 128, 0x1000, 0x64, 0x808080, atlas, table};
    native->atlas_write(atlas, 0, sizeof atlas);
    put(0x20002, 0x3e0); put(0x20004, 0x7c00);
    memset(atlas + 512, 1, 17);
    check(gpu, copy, &tracked, 10, 200, 1, 0, 0);
    EXPECT(pixels[200 * 320 + 10] == 0x3e0);
    check(gpu, copy, &tracked, 10, 201, 1, 0, 0);
    EXPECT(pixels[201 * 320 + 10] == 0x3e0);
    native->atlas_write(atlas, 512, 1);
    atlas[512] = 2;
    check(gpu, copy, &tracked, 10, 202, 1, 0, 0);
    EXPECT(pixels[202 * 320 + 10] == 0x7c00);
    check(gpu, copy, &tracked, 10, 203, 1, 16, 0);
    EXPECT(pixels[203 * 320 + 10] == 0x3e0);
    native->atlas_write(atlas, 520, 1);
    atlas[520] = 2;
    check(gpu, copy, &tracked, 10, 204, 17, 0, 0);
    EXPECT(pixels[204 * 320 + 18] == 0x7c00);
    native->atlas_write(atlas, 0, sizeof atlas);
    atlas[512] = 1;
    check(gpu, copy, &tracked, 10, 205, 1, 0, 0);
    EXPECT(pixels[205 * 320 + 10] == 0x3e0);
    tracked.page = 15 | 16 | 256;
    native->atlas_write(atlas, 20, 2);
    put(20, 0x3e0);
    check(gpu, copy, &tracked, 10, 206, 1, 74, 255);
    EXPECT(pixels[206 * 320 + 10] == 0x3e0);
    check(gpu, copy, &tracked, 10, 207, 1, 74, 255);
    EXPECT(pixels[207 * 320 + 10] == 0x3e0);
    native->atlas_write(atlas, 20, 2);
    put(20, 0x7c00);
    check(gpu, copy, &tracked, 10, 208, 1, 74, 255);
    EXPECT(pixels[208 * 320 + 10] == 0x7c00);
    native->atlas_write(atlas, 512, 128);
    memset(atlas + 512, 0x11, 128);
    native->atlas_write(atlas, 0x20002, 2);
    put(0x20002, 0x3e0);
    indexed.rgb = 0x808080;
    EXPECT(!native->polygon(t, tinted, 0, 0, 320, 240, &indexed, error, sizeof error));
    native->atlas_write(atlas, 0x20002, 2);
    put(0x20002, 0x7c00);
    gex_gpu_vertex recolored[] = {{150, 152, 0, 0}, {166, 152, 16, 0},
                                  {150, 153, 0, 1}, {166, 153, 16, 1}};
    EXPECT(!native->polygon(t, recolored, 0, 0, 320, 240, &indexed, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[150 * 320 + 150] == 0x3e0);
    EXPECT(pixels[152 * 320 + 150] == 0x7c00);
    /* A PAL8 bank overwrites sixteen PAL4 banks, including cached source
     * keys left by an earlier list. Reusing those keys must not return the
     * new PAL8 colors for an unchanged PAL4 material. */
    gex_gpu_material small = {4, 0x1800, 0x2c, 0x808080, atlas, table};
    gex_gpu_material large = {5 | 128, 0x1900, 0x2c, 0x808080, atlas, table};
    native->atlas_write(atlas, 512, 128);
    memset(atlas + 512, 0x11, 128);
    native->atlas_write(atlas, 640, 256);
    memset(atlas + 640, 1, 256);
    for (unsigned i = 0; i < 256; ++i) put(0x32000 + i * 2, 0x7c00);
    native->atlas_write(atlas, 0x32000, 512);
    put(0x30002, 31); put(0x30202, 0x3e0);
    native->atlas_write(atlas, 0x30000, 544);
    gex_gpu_vertex bank_dot[] = {{30, 210, 0, 0}, {31, 210, 1, 0},
                                {30, 211, 0, 1}, {31, 211, 1, 1}};
    EXPECT(!native->polygon(t, bank_dot, 0, 0, 320, 240, &small, error, sizeof error));
    small.clut = 0x1810;
    bank_dot[0].x = bank_dot[2].x = 31; bank_dot[1].x = bank_dot[3].x = 32;
    EXPECT(!native->polygon(t, bank_dot, 0, 0, 320, 240, &small, error, sizeof error));
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    EXPECT(pixels[210 * 320 + 30] == 31 && pixels[210 * 320 + 31] == 0x3e0);
    for (unsigned frame = 0; frame < 3; ++frame) {
        bank_dot[0].y = bank_dot[1].y = 212 + frame;
        bank_dot[2].y = bank_dot[3].y = 213 + frame;
        bank_dot[0].x = bank_dot[2].x = 30; bank_dot[1].x = bank_dot[3].x = 31;
        EXPECT(!native->polygon(t, bank_dot, 0, 0, 320, 240, &large, error, sizeof error));
        bank_dot[0].x = bank_dot[2].x = 31; bank_dot[1].x = bank_dot[3].x = 32;
        EXPECT(!native->polygon(t, bank_dot, 0, 0, 320, 240, &small, error, sizeof error));
        EXPECT(!gpu->download(t, pixels, error, sizeof error));
        EXPECT(pixels[(212 + frame) * 320 + 30] == 0x7c00);
        EXPECT(pixels[(212 + frame) * 320 + 31] == 0x3e0);
    }
    /* PAL8 alignment leaves banks 1--15 free after the first PAL4 draw.
     * Fill a gap without moving or overwriting either queued palette. */
    EXPECT(!native->clear(t, error, sizeof error));
    bank_dot[0].y = bank_dot[1].y = 218;
    bank_dot[2].y = bank_dot[3].y = 219;
    /* An irregular quad with conservative wrapped bounds retains PAL4,
     * so this still exercises the allocator's native alignment gaps. */
    bank_dot[0].u = bank_dot[2].u = -0.25f;
    bank_dot[1].y += 0.125f;
    bank_dot[0].x = bank_dot[2].x = 30; bank_dot[1].x = bank_dot[3].x = 31;
    small.clut = 0x1800;
    EXPECT(!native->polygon(t, bank_dot, 0, 0, 320, 240, &small, error, sizeof error));
    bank_dot[0].x = bank_dot[2].x = 31; bank_dot[1].x = bank_dot[3].x = 32;
    EXPECT(!native->polygon(t, bank_dot, 0, 0, 320, 240, &large, error, sizeof error));
    bank_dot[0].x = bank_dot[2].x = 32; bank_dot[1].x = bank_dot[3].x = 33;
    small.clut = 0x1810;
    EXPECT(!native->polygon(t, bank_dot, 0, 0, 320, 240, &small, error, sizeof error));
    EXPECT(PVR_GET(PVR_PALETTE_TABLE_BASE + 4) == 0xffff0000);
    EXPECT(PVR_GET(PVR_PALETTE_TABLE_BASE + 17 * 4) == 0xff00ff00);
    EXPECT(PVR_GET(PVR_PALETTE_TABLE_BASE + 257 * 4) == 0xff0000ff);
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    EXPECT(pixels[218 * 320 + 30] == 31 && pixels[218 * 320 + 31] == 0x7c00 &&
           pixels[218 * 320 + 32] == 0x3e0);
    /* Cold pressure retains exact native output before remaps are warmed. */
    native->atlas_write(atlas, 0x60000, 96 * 32);
    for (unsigned i = 0; i < 96; ++i)
        for (unsigned j = 0; j < 16; ++j) {
            unsigned c = (i * 17 + j) & 255;
            put(0x60000 + i * 32 + j * 2, 1 + c % 31 + (c / 31) * 32);
        }
    native->atlas_write(atlas, 512, 128);
    memset(atlas + 512, 0x11, 128);
    gex_gpu_material pressure = {4, 0x3000, 0x2c, 0x808080, atlas, table};
    EXPECT(!native->clear(t, error, sizeof error));
    for (unsigned i = 0; i < 96; ++i) {
        gex_gpu_vertex cell[] = {{(float)i, 220, 0, 0}, {(float)i + 1, 220, 1, 0},
                                {(float)i, 221, 0, 1}, {(float)i + 1, 221, 1, 1}};
        pressure.clut = 0x3000 + i;
        EXPECT(!native->polygon(t, cell, 0, 0, 320, 240, &pressure, error, sizeof error));
    }
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    for (unsigned i = 0; i < 96; ++i) {
        unsigned c = (i * 17 + 1) & 255;
        EXPECT(pixels[220 * 320 + i] == 1 + c % 31 + (c / 31) * 32);
    }
    native->atlas_write(atlas, 0x20004, 4);
    put(0x20004, 0x7c00); put(0x20006, 31);
    indexed.rgb = 0x808080;
    native->atlas_write(atlas, 512, 1);
    native->atlas_write(atlas, 2560, 1);
    atlas[512] = atlas[2560] = 0x11;
    check(gpu, copy, &indexed, 20, 200, 2, 0, 0);
    native->atlas_write(atlas, 512, 1);
    native->atlas_write(atlas, 2560, 1);
    atlas[512] = 0x21; atlas[2560] = 0x31;
    check(gpu, copy, &indexed, 20, 201, 1, 1, 0);
    EXPECT(pixels[201 * 320 + 20] == 0x7c00);
    check(gpu, copy, &indexed, 20, 202, 2, 0, 1);
    EXPECT(pixels[202 * 320 + 20] == 0x7c00);
    EXPECT(pixels[202 * 320 + 21] == 31);
    check(gpu, copy, &indexed, 20, 203, 2, 0, 0);
    EXPECT(pixels[203 * 320 + 20] == 0x7c00);
    EXPECT(pixels[203 * 320 + 21] == 0x7c00);
    gex_gpu_material direct = {4 | 256, 0, 0x2e, 0x808080, atlas, table};
    gex_gpu_vertex dot[] = {{20, 204, 0, 0}, {21, 204, 1, 0},
                            {20, 205, 0, 1}, {21, 205, 1, 1}};
    native->atlas_write(atlas, 512, 2);
    put(512, 0x3e0);
    EXPECT(!native->polygon(t, dot, 0, 0, 320, 240, &direct, error, sizeof error));
    native->atlas_write(atlas, 512, 2);
    put(512, 0xfc00);
    dot[0].y = dot[1].y = 206; dot[2].y = dot[3].y = 207;
    EXPECT(!native->polygon(t, dot, 0, 0, 320, 240, &direct, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[204 * 320 + 20] == 0x3e0);
    EXPECT((pixels[206 * 320 + 20] >> 10 & 31) >= 14);
    EXPECT((pixels[206 * 320 + 20] >> 10 & 31) <= 16);
    native->atlas_write(atlas, 512, 2);
    put(512, 0);
    EXPECT(!native->polygon(t, dot, 0, 0, 320, 240, &direct, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT((pixels[206 * 320 + 20] >> 10 & 31) >= 14);
    for (unsigned i = 0; i < 160 * 120; ++i) other[i] = 0xf800;
    EXPECT(!native->movie(t, (const uint8_t *)other, 160, 120, 320, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[0] == 31 && pixels[239 * 320 + 319] == 31);
    EXPECT(!native->blit(t, 10, 10, 3, 1, banner, 6, 0, error, sizeof error));
    /* Freeze an actual GPU-owned picture as a texture. Read only a separate
     * diagnostic target; the production target explicitly refuses download. */
    native->bind(t, atlas, 0);
    EXPECT(gpu->download(t, pixels, error, sizeof error) != 0);
    EXPECT(strstr(error, "readback is forbidden") != NULL);
    EXPECT(gpu->upload(t, pixels, error, sizeof error) != 0);
    gex_gpu_vertex feedback_quad[] = {{40, 40, 10, 10}, {50, 40, 20, 10},
                                      {40, 50, 10, 20}, {50, 50, 20, 20}};
    gex_gpu_material feedback_material = {256, 0, 0x2c, 0x808080, atlas, table};
    EXPECT(!native->polygon(t, feedback_quad, 0, 0, 320, 240,
                            &feedback_material, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[40 * 320 + 40] == 0x3e0);
    EXPECT(pixels[40 * 320 + 41] == 31);
    EXPECT(pixels[40 * 320 + 42] == 0x7c00);
    EXPECT(pixels[41 * 320 + 40] == 31);
    gex_gpu_vertex edge_feedback[] = {{60, 60, 74, 9}, {70, 60, 84, 9},
                                      {60, 70, 74, 19}, {70, 70, 84, 19}};
    feedback_material.page = 15 | 256;
    EXPECT(!native->polygon(t, edge_feedback, 0, 0, 320, 240,
                            &feedback_material, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    EXPECT(pixels[60 * 320 + 60] == 0x3e0 && pixels[60 * 320 + 61] == 31);
    EXPECT(pixels[60 * 320 + 62] == 0x7c00);
    indexed.page = 4;
    indexed.clut = 0x1020;
    /* Only the visible four rows sample the asset above the picture.
     * Offscreen vertices must not turn a clipped CEL into picture feedback. */
    native->atlas_write(atlas, 0x20402, 2);
    put(0x20402, 0x3e0);
    for (unsigned row = 0; row < 8; ++row) {
        native->atlas_write(atlas, row * 2048 + 512, 128);
        memset(atlas + row * 2048 + 512, 0x11, 128);
    }
    gex_gpu_vertex clipped_asset[] = {{100, 236, 0, 0}, {116, 236, 16, 0},
                                      {100, 252, 0, 16}, {116, 252, 16, 16}};
    EXPECT(!native->polygon(t, clipped_asset, 0, 0, 320, 240,
                            &indexed, error, sizeof error));
    EXPECT(!native->copy(copy, t, 255, error, sizeof error));
    EXPECT(!gpu->download(copy, pixels, error, sizeof error));
    for (unsigned row = 236; row < 240; ++row)
        for (unsigned x = 100; x < 116; ++x) EXPECT(pixels[row * 320 + x] == 0x3e0);
    gex_gpu_vertex edge_asset[] = {{120, 220, 0, 0}, {136, 220, 16, 0},
                                   {120, 228, 0, 8}, {136, 228, 16, 8}};
    EXPECT(!native->polygon(t, edge_asset, 0, 0, 320, 240,
                            &indexed, error, sizeof error));
    EXPECT(native->polygon(t, feedback_quad, 0, 0, 320, 240,
                           &indexed, error, sizeof error) != 0);
    EXPECT(strstr(error, "indexed framebuffer sampling") != NULL);
    indexed.clut = 0x1000;
    /* Learn an ordered background, then move an overlapping live sprite.
     * Camera/material changes and a write midway through deferred validation
     * must replay the prefix rather than composite stale scenery. */
    indexed.clut = 0x1020;
    indexed.page = 4;
    indexed.opcode = 0x2c;
    indexed.rgb = 0x808080;
    native->atlas_write(atlas, 1152, 8);
    for (unsigned x = 0; x < 4; ++x) put(1152 + x * 2, 0x3e0);
    gex_gpu_material backdrop_direct = {9 | 256, 0, 0x2c, 0x808080, atlas, table};
    gex_raster_state background_state = {atlas, table, 4, 0, 8, 320, 248, 0};
    for (unsigned frame = 0; frame < 9; ++frame) {
        unsigned recognized = 0;
        EXPECT(!native->clear(t, error, sizeof error));
        EXPECT(!native->clear(t, error, sizeof error));
        for (unsigned tile = 0; tile < 16; ++tile) {
            gex_gpu_vertex background[] = {
                {20 + tile * 4, 180, 0, 0}, {24 + tile * 4, 180, 4, 0},
                {20 + tile * 4, 181, 0, 1}, {24 + tile * 4, 181, 4, 1}
            };
            if (frame == 3)
                for (unsigned i = 0; i < 4; ++i) background[i].x += 1;
            if (frame == 6 && tile == 12)
                for (unsigned i = 0; i < 4; ++i) background[i].x += 1;
            int tinted = frame == 4 || (frame == 6 && tile >= 8);
            indexed.rgb = tinted ? 0x404040 : 0x808080;
            if (frame == 2 && tile == 8) {
                native->atlas_write_rect(atlas, 0x60000, 2048, 64, 4);
                for (unsigned row = 0; row < 4; ++row)
                    memset(atlas + 0x60000 + row * 2048, 0x55, 64);
            }
            if (frame == 8 && tile == 8) {
                native->atlas_write(atlas, 0x20402, 2);
                put(0x20402, 31);
            }
            uint8_t raw[40] = {0};
            raw[0] = (uint8_t)frame;
            raw[4] = raw[5] = raw[6] = tinted && !(tile & 1) ? 64 : 128;
            raw[7] = 0x2c;
            raw[14] = 0x20; raw[15] = 0x10;
            raw[22] = tile & 1 ? 9 : 4; raw[23] = tile & 1 ? 1 : 0;
            for (unsigned i = 0; i < 4; ++i) {
                raw[8 + i * 8] = (uint8_t)background[i].x;
                raw[10 + i * 8] = (uint8_t)(background[i].y + 8);
                raw[12 + i * 8] = (uint8_t)background[i].u;
                raw[13 + i * 8] = (uint8_t)background[i].v;
            }
            int consumed = native->packet(t, raw, &background_state, 0, 0,
                                           error, sizeof error);
            EXPECT(consumed >= 0);
            recognized += consumed == 1;
            if (!consumed)
                EXPECT(!native->polygon(t, background, 0, 0, 320, 240,
                                        tile & 1 ? &backdrop_direct : &indexed, error, sizeof error));
        }
        if (frame == 2) EXPECT(recognized == 16);
        if (frame == 3) EXPECT(!recognized);
        if (frame == 4 || frame == 5 || frame == 7) EXPECT(recognized == 16);
        if (frame == 6) EXPECT(recognized == 12);
        gex_gpu_material sprite = {0, 0, 0x60, 0x0000ff, atlas, table};
        gex_gpu_vertex moving[] = {
            {20 + frame, 180, 0, 0}, {21 + frame, 180, 0, 0},
            {20 + frame, 181, 0, 0}, {21 + frame, 181, 0, 0}
        };
        EXPECT(!native->polygon(t, moving, 0, 0, 320, 240,
                                &sprite, error, sizeof error));
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        EXPECT(pixels[180 * 320 + 20 + frame] == 31);
        if (frame == 3) EXPECT(pixels[180 * 320 + 20] == 0);
        if (frame < 4 || frame == 5 || frame == 7)
            EXPECT(pixels[180 * 320 + 70] == 0x3e0);
        if (frame == 4 || frame == 6) {
            EXPECT((pixels[180 * 320 + 70] >> 5 & 31) >= 14);
            EXPECT((pixels[180 * 320 + 70] >> 5 & 31) <= 16);
        }
        if (frame == 8) {
            EXPECT(pixels[180 * 320 + 30] == 0x3e0);
            EXPECT(pixels[180 * 320 + 70] == 31);
            EXPECT(pixels[180 * 320 + 75] == 0x3e0);
        }
    }
    native->atlas_write(atlas, 0x20402, 2);
    put(0x20402, 0x801f);
    indexed.opcode = 0x2e;
    for (unsigned frame = 0; frame < 4; ++frame) {
        if (frame == 3) indexed.opcode = 0x2c;
        EXPECT(!native->clear(t, error, sizeof error));
        for (unsigned tile = 0; tile < 16; ++tile) {
            gex_gpu_vertex alpha_background[] = {
                {20 + tile * 4, 180, 0, 0}, {24 + tile * 4, 180, 4, 0},
                {20 + tile * 4, 181, 0, 1}, {24 + tile * 4, 181, 4, 1}
            };
            EXPECT(!native->polygon(t, alpha_background, 0, 0, 320, 240,
                                    &indexed, error, sizeof error));
        }
        EXPECT(!native->copy(copy, t, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        if (frame < 3) {
            EXPECT((pixels[180 * 320 + 70] & 31) >= 14);
            EXPECT((pixels[180 * 320 + 70] & 31) <= 16);
        } else EXPECT(pixels[180 * 320 + 70] == 31);
    }
    EXPECT(!native->present(t, error, sizeof error));
    gpu->destroy(copy); gpu->destroy(t);
    EXPECT(thd_get_hz() == initial_scheduler_hz);
    t = gpu->create(320, 240, error, sizeof error);
    if (!t) { printf("PVR storage target failed: %s\n", error); return 1; }
    native->atlas_write(atlas, 0, 0x100000);
    put(512, 0x801f);
    EXPECT(!native->clear(t, error, sizeof error));
    size_t before_opaque = pvr_mem_available();
    gex_gpu_material shared_direct = {4 | 256, 0, 0x2c, 0x808080, atlas, table};
    gex_gpu_vertex storage_quad[] = {{10, 10, 0, 0}, {11, 10, 1, 0},
                                     {10, 11, 0, 1}, {11, 11, 1, 1}};
    EXPECT(!native->polygon(t, storage_quad, 0, 0, 320, 240,
                            &shared_direct, error, sizeof error));
    size_t after_opaque = pvr_mem_available();
    EXPECT(before_opaque - after_opaque >= 256 * 1024);
    EXPECT(before_opaque - after_opaque <= 256 * 1024 + 256);
    for (unsigned i = 0; i < 4; ++i) storage_quad[i].x += 1;
    shared_direct.opcode = 0x2e;
    EXPECT(!native->polygon(t, storage_quad, 0, 0, 320, 240,
                            &shared_direct, error, sizeof error));
    size_t after_alpha = pvr_mem_available();
    EXPECT(after_opaque - after_alpha >= 256 * 1024);
    EXPECT(after_opaque - after_alpha <= 256 * 1024 + 256);
    shared_direct.opcode = 0x2c;
    for (unsigned i = 0; i < 4; ++i) storage_quad[i].x += 1;
    EXPECT(!native->polygon(t, storage_quad, 0, 0, 320, 240,
                            &shared_direct, error, sizeof error));
    EXPECT(pvr_mem_available() == after_alpha);
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    EXPECT(pixels[10 * 320 + 10] == 31 && pixels[10 * 320 + 12] == 31);
    EXPECT((pixels[10 * 320 + 11] & 31) >= 14 && (pixels[10 * 320 + 11] & 31) <= 16);
    native->atlas_write_rect(atlas, 512, 2048, 2, 2);
    put(512, 0x3e0); put(512 + 2048, 0x801f);
    for (unsigned i = 0; i < 4; ++i) storage_quad[i].x += 1;
    shared_direct.opcode = 0x2e;
    EXPECT(!native->polygon(t, storage_quad, 0, 0, 320, 240,
                            &shared_direct, error, sizeof error));
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    EXPECT(pixels[10 * 320 + 13] == 0x3e0);
    EXPECT(pvr_mem_available() == after_alpha);
    native->atlas_write(atlas, 512, 2);
    put(512, 0);
    for (unsigned i = 0; i < 4; ++i) storage_quad[i].x += 1;
    shared_direct.opcode = 0x2c;
    EXPECT(!native->polygon(t, storage_quad, 0, 0, 320, 240,
                            &shared_direct, error, sizeof error));
    EXPECT(!gpu->download(t, pixels, error, sizeof error));
    EXPECT(pixels[10 * 320 + 14] == 0);
    EXPECT(pixels[10 * 320 + 10] == 31 && pixels[10 * 320 + 12] == 31);
    gpu->destroy(t);
    EXPECT(thd_get_hz() == initial_scheduler_hz);
    gex_packet_gpu native_raster = *gpu;
    native_raster.create = remember_device;
    gex_raster *cached_raster = gex_raster_create(320, 240);
    if (!cached_raster || gex_raster_enable_gpu(cached_raster, &native_raster)) {
        printf("PVR cached raster target failed\n"); return 1;
    }
    gex_raster_bind(cached_raster, atlas, 0);
    copy = gpu->create(320, 240, error, sizeof error);
    if (!copy) { printf("PVR cached raster diagnostic target failed: %s\n", error); return 1; }
    native->atlas_write(atlas, 512, 64);
    memset(atlas + 512, 0x11, 64);
    native->atlas_write(atlas, 0x20402, 2);
    put(0x20402, 0x3e0);
    for (unsigned frame = 0; frame < 4; ++frame) {
        gex_raster_state cache_state = {atlas, table, 4, 0, 8, 320, 248, 0};
        if (frame == 3) {
            cache_state.world_clip = 1;
            cache_state.world_left = 24; cache_state.world_right = 320;
            cache_state.world_top = 8; cache_state.world_bottom = 248;
        }
        gex_raster_clear(cached_raster);
        for (unsigned tile = 0; tile < 16; ++tile) {
            uint8_t cached_packet[20] = {0};
            cached_packet[0] = (uint8_t)frame;
            cached_packet[4] = cached_packet[5] = cached_packet[6] = 128;
            cached_packet[7] = 0x64;
            cached_packet[8] = (uint8_t)(20 + tile * 4);
            cached_packet[10] = 188;
            cached_packet[14] = 0x20; cached_packet[15] = 0x10;
            cached_packet[16] = 4; cached_packet[18] = 1;
            gex_raster_packet(cached_raster, cached_packet, &cache_state);
        }
        uint8_t empty_packet[20] = {0};
        empty_packet[7] = 0x64; empty_packet[18] = 1;
        gex_raster_packet(cached_raster, empty_packet, &cache_state);
        gex_raster_fill(cached_raster, 20 + frame, 180, 1, 1, 0x0000ff);
        EXPECT(!native->copy(copy, raster_device, 255, error, sizeof error));
        EXPECT(!gpu->download(copy, pixels, error, sizeof error));
        EXPECT(pixels[180 * 320 + 20 + frame] == 31);
        EXPECT(pixels[180 * 320 + 70] == 0x3e0);
        if (frame == 3) EXPECT(pixels[180 * 320 + 20] == 0);
    }
    gpu->destroy(copy);
    gex_raster_destroy(cached_raster);
    EXPECT(thd_get_hz() == initial_scheduler_hz);
    gex_raster *cpu = NULL, *hardware = gex_raster_create(320, 240);
#if GEX_ENABLE_SOFTWARE_RENDERER
    cpu = gex_raster_create(320, 240);
    if (!cpu) { printf("PVR reference target initialization failed\n"); return 1; }
#endif
    if (!hardware || gex_raster_enable_gpu(hardware, &compatibility)) {
        printf("PVR raster target initialization failed\n");
        gex_raster_destroy(cpu); gex_raster_destroy(hardware);
        EXPECT(thd_get_hz() == initial_scheduler_hz);
        return 1;
    }
    for (unsigned y = 0; y < 256; ++y) memset(atlas + y * 2048 + 512, 0x11, 128);
    put(0x20002, 0x3e0);
    uint8_t packet[40] = {0};
    packet[4] = packet[5] = packet[6] = 128; packet[7] = 0x2c;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned x = 20 + (i & 1 ? 128 : 0) + (i & 2 ? 13 : 0);
        unsigned y = 28 + (i & 2 ? 128 : 0);
        packet[8 + i * 8] = (uint8_t)x;
        packet[10 + i * 8] = (uint8_t)y;
        packet[12 + i * 8] = i & 1 ? 64 : 0;
        packet[13 + i * 8] = i & 2 ? 64 : 0;
    }
    packet[15] = 0x10; packet[22] = 4;
    gex_raster_state state = {atlas, table, 4, 0, 8, 320, 248, 0};
    if (cpu) gex_raster_packet(cpu, packet, &state);
    gex_raster_packet(hardware, packet, &state);
    EXPECT(((const uint16_t *)gex_raster_pixels(hardware))[40 * 320 + 40] == 0x3e0);
    if (cpu) EXPECT(!memcmp(gex_raster_pixels(cpu), gex_raster_pixels(hardware), sizeof pixels));
    uint8_t saved_packet[40];
    memcpy(saved_packet, packet, sizeof packet);
    gex_raster_state clipped = state;
    clipped.left = 25; clipped.right = 45; clipped.top = 34; clipped.bottom = 52;
    for (unsigned y = 0; y < 256; ++y)
        for (unsigned x = 0; x < 128; ++x)
            atlas[y * 2048 + 512 + x] = (uint8_t)(1 + (x + y) % 31);
    for (unsigned i = 1; i < 32; ++i) put(0x20000 + i * 2, i);
    for (unsigned test = 0; test < 4; ++test) {
        memset(packet, 0, sizeof packet);
        packet[4] = packet[5] = packet[6] = 128;
        packet[7] = test < 2 ? 0x2c : test == 2 ? 0x64 : 0x60;
        if (test < 2) {
            for (unsigned i = 0; i < 4; ++i) {
                packet[8 + i * 8] = test == 1 ? (i & 1 ? 20 : 52) : (i & 1 ? 52 : 20);
                packet[10 + i * 8] = i & 2 ? 60 : 28;
                packet[12 + i * 8] = i & 1 ? 32 : 0;
                packet[13 + i * 8] = i & 2 ? 32 : 0;
            }
            packet[22] = 4 | 128;
        } else {
            packet[8] = 20; packet[10] = 28;
            if (test == 2) {
                packet[12] = packet[13] = 248;
                packet[16] = packet[18] = 32;
            } else packet[12] = packet[14] = 32;
        }
        if (test != 3) packet[15] = 0x10;
        clipped.page = 4 | 128;
        gex_raster_clear(hardware);
        gex_raster_packet(hardware, packet, &clipped);
        if (cpu) {
            gex_raster_clear(cpu);
            gex_raster_packet(cpu, packet, &clipped);
            EXPECT(!memcmp(gex_raster_pixels(cpu), gex_raster_pixels(hardware), sizeof pixels));
        }
    }
    memcpy(packet, saved_packet, sizeof packet);
    for (unsigned y = 0; y < 256; ++y) memset(atlas + y * 2048 + 512, 0x11, 128);
    put(0x20002, 0x3e0);
    gex_raster_clear(hardware);
    gex_raster_packet(hardware, packet, &state);
    if (cpu) {
        gex_raster_clear(cpu);
        gex_raster_packet(cpu, packet, &state);
    }
    if (cpu) gex_raster_remember(cpu, (const uint8_t *)other, 640);
    gex_raster_remember(hardware, (const uint8_t *)other, 640);
    gex_raster_disable_gpu(hardware);
    EXPECT(!gex_raster_enable_gpu(hardware, &compatibility));
    if (cpu) gex_raster_sync(cpu, (const uint8_t *)other, 640);
    gex_raster_sync(hardware, (const uint8_t *)other, 640);
    if (cpu) EXPECT(!memcmp(gex_raster_pixels(cpu), gex_raster_pixels(hardware), sizeof pixels));
    gex_raster_export(hardware, (uint8_t *)other, 640);
    EXPECT(other[40 * 320 + 40] == 0x3e0);
    other[0] = 31;
    gex_raster_sync(hardware, (const uint8_t *)other, 640);
    EXPECT(((const uint16_t *)gex_raster_pixels(hardware))[0] == 31);
    put(0x20002, 0x7c00);
    gex_raster_packet(hardware, packet, &state);
    gex_raster_sync(hardware, (const uint8_t *)other, 640);
    EXPECT(((const uint16_t *)gex_raster_pixels(hardware))[40 * 320 + 40] == 0x7c00);
    gex_raster_sync(hardware, (const uint8_t *)other, 640);
    gex_raster_export(hardware, (uint8_t *)other, 640);
    EXPECT(other[0] == 31 && other[40 * 320 + 40] == 0x7c00);
    gex_raster_clear(hardware);
    gex_raster_sync(hardware, (const uint8_t *)other, 640);
    EXPECT(((const uint16_t *)gex_raster_pixels(hardware))[40 * 320 + 40] == 0);
    gex_raster_export(hardware, (uint8_t *)other, 640);
    EXPECT(other[0] == 0 && other[40 * 320 + 40] == 0);
    gex_raster_destroy(cpu); gex_raster_destroy(hardware);
    command_stream_regression(gpu);
    texture_refresh_regression(gpu);
    presentation_palette_regression(gpu);
    lazy_page_regression(gpu);
    sparse_sampling_regression(gpu);
    lazy_alpha_regression(gpu);
    alpha_pressure_regression(gpu);
    fragmented_alpha_regression(gpu, 0);
    fragmented_alpha_regression(gpu, 1);
    presentation_pacing_regression(gpu);
    opaque_region_regression(gpu);
    shared_color_reclamation_regression(gpu);
    shared_palette_regression(gpu, 0);
    shared_palette_regression(gpu, 1);
    EXPECT(thd_get_hz() == initial_scheduler_hz);
    mmu_shutdown_basic();
    printf("Dreamcast PVR packet regression: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
