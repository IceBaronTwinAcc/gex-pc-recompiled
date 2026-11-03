#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"
#include "recomp/gen/recomp_funcs.h"
#include "renderer.h"
#include "graphics_guest.h"
#include "win32hle.h"
#include "raster.h"
#include "platform/host.h"

static gex_raster *targets[2];
static gex_raster *fade_picture;
static gex_raster *shaded_picture;
static gex_render_frame fade_frame;
static unsigned frame_shade = 255;
static int render_width, render_height;
static int exclusive_gpu;
static int pictures_bound;
static const uint8_t *bound_atlas;
static void *packet_storage;
static uint32_t packet_address, packet_bytes;
static uint32_t saved_camera[6];
static int camera_active;
static const uint32_t camera_addresses[] = {0x4A2974, 0x4A2988, 0x4A293C, 0x4A2978};
static struct camera_barrier {
    int32_t x, y;
    unsigned direction;
    uint32_t record;
} *camera_barriers;
static size_t camera_barrier_count, camera_barrier_capacity;
static uint32_t camera_scene, camera_map;
static uint32_t camera_groups, camera_group_count;
static int world_clip[4], world_clip_active;
static uint32_t world_clip_scene, world_clip_map;
static uint32_t saved_hud[4];
static struct backdrop_range { uint32_t begin, end; } *backdrops;
static size_t backdrop_count, backdrop_capacity;
static int backdrop_active;
static uint32_t backdrop_scene, backdrop_map;
static uint32_t fixed_scene, fixed_map;
static uint32_t fixed_level;
static uint32_t frame_scene, frame_map;
static int transition_active;
static struct hud_range { uint32_t begin, end; int scale, offset_x; } *hud_ranges;
static size_t hud_count, hud_capacity;
static struct hud_range hud_draw;
static void synchronize(int remember);

static int fixed_screen(void) {
    if (transition_active) return 1;
    uint32_t scene = MEM32(0x4A2990);
    uint32_t level = MEM32(0x4A2964);
    /* Authored IDs: maps (49-55), title (63), game over (66), results (68). */
    if ((level >= 49 && level <= 55) || level == 63 || level == 66 || level == 68) {
        fixed_scene = scene;
        fixed_map = scene ? MEM32(scene + 4) : 0;
        fixed_level = level;
        return 1;
    }
    /* The destination level changes before the outgoing fixed screen finishes. */
    if (fixed_map && scene == fixed_scene && MEM32(scene + 4) == fixed_map)
        return 1;
    fixed_scene = fixed_map = 0;
    fixed_level = 0;
    return scene && scene == MEM32(0x455B84);
}

int gex_graphics_view_width(void) { return render_width && !fixed_screen() ? render_width : 320; }
int gex_graphics_view_height(void) { return render_height && !fixed_screen() ? render_height + 16 : 240; }

int gex_graphics_screen_point(int *x, int *y) {
    gex_render_frame frame;
    if (gex_renderer_frame(&frame)) {
        fprintf(stderr, "[renderer] menu pointer target unavailable\n");
        abort();
    }
    gex_render_rect rect = render_width ?
        gex_renderer_fit(frame.width, frame.height, 320, 224) :
        (gex_render_rect){0, 0, frame.width, frame.height};
    if (*x < rect.x || *y < rect.y || *x >= rect.x + rect.width || *y >= rect.y + rect.height)
        return 0;
    *x = (*x - rect.x) * 320 / rect.width;
    *y = (*y - rect.y) * 240 / rect.height;
    return 1;
}

void gex_graphics_backdrop_begin(void) {
    backdrop_active = 0;
    if (!render_width || !fixed_screen()) return;
    uint32_t scene = MEM32(0x4A2990);
    uint32_t map = scene ? MEM32(scene + 4) : 0;
    if (scene != backdrop_scene || map != backdrop_map) {
        backdrop_count = 0;
        backdrop_scene = scene;
        backdrop_map = map;
    }
    backdrop_active = 1;
}

static void remember_backdrop(uint32_t begin, uint32_t end) {
    if (begin == end) return;
    for (size_t i = 0; i < backdrop_count;) {
        if (begin <= backdrops[i].end && end >= backdrops[i].begin) {
            if (begin > backdrops[i].begin) begin = backdrops[i].begin;
            if (end < backdrops[i].end) end = backdrops[i].end;
            backdrops[i] = backdrops[--backdrop_count];
            i = 0;
        } else ++i;
    }
    if (backdrop_count == backdrop_capacity) {
        size_t capacity = backdrop_capacity ? backdrop_capacity * 2 : 4;
        void *next = realloc(backdrops, capacity * sizeof *backdrops);
        if (!next) {
            fprintf(stderr, "[renderer] cannot allocate screen backdrop ranges\n");
            abort();
        }
        backdrops = next;
        backdrop_capacity = capacity;
    }
    backdrops[backdrop_count++] = (struct backdrop_range){begin, end};
}

void gex_graphics_backdrop_end(void) {
    backdrop_active = 0;
}

void gex_graphics_transition_begin(void) {
    transition_active = 1;
    for (int i = 0; i < 2; ++i)
        if (targets[i]) gex_raster_clear(targets[i]);
}

void gex_graphics_transition_end(void) {
    transition_active = 0;
}

void gex_graphics_packet_allocated(uint32_t next) {
    if (!render_width || !packet_bytes) return;
    uint32_t begin = MEM32(0x4A2AE4);
    if (next < begin) begin = packet_address;
    if (begin == next) return;
    /* Packet addresses are recycled. Retire only the overwritten bytes,
     * leaving cached display lists elsewhere in the arena classified. */
    for (size_t i = 0; i < backdrop_count;) {
        struct backdrop_range range = backdrops[i];
        if (range.begin >= next || range.end <= begin) {
            ++i;
        } else if (range.begin < begin && range.end > next) {
            backdrops[i++].end = begin;
            remember_backdrop(next, range.end);
        } else if (range.begin < begin) {
            backdrops[i++].end = begin;
        } else if (range.end > next) {
            backdrops[i++].begin = next;
        } else {
            backdrops[i] = backdrops[--backdrop_count];
        }
    }
    if (backdrop_active) remember_backdrop(begin, next);
}

int gex_graphics_hud_begin(uint32_t object, int anchor, int scaled) {
    if (!render_width || (fixed_screen() && (fixed_level != 68 || transition_active))) return 0;
    const unsigned fields[] = {0x78, 0x7C, 0xC8, 0xCC};
    for (int i = 0; i < 4; ++i) saved_hud[i] = MEM32(object + fields[i]);
    int64_t scale = (int64_t)render_height * 65536 / 224;
    int64_t fit = (int64_t)render_width * 65536 / 320;
    if (scale > fit) scale = fit;
    int fraction = anchor == 1 ? 3 : anchor == 2 ? 1 : anchor == 3 ? 2 : 0;
    hud_draw.scale = 0;
    hud_draw.begin = MEM32(0x4A2AE4);
    if (scale >= 65536) {
        hud_draw.scale = (int)(scale / 65536);
        int extra = render_width - 320 * hud_draw.scale;
        hud_draw.offset_x = extra * fraction / 3;
        /* Emit original-size CELs, then replicate their rasterized pixels.
         * Scaling guest quads instead changes their inclusive UV sampling. */
        scale = 65536;
    }
    int64_t x = (int32_t)(saved_hud[0] - MEM32(0x4A2A38));
    int64_t y = (int32_t)(saved_hud[1] - MEM32(0x4A2A1C)) - 8 * 65536;
    x = x * scale / 65536;
    if (!hud_draw.scale)
        x += ((int64_t)render_width * 65536 - 320 * scale) * fraction / 3;
    y = y * scale / 65536;
    x -= (x % 65536 + 65536) % 65536;
    y -= (y % 65536 + 65536) % 65536;
    MEM32(object + 0x78) = MEM32(0x4A2974) + (uint32_t)x;
    MEM32(object + 0x7C) = MEM32(0x4A2988) + (uint32_t)(y + 8 * 65536);
    MEM32(object + 0xC8) = (uint32_t)((scaled ? (int64_t)(int32_t)saved_hud[2] : 65536) * scale / 65536);
    MEM32(object + 0xCC) = (uint32_t)((scaled ? (int64_t)(int32_t)saved_hud[3] : 65536) * scale / 65536);
    return 1;
}

void gex_graphics_hud_end(uint32_t object) {
    if (!render_width || (fixed_screen() && (fixed_level != 68 || transition_active))) return;
    if (hud_draw.begin != MEM32(0x4A2AE4)) {
        if (hud_count == hud_capacity) {
            size_t capacity = hud_capacity ? hud_capacity * 2 : 32;
            void *next = realloc(hud_ranges, capacity * sizeof *hud_ranges);
            if (!next) {
                fprintf(stderr, "[renderer] cannot allocate HUD packet ranges\n");
                abort();
            }
            hud_ranges = next;
            hud_capacity = capacity;
        }
        hud_draw.end = MEM32(0x4A2AE4);
        hud_ranges[hud_count++] = hud_draw;
    }
    hud_draw.scale = 0;
    const unsigned fields[] = {0x78, 0x7C, 0xC8, 0xCC};
    for (int i = 0; i < 4; ++i) MEM32(object + fields[i]) = saved_hud[i];
}

void gex_graphics_packet_arena(void) {
    backdrop_active = 0;
    backdrop_count = 0;
    backdrop_scene = backdrop_map = 0;
    hud_count = 0;
    if (!packet_bytes) return;
    MEM32(0x4A2AE0) = MEM32(0x4A2AE4) = packet_address;
    MEM32(0x4A2ADC) = packet_address + packet_bytes;
}

void gex_graphics_camera_begin(void) {
    camera_barrier_count = 0;
    camera_scene = MEM32(0x4A2990);
    camera_map = camera_scene ? MEM32(camera_scene + 4) : 0;
    camera_groups = MEM32(0x4A2A78);
    camera_group_count = MEM32(0x4626FC);
    world_clip_active = 0;
}

void gex_graphics_camera_barrier(uint32_t object) {
    if (!render_width || fixed_screen()) return;
    uint32_t scene = MEM32(0x4A2990);
    if (scene != camera_scene || (scene && MEM32(scene + 4) != camera_map))
        gex_graphics_camera_begin();
    struct camera_barrier barrier = {
        (int32_t)MEM32(object + 0x78), (int32_t)MEM32(object + 0x7C),
        MEM32(object + 0x98), MEM32(object + 0x18C)
    };
    for (size_t i = 0; barrier.record && i < camera_barrier_count; ++i)
        if (camera_barriers[i].record == barrier.record) {
            camera_barriers[i] = barrier;
            return;
        }
    if (camera_barrier_count == camera_barrier_capacity) {
        size_t capacity = camera_barrier_capacity ? camera_barrier_capacity * 2 : 32;
        void *next = realloc(camera_barriers, capacity * sizeof *camera_barriers);
        if (!next) {
            fprintf(stderr, "[renderer] cannot allocate camera barriers\n");
            abort();
        }
        camera_barriers = next;
        camera_barrier_capacity = capacity;
    }
    camera_barriers[camera_barrier_count++] = barrier;
}

static void camera_properties(struct camera_barrier *barrier, uint32_t properties,
                              int overrides) {
    for (uint32_t p = properties; p && MEM32(p); p += 8) {
        uint32_t key = MEM32(p);
        /* Override lists also contain spawn directives, not field values. */
        if (overrides && (key & 0x10000000)) continue;
        switch (key & 0xFFFF) {
        case 4: barrier->x = (int32_t)MEM32(p + 4); break;
        case 5: barrier->y = (int32_t)MEM32(p + 4); break;
        case 12: barrier->direction = MEM32(p + 4); break;
        }
    }
}

static void camera_bound(const struct camera_barrier *barrier, const uint32_t saved[4],
                         int64_t lower[2], int64_t upper[2]) {
    if (barrier->direction > 3) return;
    const int original[] = {320, 240};
    unsigned axis = barrier->direction / 2, other = 1 - axis;
    int64_t point[] = {barrier->x, barrier->y};
    int64_t start = (int32_t)saved[other];
    /* Select the original camera's segment and side of each one-way plane. */
    if (point[other] < start || point[other] >= start + original[other] * 65536LL)
        return;
    if (!(barrier->direction & 1)) {
        if (point[axis] <= (int32_t)saved[axis] && point[axis] > lower[axis])
            lower[axis] = point[axis];
    } else if (point[axis] >= (int32_t)saved[axis] + original[axis] * 65536LL &&
               point[axis] < upper[axis]) {
        upper[axis] = point[axis];
    }
}

static void camera_bounds(const uint32_t saved[4], int64_t lower[2], int64_t upper[2]) {
    for (size_t i = 0; i < camera_barrier_count; ++i)
        camera_bound(&camera_barriers[i], saved, lower, upper);
    /* Read the level records, not just active objects: native activation ends
     * before an expanded view has cleared a boundary. Never activate simulation
     * objects or retain guest object pointers across updates. */
    for (uint32_t i = 0; camera_groups && i < camera_group_count; ++i) {
        uint32_t group = MEM32(camera_groups + i * 4);
        if (!group || MEM32(group + 4) != 0x419870) continue;
        uint32_t records = MEM32(group), count = MEM32(group + 8);
        for (uint32_t j = 0; records && j < count; ++j) {
            uint32_t record = records + j * 16;
            uint32_t type = MEM32(record + 4) & 0x3FFF;
            if (MEM32(0x45CA38 + type * 24) != 0x40FE00) continue;
            int observed = 0;
            for (size_t k = 0; k < camera_barrier_count; ++k)
                if (camera_barriers[k].record == record) { observed = 1; break; }
            if (observed) continue;
            struct camera_barrier barrier = {
                (int32_t)(MEM32(record) & 0xFFFF0000),
                (int32_t)(MEM32(record) << 16), 0, record
            };
            uint32_t preset = MEM32(record + 8) & 0xFFFF;
            if (preset)
                camera_properties(&barrier, MEM32(MEM32(0x4A2934) + preset * 8 - 4), 0);
            camera_properties(&barrier, MEM32(record + 12), 1);
            camera_bound(&barrier, saved, lower, upper);
        }
    }
}

static int widen_camera(uint32_t saved[6]) {
    if (!render_width || camera_active || fixed_screen()) return 0;
    uint32_t scene = MEM32(0x4A2990);
    uint32_t map = scene ? MEM32(scene + 4) : 0;
    if (!map) return 0;
    for (int i = 0; i < 4; ++i) saved[i] = MEM32(camera_addresses[i]);
    saved[4] = MEM16(0x4A2A96);
    saved[5] = MEM16(0x4A2A94);
    const int extents[] = {render_width, render_height + 16};
    const int original[] = {320, 240};
    int64_t lower[2] = {0, 0};
    int64_t upper[2] = {(int32_t)MEM32(map + 4), (int32_t)MEM32(map + 8)};
    if (scene == camera_scene && map == camera_map)
        camera_bounds(saved, lower, upper);
    for (int axis = 0; axis < 2; ++axis) {
        int64_t position = (int32_t)saved[axis];
        position -= (int64_t)(extents[axis] - original[axis]) * 32768;
        int64_t limit = upper[axis] - (int64_t)extents[axis] * 65536;
        if (limit < lower[axis]) limit = lower[axis];
        if (position > limit) position = limit;
        if (position < lower[axis]) position = lower[axis];
        uint32_t next = (uint32_t)position & 0xFFFF0000;
        /* CELs also apply a half-frame camera offset. Clamp both endpoints
         * of that displacement so a stationary render camera cannot drift
         * when the native camera starts moving away from a boundary. */
        int64_t previous = (int32_t)saved[axis] -
            (int64_t)(extents[axis] - original[axis]) * 32768 -
            (int16_t)saved[axis + 4] * 131072LL;
        if (previous > limit) previous = limit;
        if (previous < lower[axis]) previous = lower[axis];
        uint32_t prior = (uint32_t)previous & 0xFFFF0000;
        MEM16(axis ? 0x4A2A94 : 0x4A2A96) =
            (uint16_t)(((int64_t)(int32_t)next - (int32_t)prior) / 131072);
        MEM32(camera_addresses[axis]) = next;
        MEM32(camera_addresses[axis + 2]) = saved[axis + 2] + next - saved[axis];
        world_clip[axis] = (int)((lower[axis] - (int32_t)next + 65535) / 65536);
        world_clip[axis + 2] = (int)((upper[axis] - (int32_t)next) / 65536);
    }
    world_clip_active = 1;
    world_clip_scene = scene;
    world_clip_map = map;
    return 1;
}

static void restore_camera(const uint32_t saved[6]) {
    for (int i = 0; i < 4; ++i) MEM32(camera_addresses[i]) = saved[i];
    MEM16(0x4A2A96) = (uint16_t)saved[4];
    MEM16(0x4A2A94) = (uint16_t)saved[5];
}

/* Restrict camera changes to drawing; player callbacks also update gameplay. */
void gex_graphics_draw_player_sprite(void) {
    uint32_t saved[6];
    int changed = widen_camera(saved);
    sub_00441150();
    if (changed) restore_camera(saved);
}

void gex_graphics_scene_begin(void) {
    if (!widen_camera(saved_camera)) return;
    /* Authored backgrounds need not cover an expanded view. Masked scenery
     * must not retain old sprite poses in the newly exposed area. */
    gex_raster_clear(targets[0]);
    camera_active = 1;
}

void gex_graphics_scene_end(void) {
    if (!camera_active) return;
    restore_camera(saved_camera);
    camera_active = 0;
}

int gex_graphics_draw_tiles(void) {
    if (!render_width || fixed_screen()) return 0;
    uint32_t map = MEM32(g_esp + 4), tiles = MEM32(g_esp + 8);
    int32_t camera_x = (int32_t)MEM32(g_esp + 12);
    int32_t camera_y = (int32_t)MEM32(g_esp + 16);
    MEM32(0x46BC58) = MEM32(0x46BC5C) = 0xFFFFFF;
    MEM32(0x46BC60) = MEM32(0x46BC64) = 0xFFFFFF;
    MEM32(0x46BC68) = 0x46BC5C; MEM32(0x46BC6C) = 0x46BC58;
    MEM32(0x46BC70) = 0x46BC60; MEM32(0x46BC74) = 0x46BC64;
    int columns = (int32_t)MEM32(map + 12), rows = (int32_t)MEM32(map + 16);
    int64_t margin_x = (int64_t)abs((int16_t)MEM16(0x4A2A96)) * 131072;
    int64_t margin_y = (int64_t)abs((int16_t)MEM16(0x4A2A94)) * 131072;
    int first_x = camera_x > margin_x ? (int)((camera_x - margin_x) / 16777216) : 0;
    int first_y = camera_y > margin_y ? (int)((camera_y - margin_y) / 16777216) : 0;
    int last_x = (int)(((int64_t)camera_x + (int64_t)render_width * 65536 + margin_x + 16777215) / 16777216);
    int last_y = (int)(((int64_t)camera_y + (int64_t)(render_height + 16) * 65536 + margin_y + 16777215) / 16777216);
    if (last_x > columns) last_x = columns;
    if (last_y > rows) last_y = rows;
    uint32_t stack = g_esp, ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp;
    for (int y = first_y; y < last_y; ++y)
        for (int x = first_x; x < last_x; ++x) {
            uint32_t chunk = MEM32(map + 44 + (y * columns + x) * 4);
            if (!chunk) continue;
            PUSH32(g_esp, (uint32_t)((int64_t)y * 16777216 - camera_y));
            PUSH32(g_esp, (uint32_t)((int64_t)x * 16777216 - camera_x));
            PUSH32(g_esp, tiles);
            PUSH32(g_esp, chunk);
            PUSH32(g_esp, 0);
            sub_0043FCE0();
            g_esp = stack;
        }
    g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp;
    g_esp += 4;
    return 1;
}

static void fatal(void) {
    fprintf(stderr, "[renderer] %s\n", gex_renderer_error());
    abort();
}

int gex_graphics_set_render_size(int width, int height) {
    gex_raster *next[2] = {gex_raster_create(width, height), NULL};
    if (next[0]) next[1] = gex_raster_create(width, height);
    if (!next[0] || !next[1]) {
        fprintf(stderr, "[renderer] cannot allocate viewport targets for %dx%d\n", width, height);
        gex_raster_destroy(next[0]); gex_raster_destroy(next[1]);
        return -1;
    }
    int hardware = exclusive_gpu;
    if (hardware && (gex_raster_enable_gpu(next[0], gex_packet_gpu_get()) ||
                     gex_raster_enable_gpu(next[1], gex_packet_gpu_get()))) {
        gex_raster_destroy(next[0]); gex_raster_destroy(next[1]);
        return -1;
    }
    /* Two layers, mirrored draw lists and worst-case 40-byte quads per 32px
     * tile, plus space for objects, text and scene-control packets. */
    uint32_t reserve = 1048576;
#ifdef GEX_NATIVE_GAMEPLAY_WIDTH
    /* Native consoles need the original 0x46DC40..0x47EDB0 arena capacity,
     * not a desktop megabyte reserve, plus the viewport's additional tiles. */
    reserve = 0x11170;
#endif
    uint32_t bytes = ((width + 31) / 32 + 2) * ((height + 47) / 32 + 2) * 160 + reserve;
    uint32_t address = 0;
    void *storage;
#ifdef GEX_NATIVE_GAMEPLAY_WIDTH
    /* Native gameplay fits the original guest arena; retain its existing
     * mapped storage instead of reserving a second copy in 16 MiB RAM. */
    address = 0x46DC40;
    bytes = 0x11170;
    storage = NULL;
#else
    storage = gex_host_guest_alloc(bytes, &address);
#endif
    if (!address || gex_renderer_resize(width, height)) {
        if (!address) fprintf(stderr, "[renderer] cannot allocate viewport packet arena\n");
        else fprintf(stderr, "[renderer] %s\n", gex_renderer_error());
        if (storage)         gex_host_guest_free(storage);
        gex_raster_destroy(next[0]); gex_raster_destroy(next[1]);
        return -1;
    }
    gex_graphics_shutdown();
    targets[0] = next[0]; targets[1] = next[1];
    packet_storage = storage; packet_address = address; packet_bytes = bytes;
    render_width = width; render_height = height;
    exclusive_gpu = hardware;
    if (hardware && gex_packet_gpu_get()->native && gex_renderer_device_mode(1)) {
        fprintf(stderr, "[renderer] %s\n", gex_renderer_error());
        return -1;
    }
    return 0;
}

int gex_graphics_enable_gpu(void) {
    if (exclusive_gpu) return 0;
    const gex_packet_gpu *gpu = gex_packet_gpu_get();
    if (!gpu) {
        fprintf(stderr, "[packet-gpu] this platform has no packet GPU backend\n");
        return -1;
    }
    if (!targets[0] && gex_graphics_set_render_size(320, 224)) return -1;
    if (gex_raster_enable_gpu(targets[0], gpu)) return -1;
    if (gex_raster_enable_gpu(targets[1], gpu)) {
        gex_raster_disable_gpu(targets[0]);
        return -1;
    }
    exclusive_gpu = 1;
    if (gpu->native) {
        for (unsigned plane = 0; plane < 2; ++plane) gex_raster_bind(targets[plane], NULL, plane);
        if (gex_renderer_device_mode(1)) {
            fprintf(stderr, "[renderer] %s\n", gex_renderer_error());
            gex_raster_disable_gpu(targets[0]); gex_raster_disable_gpu(targets[1]);
            exclusive_gpu = 0;
            return -1;
        }
    }
    fprintf(stderr, "[packet-gpu] exclusive packet rendering enabled\n");
    return 0;
}

int gex_graphics_gpu_owned(void) { return gex_raster_gpu_owned(targets[0]); }

static void bind_pictures(void) {
    const uint8_t *atlas = MEM32(0x4A33AC) ? (const uint8_t *)ADDR(MEM32(0x4A33AC)) : NULL;
    if (pictures_bound && bound_atlas == atlas) return;
    for (unsigned i = 0; i < 2; ++i) gex_raster_bind(targets[i], atlas, i);
    bound_atlas = atlas;
    pictures_bound = 1;
}

void gex_graphics_movie(const uint8_t *pixels, int width, int height, int pitch) {
    gex_raster_movie(targets[0], pixels, width, height, pitch);
}

void gex_graphics_fade_capture(void) {
    if (fade_picture || fade_frame.pixels) hle_fatal("screen fade already captured");
    gex_render_frame frame;
    if (gex_renderer_frame(&frame)) hle_fatal("screen fade framebuffer unavailable");
    if (gex_graphics_gpu_owned()) {
        fade_picture = gex_raster_create(render_width, render_height);
        if (!fade_picture || gex_raster_enable_gpu(fade_picture, gex_packet_gpu_get()))
            hle_fatal("cannot allocate screen fade picture");
        gex_raster_copy(fade_picture, targets[0]);
    } else {
        fade_frame = frame;
        fade_frame.pixels = malloc((size_t)frame.pitch * frame.height);
        if (!fade_frame.pixels) hle_fatal("cannot allocate screen fade framebuffer");
        memcpy(fade_frame.pixels, frame.pixels, (size_t)frame.pitch * frame.height);
    }
}

void gex_graphics_fade_present(unsigned shade) {
    if (shade > 255) hle_fatal("invalid screen fade shade");
    if (fade_picture) {
        gex_raster_copy_shaded(targets[0], fade_picture, shade);
        gex_raster_present(targets[0]);
    } else {
        gex_render_frame frame;
        if (!fade_frame.pixels || gex_renderer_frame(&frame) || !frame.pixels ||
            frame.width != fade_frame.width || frame.height != fade_frame.height)
            hle_fatal("screen fade framebuffer changed");
        for (int y = 0; y < frame.height; ++y)
            for (int x = 0; x < frame.width; ++x) {
                const uint8_t *source = fade_frame.pixels + y * fade_frame.pitch + x * 2;
                unsigned value = source[0] | (unsigned)source[1] << 8;
                unsigned shaded = ((value >> 11) * shade / 255) << 11 |
                    (((value >> 5 & 63) * shade / 255) << 5) |
                    ((value & 31) * shade / 255);
                uint8_t *destination = frame.pixels + y * frame.pitch + x * 2;
                destination[0] = (uint8_t)shaded;
                destination[1] = (uint8_t)(shaded >> 8);
            }
        if (gex_renderer_commit()) hle_fatal("screen fade presentation: %s", gex_renderer_error());
    }
}

void gex_graphics_fade_release(void) {
    gex_raster_destroy(fade_picture);
    fade_picture = NULL;
    gex_raster_destroy(shaded_picture);
    shaded_picture = NULL;
    free(fade_frame.pixels);
    memset(&fade_frame, 0, sizeof fade_frame);
}

void gex_graphics_frame_shade(unsigned shade) {
    if (shade > 255) hle_fatal("invalid guest frame shade");
    frame_shade = shade;
}

void gex_graphics_cpu_access(void) {
    if (gex_graphics_gpu_owned()) { bind_pictures(); return; }
    if (!exclusive_gpu || !MEM32(0x4A33AC)) return;
    synchronize(0);
    uint8_t *atlas = (uint8_t *)ADDR(MEM32(0x4A33AC));
    for (int plane = 0; plane < 2; ++plane)
        gex_raster_export(targets[plane], atlas + 0x4000 + plane * 0x78000, 0x800);
}

void gex_graphics_shutdown(void) {
    gex_graphics_fade_release();
    frame_shade = 255;
    pictures_bound = 0;
    bound_atlas = NULL;
    for (int i = 0; i < 2; ++i) {
        gex_raster_destroy(targets[i]); targets[i] = NULL;
    }
    if (gex_renderer_device_mode(0)) hle_fatal("renderer shutdown: %s", gex_renderer_error());
    if (packet_storage) gex_host_guest_free(packet_storage);
    packet_storage = NULL; packet_address = packet_bytes = 0;
    free(backdrops);
    backdrops = NULL;
    backdrop_count = backdrop_capacity = 0;
    backdrop_active = 0;
    backdrop_scene = backdrop_map = 0;
    free(hud_ranges);
    hud_ranges = NULL;
    hud_count = hud_capacity = 0;
    hud_draw.scale = 0;
    fixed_scene = fixed_map = 0;
    fixed_level = 0;
    frame_scene = frame_map = 0;
    transition_active = 0;
    free(camera_barriers);
    camera_barriers = NULL;
    camera_barrier_count = camera_barrier_capacity = 0;
    camera_groups = camera_group_count = 0;
    camera_scene = camera_map = 0;
    world_clip_active = 0;
    world_clip_scene = world_clip_map = 0;
    gex_graphics_scene_end();
    render_width = render_height = 0;
    exclusive_gpu = 0;
}

static void synchronize(int remember) {
    if (gex_graphics_gpu_owned()) { bind_pictures(); return; }
    uint32_t address = MEM32(0x4A33AC);
    if (!targets[0] || !address || (!remember && fixed_screen() && !exclusive_gpu)) return;
    const uint8_t *atlas = (const uint8_t *)ADDR(address);
    for (int i = 0; i < 2; ++i) {
        const uint8_t *picture = atlas + 0x4000 + i * 0x78000;
        if (remember) gex_raster_remember(targets[i], picture, 0x800);
        else gex_raster_sync(targets[i], picture, 0x800);
    }
}

void gex_graphics_pause_begin(void) {
    if (exclusive_gpu) gex_graphics_cpu_access();
    else synchronize(0);
}

void gex_graphics_pause_end(void) {
    if (!targets[0]) return;
    gex_raster_pause(targets[1], targets[0], (const uint8_t *)ADDR(0x4517F0));
    synchronize(1);
}

static int picture_overlap(unsigned offset, unsigned bytes) {
    offset &= 0xfffff;
    while (bytes) {
        unsigned x = offset & 2047, y = offset / 2048;
        unsigned count = bytes < 2048 - x ? bytes : 2048 - x;
        if (x < 640 && ((y >= 8 && y < 232) || (y >= 248 && y < 472))) return 1;
        bytes -= count;
        offset = (offset + count) & 0xfffff;
    }
    return 0;
}

void gex_graphics_atlas_access(uint32_t rectangle) {
    if (!exclusive_gpu) return;
    int x = (int16_t)MEM16(rectangle), y = (int16_t)MEM16(rectangle + 2);
    int width = (int16_t)MEM16(rectangle + 4), height = (int16_t)MEM16(rectangle + 6);
    if (width < 0 || height < 0) {
        gex_graphics_cpu_access();
        return;
    }
    for (int row = 0; row < height; ++row)
        if (picture_overlap(((unsigned)(y + row) * 1024 + (unsigned)x) * 2,
                            (unsigned)width * 2)) {
            gex_graphics_cpu_access();
            return;
        }
}

int gex_graphics_native_operation(uint32_t function) {
    if (!gex_graphics_gpu_owned()) return 0;
    bind_pictures();
    if (function == 0x405240 || function == 0x409FE0) {
        gex_raster_clear(targets[0]);
        if (function == 0x409FE0) MEM32(0x4A295C) = 0;
        return 1;
    }
    if (function == 0x444C90) {
        const gex_packet_gpu *gpu = gex_packet_gpu_get();
        if (gpu->native->atlas_write)
            gpu->native->atlas_write((const uint8_t *)ADDR(MEM32(0x4A33AC)), 0, 0x100000);
        for (unsigned i = 0; i < 2; ++i) gex_raster_clear(targets[i]);
        return 0; /* The remaining initialization resets texture data and tables. */
    }
    if (function == 0x4053A0) {
        gex_graphics_pause_end();
        return 1;
    }
    if (function == 0x405450) {
        const uint8_t *banner = (const uint8_t *)ADDR(0x453510);
        gex_raster_clear(targets[0]);
        gex_raster_blit(targets[0], 80, 101, 160, 21, banner, 320,
                         banner[0] | (unsigned)banner[1] << 8);
        return 1;
    }
    uint32_t rectangle = MEM32(g_esp + 4);
    int x = (int16_t)MEM16(rectangle), y = (int16_t)MEM16(rectangle + 2);
    int width = (int16_t)MEM16(rectangle + 4), height = (int16_t)MEM16(rectangle + 6);
    if (x < 0 || y < 0 || width < 0 || height < 0 ||
        (width && height && (uint64_t)(y + height - 1) * 1024 + x + width > 1024 * 512))
        hle_fatal("invalid GPU atlas rectangle at 0x%08X: %d,%d %dx%d", function, x, y, width, height);
    if (!width || !height) { g_eax = 1; return 1; }
    int overlaps = 0;
    for (int row = 0; row < height && !overlaps; ++row)
        overlaps = picture_overlap((unsigned)(y + row) * 2048 + (unsigned)x * 2, (unsigned)width * 2);
    uint8_t *atlas = (uint8_t *)ADDR(MEM32(0x4A33AC));
    if (!overlaps && function == 0x4451E0) {
        const uint8_t *pixels = (const uint8_t *)ADDR(MEM32(g_esp + 8));
        int equal = 1;
        for (int row = 0; equal && row < height; ++row)
            equal = !memcmp(atlas + (unsigned)(y + row) * 2048 + (unsigned)x * 2,
                            pixels + (size_t)row * width * 2, (size_t)width * 2);
        if (equal) { g_eax = 1; return 1; }
    }
    const gex_packet_gpu *gpu = gex_packet_gpu_get();
    if (gpu->native->atlas_write_rect)
        gpu->native->atlas_write_rect(atlas, (unsigned)y * 2048 + (unsigned)x * 2,
                                      2048, (size_t)width * 2, (unsigned)height);
    else if (gpu->native->atlas_write)
        for (int row = 0; row < height; ++row)
            gpu->native->atlas_write(atlas, (unsigned)(y + row) * 2048 + (unsigned)x * 2,
                                     (size_t)width * 2);
    if (!overlaps) return 0;
    unsigned rgb = 0, color = 0;
    const uint8_t *source = NULL;
    if (function == 0x444D10) {
        rgb = MEM8(g_esp + 8) | (unsigned)MEM8(g_esp + 12) << 8 | (unsigned)MEM8(g_esp + 16) << 16;
        color = (rgb & 255) >> 3 | ((rgb >> 8 & 255) >> 3) << 5 | ((rgb >> 16 & 255) >> 3) << 10;
    } else if (function == 0x4451E0) {
        uint32_t address = MEM32(g_esp + 8);
        unsigned offset = address - MEM32(0x4A33AC);
        if (offset < 0x100000 && picture_overlap(offset, (unsigned)width * height * 2))
            hle_fatal("CPU source references GPU-owned picture in atlas upload");
        source = (const uint8_t *)ADDR(address);
    } else hle_fatal("unknown GPU framebuffer operation 0x%08X", function);
    /* Texture uploads outside picture rectangles remain ordinary CPU asset
     * writes. Picture pixels have no CPU mirror and are composed on device. */
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col) {
            unsigned at = (unsigned)(y + row) * 2048 + (unsigned)(x + col) * 2;
            if (picture_overlap(at, 2)) continue;
            unsigned value = source ? source[(row * width + col) * 2] |
                (unsigned)source[(row * width + col) * 2 + 1] << 8 : color;
            atlas[at] = (uint8_t)value; atlas[at + 1] = (uint8_t)(value >> 8);
        }
    if (x + width > 1024) {
        for (int row = 0; row < height; ++row)
            for (int col = 0; col < width;) {
                int px = (x + col) % 1024, py = y + row + (x + col) / 1024;
                int count = width - col < 1024 - px ? width - col : 1024 - px;
                if (px < 320 && ((py >= 8 && py < 232) || (py >= 248 && py < 472))) {
                    unsigned plane = py >= 248;
                    int pixels = count < 320 - px ? count : 320 - px;
                    if (source)
                        gex_raster_blit(targets[plane], px, py - (int)plane * 240, pixels, 1,
                            source + (row * width + col) * 2, width * 2, -1);
                    else gex_raster_fill(targets[plane], px, py - (int)plane * 240, pixels, 1, rgb);
                }
                col += count;
            }
        g_eax = 1;
        return 1;
    }
    for (unsigned plane = 0; plane < 2; ++plane) {
        int origin = 8 + (int)plane * 240;
        int left = x, right = x + width < 320 ? x + width : 320;
        int top = y > origin ? y : origin;
        int bottom = y + height < origin + 224 ? y + height : origin + 224;
        if (left >= right || top >= bottom) continue;
        if (source)
            gex_raster_blit(targets[plane], left, top - origin + 8, right - left, bottom - top,
                source + ((top - y) * width + left - x) * 2, width * 2, -1);
        else gex_raster_fill(targets[plane], left, top - origin + 8, right - left, bottom - top, rgb);
    }
    g_eax = 1;
    return 1;
}

static int texture_picture_overlap(uint32_t packet, unsigned page, unsigned format) {
    unsigned u = MEM8(packet + 12), v = MEM8(packet + 13);
    unsigned width, height;
    if ((MEM8(packet + 7) & ~2u) == 0x2c) {
        unsigned max_u = u, max_v = v;
        for (unsigned i = 1; i < 4; ++i) {
            unsigned x = MEM8(packet + 12 + i * 8);
            unsigned y = MEM8(packet + 13 + i * 8);
            if (!x && (i & 1)) x = 256;
            if (!y && (i & 2)) y = 256;
            if (x < u) u = x;
            if (y < v) v = y;
            if (x > max_u) max_u = x;
            if (y > max_v) max_v = y;
        }
        /* Mirrored CELs may start one texel later than their UV origin. */
        width = max_u - u + 2;
        height = max_v - v + 2;
    } else {
        int w = (int16_t)MEM16(packet + 16), h = (int16_t)MEM16(packet + 18);
        if (w <= 0 || h <= 0) return 0;
        width = (unsigned)w; height = (unsigned)h;
    }
    if (width > 256) width = 256;
    if (height > 256) height = 256;
    unsigned base = (page & 15) * 128 + (page & 16 ? 0x80000 : 0);
    for (unsigned row = 0; row < height; ++row)
        for (unsigned at = 0; at < width;) {
            unsigned x = (u + at) & 255;
            unsigned count = width - at;
            if (count > 256 - x) count = 256 - x;
            unsigned offset = format == 0 ? x / 2 : format == 1 ? x : x * 2;
            unsigned bytes = format == 0 ? (x + count + 1) / 2 - x / 2 :
                             format == 1 ? count : count * 2;
            if (picture_overlap(base + ((v + row) & 255) * 2048 + offset, bytes)) return 1;
            at += count;
        }
    return 0;
}

int gex_graphics_draw_packet(uint32_t packet) {
    if (!targets[0] || MEM32(0x451040) || !MEM32(0x4A33AC)) return 0;
    unsigned opcode = MEM8(packet + 7);
    if (gex_graphics_gpu_owned()) bind_pictures();
    if (exclusive_gpu && (opcode == 0xe1 || (opcode & ~2u) == 0x2c)) {
        unsigned page = opcode == 0xe1 ? MEM32(packet + 4) & 0x1ff : MEM16(packet + 22);
        MEM32(0x4A2F78) = MEM32(0x4A33AC) + (page & 15) * 128 +
                          ((page & 16) ? 0x80000 : 0);
        MEM8(0x4A2F90) = (uint8_t)((page >> 7) & 3);
        if (opcode == 0xe1) {
            MEM8(0x4A33A0) = (page & 0x30) ? 0x30 : 0;
            return 1;
        }
    }
    if (exclusive_gpu && !gex_graphics_gpu_owned() &&
        ((opcode & ~2u) == 0x2c || (opcode & ~2u) == 0x64)) {
        unsigned clut = MEM16(packet + 14) * 32;
        unsigned format = MEM8(0x4A2F90);
        unsigned page = (opcode & ~2u) == 0x2c ? MEM16(packet + 22) : MEM32(0x4A2F78) - MEM32(0x4A33AC);
        if ((opcode & ~2u) != 0x2c)
            page = (page & 0x7ff) / 128 | (page & 0x80000 ? 16 : 0);
        if (texture_picture_overlap(packet, page, format) ||
            (format < 2 && picture_overlap(clut, format == 0 ? 32 : 512))) {
            gex_graphics_cpu_access();
        }
    }
    int origin = MEM32(0x450010) >= 240 ? 240 : 0;
    gex_raster_state state = {
        (const uint8_t *)ADDR(MEM32(0x4A33AC)),
        (const uint8_t *)ADDR(0x4A2B50),
        (MEM8(0x4A2F90) << 7), (int32_t)MEM32(0x450008),
        (int32_t)MEM32(0x450010), (int32_t)MEM32(0x45000C),
        (int32_t)MEM32(0x450014), origin, fixed_screen() ? 1 : 0
    };
    if (state.layout && !transition_active && fixed_level != 66)
        for (size_t i = 0; i < backdrop_count; ++i)
            if (backdrops[i].begin <= backdrops[i].end ?
                packet >= backdrops[i].begin && packet < backdrops[i].end :
                packet >= packet_address && packet < packet_address + packet_bytes &&
                (packet >= backdrops[i].begin || packet < backdrops[i].end)) {
                state.layout = 2;
                break;
            }
    int hud = 0;
    for (size_t i = 0; i < hud_count; ++i)
        if (hud_ranges[i].begin <= hud_ranges[i].end ?
            packet >= hud_ranges[i].begin && packet < hud_ranges[i].end :
            packet >= packet_address && packet < packet_address + packet_bytes &&
            (packet >= hud_ranges[i].begin || packet < hud_ranges[i].end)) {
            state.layout = 0;
            state.pixel_scale = hud_ranges[i].scale;
            state.offset_x = hud_ranges[i].offset_x;
            hud = 1;
            break;
        }
    uint32_t scene = MEM32(0x4A2990);
    if (!state.layout && !hud && world_clip_active && scene == world_clip_scene &&
        scene && MEM32(scene + 4) == world_clip_map) {
        state.world_clip = 1;
        state.world_left = world_clip[0]; state.world_top = world_clip[1];
        state.world_right = world_clip[2]; state.world_bottom = world_clip[3];
    }
    /* Sprite packets inherit the texture page; quad packets carry their own. */
    uint32_t offset = MEM32(0x4A2F78) - MEM32(0x4A33AC);
    state.page |= ((offset & 0x7FFFF) / 128) & 15;
    if (offset & 0x80000) state.page |= 16;
    gex_raster_packet(targets[origin / 240], (const uint8_t *)ADDR(packet), &state);
    return exclusive_gpu;
}

void gex_graphics_end_frame(void) {
    gex_graphics_cpu_access();
    if (!exclusive_gpu) synchronize(1);
    /* Cached backdrop CELs outlive a frame; retire them with their scene/arena. */
    hud_count = 0;
}

void gex_graphics_format(void) {
    g_eax = 0x565;
    g_esp += 4;
}

void gex_graphics_create(void) {
    MEM32(0x451030) = 0;
    MEM32(0x454FC8) = MEM32(0x49FB8C) = 0x565;
    g_eax = 1;
    g_esp += 4;
}

void gex_graphics_destroy(void) {
    MEM32(0x451030) = 0;
    g_esp += 4;
}

void gex_graphics_resolution(void) {
    uint32_t mode = MEM32(g_esp + 4);
    if (mode != 0 && mode != 0x320240 && mode != 0x640480) {
        fprintf(stderr, "[renderer] unsupported original resolution 0x%X\n", mode);
        g_eax = 0;
    } else {
        if (gex_renderer_resize(render_width ? render_width : mode == 0x640480 ? 640 : 320,
                                render_height ? render_height : mode == 0x640480 ? 480 : 240)) fatal();
        MEM32(0x451038) = mode;
        MEM32(0x454FC8) = 0x565;
        g_eax = 1;
    }
    g_esp += 4;
}

void gex_graphics_begin_frame(void) {
    if (targets[0] && !MEM32(0x451040) && MEM32(0x4A294C) != 2) {
        uint32_t scene = MEM32(0x4A2990);
        uint32_t map = scene ? MEM32(scene + 4) : 0;
        /* Fitted scenes may never draw their borders. Retire the previous
         * scene's pixels once, without clearing incremental draws or fades. */
        if (fixed_screen() && (scene != frame_scene || map != frame_map))
            for (int i = 0; i < 2; ++i) gex_raster_clear(targets[i]);
        frame_scene = scene;
        frame_map = map;
    }
    synchronize(0);
    /* The level-name scene's masked static tiles leave their black texels
     * untouched. Start this scene on black so old Gex poses cannot show through.
     * Other scenes retain their framebuffer for fades and incremental drawing. */
    if (!MEM32(0x4A2958) || MEM32(0x4A2990) != MEM32(0x455B84) ||
        MEM32(0x451040) || MEM32(0x4A294C) == 2)
        return;
    uint32_t address = MEM32(0x4A33AC);
    if (address) {
        uint8_t *pixels = (uint8_t *)ADDR(address);
        if (!gex_graphics_gpu_owned()) {
            for (unsigned y = 8; y < 232; ++y)
                memset(pixels + y * 0x800, 0, 640);
            if (targets[0]) gex_raster_remember(targets[0], pixels + 0x4000, 0x800);
        }
        if (targets[0]) gex_raster_clear(targets[0]);
    }
}

void gex_graphics_flush(void) {
    if (!MEM32(0x451040)) {
        ++MEM32(0x487A90);
        uint32_t address = MEM32(0x487F70);
        if (address) {
            uint8_t *pixels = (uint8_t *)ADDR(address);
            if (targets[0]) {
                if (gex_graphics_gpu_owned()) {
                    int plane = MEM32(0x4A294C) == 2 ? 1 : 0;
                    if (plane) gex_raster_copy(targets[0], targets[1]);
                    gex_raster *picture = targets[plane];
                    if (frame_shade < 255) {
                        if (!shaded_picture) {
                            shaded_picture = gex_raster_create(render_width, render_height);
                            if (!shaded_picture ||
                                gex_raster_enable_gpu(shaded_picture, gex_packet_gpu_get()))
                                hle_fatal("cannot allocate shaded guest picture");
                        }
                        gex_raster_copy_shaded(shaded_picture, picture, frame_shade);
                        picture = shaded_picture;
                    }
                    gex_raster_present(picture);
                    g_esp += 4;
                    return;
                }
                if (exclusive_gpu) gex_graphics_cpu_access();
                else synchronize(0);
                int plane = MEM32(0x4A294C) == 2 ? 1 : 0;
                if (plane) {
                    gex_raster_copy(targets[0], targets[1]);
                    for (unsigned y = 0; y < 224; ++y)
                        memcpy(pixels + 0x4000 + y * 0x800,
                               pixels + 0x7C000 + y * 0x800, 640);
                    gex_raster_remember(targets[0], pixels + 0x4000, 0x800);
                }
                if (gex_renderer_upload_bgr555_shaded(gex_raster_pixels(targets[plane]),
                        render_width, render_height, render_width * 2, frame_shade)) fatal();
                g_esp += 4;
                return;
            }
            /* The pause screen is stored in the atlas's second 240-row plane. */
            if (MEM32(0x4A294C) == 2)
                for (unsigned y = 0; y < 224; ++y)
                    memcpy(pixels + 0x4000 + y * 0x800,
                           pixels + 0x7C000 + y * 0x800, 640);
            /* Present the active picture, not the eight padding rows on each side. */
            if (gex_renderer_upload_bgr555_shaded(pixels + 0x4000, 320, 224, 0x800,
                                                  frame_shade)) fatal();
        }
    }
    g_esp += 4;
}

void gex_graphics_forbidden(void) {
    fprintf(stderr, "[renderer] obsolete DirectDraw entry reached at 0x%08X\n", g_cur_func);
    abort();
}
