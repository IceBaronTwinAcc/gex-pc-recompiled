#include <kos.h>
#include <dc/pvr.h>
#include <dc/asic.h>
#include <dc/sq.h>
#include <dc/vblank.h>
#include <math.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "packet_gpu.h"
#include "raster.h"
#include "sampling.h"
#include "title_cel.h"
#include "title_draw.h"
#include "map_draw.h"

/* A single presorted translucent list preserves original packet order. */
enum { TW = 512, TH = 256, CACHE_COUNT = 24, CACHE_BYTES = 4 * 1024 * 1024,
       COMMAND_BYTES = 65536, COMMAND_MAX_BYTES = 196608,
       BACKGROUND_POLYGONS = 384, BACKGROUND_MINIMUM = 16,
       REMAP_COUNT = 256, REMAP_BYTES = 512 * 1024, SNAPSHOT_BYTES = 512 * 1024,
       TITLE_SNAPSHOT_BYTES = 2 * 1024 * 1024,
       SHARED_GROUPS = 2,
       WARM_REGIONS = 4, WARM_BYTES = 128 * 128 };
enum { BACKGROUND_IDLE, BACKGROUND_RECORD, BACKGROUND_REUSE, BACKGROUND_VERIFY,
       BACKGROUND_REDRAW };
typedef struct {
    unsigned begin, end, top, bottom;
} texture_read;
typedef struct {
    uint8_t packet[36];
    const uint8_t *atlas, *modulation;
    int state[16];
    unsigned valid;
} packet_key;
typedef struct {
    gex_gpu_vertex vertices[4];
    int clip[4];
    gex_gpu_material material;
    packet_key key;
    float prepared[8];
    uint16_t region[4];
    unsigned prepared_valid, sample_scale;
} background_polygon;
typedef struct target {
    pvr_ptr_t image[2];
    uint16_t *history;
    size_t history_bytes;
    unsigned current;
    const uint8_t *atlas;
    unsigned plane;
    int owned;
    background_polygon *background;
    unsigned background_count, background_at;
    int background_mode, background_valid;
    unsigned background_misses;
    unsigned pal4_draws;
    unsigned warm_at, warm_budget, warm_bytes;
    gex_gpu_material warm_material;
    uint16_t warm_region[4];
    unsigned warm_pending;
    unsigned reclaim_budget;
    uint64_t warm_previous, reclaim_cutoff;
    uint64_t background_revision;
    packet_key pending_key;
    uint32_t dependencies[512];
    uint8_t *saved_commands;
    size_t saved_size;
    pvr_poly_hdr_t saved_header;
    int saved_replaces, saved_clear_only;
    struct transient_texture **saved_transients;
    size_t saved_transient_count;
    texture_read saved_reads[CACHE_COUNT];
    uint8_t saved_remaps[REMAP_COUNT];
    int known_clear;
#if GEX_PROFILE_RENDERER
    uint64_t draw_cpu_begin;
#endif
    struct target *next;
} target;
static pvr_ptr_t current_image(const target *t) {
    return t->image[t->current];
}
typedef struct {
    unsigned page, rgb, opcode, raw;
    size_t bytes, source_bytes;
    uint8_t *source;
    pvr_ptr_t image[3];
    int valid;
    uint64_t used;
    size_t storage;
    const uint8_t *verified_atlas;
    uint32_t dirty[256];
    unsigned region_begin, region_end, region_top, region_bottom;
    unsigned read_begin, read_end, read_top, read_bottom;
    pvr_poly_hdr_t header[3];
    int header_format[3], header_valid[3];
} texture;
typedef struct {
    unsigned page, clut;
    uint16_t origin_u, origin_v, dimension, next;
    uint8_t group, valid, alpha, queued;
    int header_format;
    uint64_t used;
    const uint8_t *verified_atlas;
    pvr_ptr_t image;
    pvr_poly_hdr_t header;
    uint8_t index_map[16];
    uint16_t source_indices;
} remap_texture;
typedef struct transient_texture {
    pvr_ptr_t image[2];
    const uint8_t *atlas;
    uint64_t epoch, used, title_id;
    size_t retained_bytes;
    unsigned page, clut, alpha, origin_u, origin_v, dimension;
    uint32_t zero_mask[8];
    unsigned current, saved_refs;
    uint16_t *direct_pixels;
    size_t direct_count;
    unsigned direct_width, direct_height, direct_passes;
    uint32_t direct_hash;
    struct transient_texture *next;
    struct transient_texture *title_next;
    struct transient_texture *snapshot_next;
    unsigned snapshot_bucket;
    pvr_poly_hdr_t image_header[2];
    unsigned image_header_valid[2];
} transient_texture;
typedef struct transient_pin {
    transient_texture *texture;
    struct transient_pin *previous;
} transient_pin;
typedef struct {
    texture *page;
    remap_texture *region;
    transient_texture *converted;
} draw_texture;

static texture cache[CACHE_COUNT];
static unsigned cache_hint;
static remap_texture remaps[REMAP_COUNT];
static uint16_t remap_heads[256];
static unsigned remap_hash(unsigned page, unsigned clut, unsigned alpha) {
    return ((clut ^ (clut >> 8)) * 33 ^ page * 17 ^ alpha * 97) & 255;
}
static size_t remap_bytes;
static struct {
    uint32_t colors[256];
    uint32_t occupied[8];
    uint16_t lookup[512];
    unsigned count, written;
    int bank;
} shared_groups[SHARED_GROUPS];
static int shared_dirty;
static unsigned users;
static uint64_t cache_clock;
static size_t cache_bytes;
static target *active;
static target *targets;
static uint8_t *commands;
static size_t command_size, command_capacity;
static int command_replaces, command_clear_only;
static unsigned saved_count;
static texture_read current_reads[CACHE_COUNT];
static uint8_t current_remaps[REMAP_COUNT];
static pvr_poly_hdr_t last_header;
static uint32_t palettes[1024];
static uint32_t programmed_palettes[32];
static struct {
    const uint8_t *atlas;
    unsigned offset, count, alpha;
    uint32_t zero_mask[8];
    int valid;
} palette_keys[64];
static struct {
    unsigned source, alpha;
    int valid;
    uint32_t colors[16];
    unsigned mapped;
    unsigned blocked;
    unsigned group;
    uint8_t index_map[16];
} small_palettes[256];
static unsigned palette_used;
static uint32_t palette_banks[2];
static target *pictures[2];
static const uint8_t *tracked_atlas;
static uint64_t atlas_revision;
static uint64_t atlas_write_epoch;
static size_t retained_snapshot_bytes;
static size_t retained_title_bytes;
static transient_texture *transients;
static transient_texture *title_textures[256];
static transient_texture *snapshot_textures[256];
static transient_pin *active_transient_pin;
static unsigned previous_scheduler_hz;
static target *last_presented;
static unsigned last_present_vblank;
static int presentation_paced;
static int fail(char *error, size_t size, const char *message);
static int transient_pinned(const transient_texture *t) {
    for (transient_pin *pin = active_transient_pin; pin; pin = pin->previous)
        if (pin->texture == t) return 1;
    return 0;
}

static unsigned title_texture_bucket(uint64_t id) {
    uint32_t key = (uint32_t)id ^ (uint32_t)(id >> 32);
    return (key * UINT32_C(2654435761)) >> 24;
}

static unsigned snapshot_texture_bucket(unsigned page, unsigned clut, unsigned alpha,
                                        unsigned left, unsigned top, unsigned dimension) {
    uint32_t key = page * 33u ^ clut * 65599u ^ alpha * 97u ^
                   left * 257u ^ top * 4099u ^ dimension * 17u;
    return (key * UINT32_C(2654435761)) >> 24;
}

static void free_transient(transient_texture **link) {
    transient_texture *t = *link;
    *link = t->next;
    if (t->title_id) {
        transient_texture **entry = &title_textures[title_texture_bucket(t->title_id)];
        while (*entry != t) entry = &(*entry)->title_next;
        *entry = t->title_next;
    }
    if (t->snapshot_bucket) {
        transient_texture **entry = &snapshot_textures[t->snapshot_bucket - 1];
        while (*entry != t) entry = &(*entry)->snapshot_next;
        *entry = t->snapshot_next;
    }
    retained_snapshot_bytes -= t->retained_bytes;
    if (t->title_id) retained_title_bytes -= t->retained_bytes;
    cache_bytes -= t->retained_bytes;
    for (unsigned i = 0; i < 2; ++i)
        if (t->image[i]) pvr_mem_free(t->image[i]);
    free(t->direct_pixels);
    free(t);
}

static transient_texture **oldest_idle_snapshot_kind(int title) {
    transient_texture **oldest = NULL;
    for (transient_texture **link = &transients; *link; link = &(*link)->next) {
        transient_texture *t = *link;
        if (!t->retained_bytes || t->current || t->saved_refs || transient_pinned(t))
            continue;
        if (title >= 0 && !!t->title_id != title) continue;
        if (!oldest || t->used < (*oldest)->used) oldest = link;
    }
    return oldest;
}

static transient_texture **oldest_idle_snapshot(void) {
    return oldest_idle_snapshot_kind(-1);
}

static void release_transients(void) {
    transient_texture **link = &transients;
    while (*link) {
        transient_texture *t = *link;
        if (transient_pinned(t) || t->current || t->saved_refs ||
            (t->retained_bytes &&
             ((t->direct_pixels || t->title_id) ? users != 0 : t->epoch == atlas_write_epoch))) {
            link = &t->next;
            continue;
        }
        free_transient(link);
    }
}
static void clear_current_transients(void) {
    for (transient_texture *t = transients; t; t = t->next) t->current = 0;
}

static int capture_transients(transient_texture ***out, size_t *count,
                              char *error, size_t size) {
    size_t n = 0;
    for (transient_texture *t = transients; t; t = t->next) n += !!t->current;
    transient_texture **items = n ? malloc(n * sizeof *items) : NULL;
    if (n && !items) return fail(error, size, "cannot retain PVR snapshot textures");
    size_t i = 0;
    for (transient_texture *t = transients; t; t = t->next)
        if (t->current) {
            items[i++] = t;
            ++t->saved_refs;
        }
    *out = items;
    *count = n;
    return 0;
}

static int clone_saved_transients(const target *source, transient_texture ***out,
                                  char *error, size_t size) {
    size_t count = source->saved_transient_count;
    transient_texture **items = count ? malloc(count * sizeof *items) : NULL;
    if (count && !items) return fail(error, size, "cannot retain PVR snapshot textures");
    for (size_t i = 0; i < count; ++i) {
        items[i] = source->saved_transients[i];
        ++items[i]->saved_refs;
    }
    *out = items;
    return 0;
}

static void release_saved_transients(target *t, int restore) {
    for (size_t i = 0; i < t->saved_transient_count; ++i) {
        transient_texture *texture = t->saved_transients[i];
        if (restore) texture->current = 1;
        --texture->saved_refs;
    }
    free(t->saved_transients);
    t->saved_transients = NULL;
    t->saved_transient_count = 0;
}
static int display_wait(uint32_t address, char *error, size_t size) {
    uint64_t deadline = timer_us_gettime64() + 100000;
    while (PVR_GET(PVR_FB_ADDR) != address) {
        if (timer_us_gettime64() >= deadline)
            return fail(error, size, "PVR display page flip timed out");
        thd_sleep(1);
    }
    return 0;
}
#if GEX_PROFILE_RENDERER
static struct native_cpu {
    uint64_t clear, atlas, packet, polygon;
} native_draw_cpu;
#define NATIVE_DRAW_BEGIN() uint64_t native_draw_begin = thd_get_cpu_time(thd_get_current())
#define NATIVE_DRAW_END(field) do { \
    uint64_t cpu = thd_get_cpu_time(thd_get_current()); \
    if (cpu >= native_draw_begin) native_draw_cpu.field += cpu - native_draw_begin; \
} while (0)
static struct pvr_work {
    unsigned polygons, prepared, reused, headers, same_headers;
    unsigned palette_entries, same_palette_entries, remap_lookups, remap_checks;
    unsigned resolves, preservation, captures, snapshots, writes, refresh_bytes;
    unsigned allocation_retries;
    unsigned page_builds, remap_builds;
    unsigned page_lookups, page_checks;
    unsigned palette_drains, texture_refreshes;
    unsigned pal4_calls, pal8_calls, pal8_drains;
    unsigned remap_bounds, remap_hits, remap_budget, remap_colors, remap_queued, remap_reclaims;
    size_t compared_bytes, discarded_bytes;
    uint64_t prepare_cpu, prepare_time, texture_cpu, texture_time;
    uint64_t remap_cpu, remap_time, flush_cpu, flush_time, capture_cpu, capture_time;
    uint64_t packet_cpu, packet_time, polygon_cpu, polygon_time;
    uint64_t atlas_cpu, atlas_time;
    unsigned display_samples, display_late, display_missed;
    uint64_t display_sum, display_max;
} work;
struct frame_work {
    unsigned polygons, remaps, pages, refresh, drains;
};
static size_t display_seen;
static uint64_t display_last_us;
static int profile_display(char *error, size_t size) {
    pvr_stats_t stats;
    if (pvr_get_stats(&stats)) return fail(error, size, "PVR display statistics failed");
    size_t frames = stats.frame_count;
    uint64_t now = timer_us_gettime64();
    uint64_t interval = (now - display_last_us) * 1000;
    if (frames != display_seen) {
        if (display_seen) {
            unsigned skipped = frames - display_seen - 1;
            work.display_missed += skipped;
            ++work.display_samples;
            work.display_sum += interval;
            if (interval > work.display_max) work.display_max = interval;
            work.display_late += interval > UINT64_C(40000000);
        }
        display_seen = frames;
        display_last_us = now;
    }
    return 0;
}
static struct {
    unsigned frame, samples, over_budget;
    uint64_t last_time, last_cpu, interval_sum, cpu_sum;
    uint64_t cpu_max, cpu_max_draw, cpu_max_native, intervals[30];
    struct native_cpu last_native, max_native;
    struct frame_work last_work, max_work;
} timing;
static struct {
    unsigned end, over_budget;
    uint64_t mean, p95, maximum, cpu, cpu_max, cpu_max_draw, cpu_max_native;
    struct native_cpu max_native;
    struct frame_work max_work;
    struct pvr_work work;
} reports[128];
static unsigned report_count, dropped_reports;
static int report_registered;
static void report_profile(void) {
    for (unsigned i = 0; i < report_count; ++i) {
        const struct pvr_work *w = &reports[i].work;
        fprintf(stderr, "[pvr-work] end=%u frames=30 dt_mean=%llu dt_p95=%llu dt_max=%llu "
                "cpu_mean_ns=%llu cpu_max_ns=%llu cpu_max_draw_ns=%llu cpu_max_native_ns=%llu "
                "cpu_max_clear_ns=%llu cpu_max_atlas_ns=%llu cpu_max_packet_ns=%llu cpu_max_polygon_ns=%llu "
                "cpu_max_polygons=%u cpu_max_remaps=%u cpu_max_pages=%u cpu_max_refresh=%u cpu_max_drains=%u "
                "over_budget=%u polygons=%u prepared=%u reused=%u headers=%u same_headers=%u "
                "palette=%u same_palette=%u remap_lookups=%u remap_checks=%u resolves=%u preserve=%u "
                "captures=%u snapshots=%u writes=%u alloc_retries=%u compared=%lu discarded=%lu "
                "page_builds=%u remap_builds=%u page_lookups=%u page_checks=%u refresh=%u "
                "prepare_cpu=%llu texture_cpu=%llu remap_cpu=%llu flush_cpu=%llu "
                "flush_us=%llu capture_cpu=%llu capture_us=%llu packet_cpu=%llu polygon_cpu=%llu "
                "palette_drains=%u texture_refreshes=%u remap_bounds=%u remap_hits=%u "
                "remap_budget=%u remap_colors=%u remap_queued=%u atlas_cpu=%llu "
                "pal4_calls=%u pal8_calls=%u pal8_drains=%u remap_reclaims=%u "
                "display_samples=%u display_mean_us=%llu display_max_us=%llu "
                "display_late=%u display_missed=%u\n",
                reports[i].end, (unsigned long long)reports[i].mean,
                (unsigned long long)reports[i].p95, (unsigned long long)reports[i].maximum,
                (unsigned long long)reports[i].cpu,
                (unsigned long long)reports[i].cpu_max,
                (unsigned long long)reports[i].cpu_max_draw,
                (unsigned long long)reports[i].cpu_max_native,
                (unsigned long long)reports[i].max_native.clear,
                (unsigned long long)reports[i].max_native.atlas,
                (unsigned long long)reports[i].max_native.packet,
                (unsigned long long)reports[i].max_native.polygon,
                reports[i].max_work.polygons, reports[i].max_work.remaps,
                reports[i].max_work.pages, reports[i].max_work.refresh,
                reports[i].max_work.drains, reports[i].over_budget,
                w->polygons, w->prepared, w->reused, w->headers,
                w->same_headers, w->palette_entries, w->same_palette_entries,
                w->remap_lookups, w->remap_checks, w->resolves, w->preservation,
                w->captures, w->snapshots, w->writes, w->allocation_retries,
                (unsigned long)w->compared_bytes,
                (unsigned long)w->discarded_bytes, w->page_builds, w->remap_builds,
                w->page_lookups, w->page_checks,
                w->refresh_bytes, (unsigned long long)w->prepare_cpu,
                (unsigned long long)w->texture_cpu, (unsigned long long)w->remap_cpu,
                (unsigned long long)w->flush_cpu, (unsigned long long)w->flush_time,
                (unsigned long long)w->capture_cpu, (unsigned long long)w->capture_time,
                (unsigned long long)w->packet_cpu, (unsigned long long)w->polygon_cpu,
                w->palette_drains, w->texture_refreshes, w->remap_bounds, w->remap_hits,
                w->remap_budget, w->remap_colors, w->remap_queued,
                (unsigned long long)w->atlas_cpu, w->pal4_calls, w->pal8_calls, w->pal8_drains,
                w->remap_reclaims, w->display_samples,
                (unsigned long long)(w->display_samples ? w->display_sum / w->display_samples / 1000 : 0),
                (unsigned long long)(w->display_max / 1000), w->display_late, w->display_missed);
    }
    if (dropped_reports)
        fprintf(stderr, "[pvr-work] truncated: %u windows exceeded diagnostic storage\n",
                dropped_reports);
}
#define WORK(field, value) (work.field += (value))
#define PROFILE_BEGIN() \
    uint64_t profile_begin = timer_us_gettime64(); \
    uint64_t profile_cpu_begin = thd_get_cpu_time(thd_get_current())
#define PROFILE_END(field) do { \
    uint64_t cpu = thd_get_cpu_time(thd_get_current()); \
    work.field##_time += timer_us_gettime64() - profile_begin; \
    if (cpu >= profile_cpu_begin) work.field##_cpu += cpu - profile_cpu_begin; \
} while (0)
#else
#define NATIVE_DRAW_BEGIN() ((void)0)
#define NATIVE_DRAW_END(field) ((void)0)
#define WORK(field, value) ((void)0)
#define PROFILE_BEGIN() ((void)0)
#define PROFILE_END(field) ((void)0)
#endif
static void program_palette(unsigned offset, const uint32_t *colors, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        unsigned at = offset + i;
        uint32_t bit = UINT32_C(1) << (at & 31);
        if ((programmed_palettes[at / 32] & bit) && palettes[at] == colors[i]) {
            WORK(same_palette_entries, 1);
            continue;
        }
        pvr_set_pal_entry(at, colors[i]);
        palettes[at] = colors[i];
        programmed_palettes[at / 32] |= bit;
    }
    WORK(palette_entries, count);
}
static int native_polygon_draw(void *data, const gex_gpu_vertex input[4],
                               int left, int top, int right, int bottom,
                               unsigned sample_scale, const gex_gpu_material *m, char *error, size_t size);
static int background_replay(target *t, char *error, size_t size);
static int background_stop(target *t, char *error, size_t size);
static int background_advance(target *t, char *error, size_t size);
static int prune_shared_colors(char *error, size_t size);

static int interval_overlap(uint64_t begin, uint64_t bytes, uint64_t offset,
                            size_t pitch, size_t row_bytes, unsigned rows) {
    if (!rows || !row_bytes || begin + bytes <= offset) return 0;
    uint64_t row = pitch && begin >= offset + row_bytes ?
                   (begin - offset - row_bytes) / pitch + 1 : 0;
    return row < rows && offset + row * pitch < begin + bytes;
}

static int atlas_rows_overlap(unsigned top, unsigned rows, unsigned source, unsigned height) {
    if (rows >= 512) return 1;
    unsigned bottom = top + rows;
    return bottom <= 512 ? source < bottom && top < source + height :
                          source + height > top || source < bottom - 512;
}

static int atlas_rect_write_overlap(unsigned begin, unsigned width, unsigned height,
                                    unsigned offset, unsigned bytes, unsigned rows) {
    unsigned left = offset & 2047, top = (offset & 0xfffff) / 2048;
    unsigned first = bytes < 2048 - left ? bytes : 2048 - left;
    unsigned u = begin & 2047, v = begin / 2048;
    return (u < left + first && left < u + width &&
            atlas_rows_overlap(top, rows, v, height)) ||
           (bytes > first && u < bytes - first &&
            atlas_rows_overlap((top + 1) & 511, rows, v, height));
}

static int write_overlap(unsigned begin, size_t bytes, unsigned offset,
                         size_t pitch, size_t row_bytes, unsigned rows) {
    offset &= 0xfffff;
    /* CLUT tests at atlas pitch need no SH-4 64-bit division. */
    if (pitch == 2048 && bytes && bytes <= 2048 && row_bytes &&
        row_bytes <= 2048 && rows && begin < 0x100000) {
        unsigned first = bytes < 2048 - (begin & 2047) ? bytes : 2048 - (begin & 2047);
        return atlas_rect_write_overlap(begin, first, 1, offset, (unsigned)row_bytes, rows) ||
               (bytes > first &&
                atlas_rect_write_overlap((begin + first) & 0xfffff, (unsigned)bytes - first,
                                         1, offset, (unsigned)row_bytes, rows));
    }
    return interval_overlap(begin, bytes, offset, pitch, row_bytes, rows) ||
           interval_overlap((uint64_t)begin + 0x100000, bytes, offset, pitch, row_bytes, rows) ||
           interval_overlap(begin, bytes, (uint64_t)offset + 0x100000, pitch, row_bytes, rows);
}

static int remap_write_overlap(const remap_texture *t, unsigned offset,
                               size_t pitch, size_t bytes, unsigned rows) {
    unsigned base = (t->page & 15) * 128 + (t->page & 16 ? 0x80000 : 0);
    unsigned begin = base + t->origin_v * 2048 + t->origin_u / 2;
    /* An odd starting nibble makes the final texel occupy an extra byte. */
    unsigned width = (t->dimension + (t->origin_u & 1) + 1) / 2;
    if (pitch == 2048 && bytes <= 2048) {
        /* Atlas rows wrap at 1 MiB; a right-edge write can touch the next
         * row as well. Remapped regions themselves stay inside the page. */
        return atlas_rect_write_overlap(begin, width, t->dimension,
                                        offset, (unsigned)bytes, rows);
    }
    for (unsigned row = 0; row < rows; ++row)
        if (write_overlap((offset + (uint64_t)row * pitch) & 0xfffff, bytes,
                          begin, 2048, width, t->dimension)) return 1;
    return 0;
}

static int snapshot_segment_overlap(unsigned begin, unsigned width, unsigned height,
                                    unsigned offset, unsigned bytes, unsigned rows) {
    unsigned first = height < 512 - begin / 2048 ? height : 512 - begin / 2048;
    return atlas_rect_write_overlap(begin, width, first, offset, bytes, rows) ||
           (height > first &&
            atlas_rect_write_overlap(begin & 2047, width, height - first,
                                     offset, bytes, rows));
}

static int snapshot_range_overlap(unsigned begin, unsigned width, unsigned offset, unsigned bytes) {
    return (begin < offset + bytes && offset < begin + width) ||
           (begin + 0x100000 < offset + bytes && offset < begin + 0x100000 + width) ||
           (begin < offset + 0x100000 + bytes && offset + 0x100000 < begin + width);
}

static int snapshot_write_overlap(const transient_texture *t, unsigned offset,
                                  size_t pitch, size_t bytes, unsigned rows) {
    if (!bytes || !rows) return 0;
    if (bytes >= 0x100000) return 1;
    unsigned format = t->page >> 7 & 3;
    unsigned base = (t->page & 15) * 128 + (t->page & 16 ? 0x80000 : 0);
    unsigned column = format == 0 ? t->origin_u / 2 :
                      format == 1 ? t->origin_u : t->origin_u * 2;
    unsigned width = format == 0 ? (t->dimension + (t->origin_u & 1) + 1) / 2 :
                     format == 1 ? t->dimension : t->dimension * 2;
    unsigned begin = (base + t->origin_v * 2048 + column) & 0xfffff;
    unsigned clut = (t->clut * 32) & 0xfffff;
    unsigned clut_bytes = format == 0 ? 32 : format == 1 ? 512 : 0;
    if (pitch == 2048 && bytes <= 2048) {
        unsigned first = width < 2048 - (begin & 2047) ? width : 2048 - (begin & 2047);
        return (clut_bytes && write_overlap(clut, clut_bytes, offset, pitch, bytes, rows)) ||
               snapshot_segment_overlap(begin, first, t->dimension, offset, (unsigned)bytes, rows) ||
               (width > first &&
                snapshot_segment_overlap((begin + first) & 0xfffff, width - first, t->dimension,
                                         offset, (unsigned)bytes, rows));
    }
    for (unsigned y = 0; y < rows; ++y) {
        unsigned at = (offset + (uint64_t)y * pitch) & 0xfffff;
        if (clut_bytes && snapshot_range_overlap(clut, clut_bytes, at, (unsigned)bytes))
            return 1;
        for (unsigned source_y = 0; source_y < t->dimension; ++source_y)
            if (snapshot_range_overlap((begin + source_y * 2048) & 0xfffff, width,
                                       at, (unsigned)bytes)) return 1;
    }
    return 0;
}

static void dependency_range(target *t, unsigned offset, unsigned bytes) {
    while (bytes) {
        unsigned at = offset & 0xfffff;
        unsigned block = at / 64;
        t->dependencies[block / 32] |= UINT32_C(1) << (block & 31);
        unsigned count = 64 - (at & 63);
        if (count > bytes) count = bytes;
        bytes -= count;
        offset += count;
}
    }

static int background_affected(const target *t, unsigned offset, size_t pitch,
                               size_t bytes, unsigned rows) {
    for (unsigned row = 0; row < rows; ++row) {
        uint64_t begin = (offset + (uint64_t)row * pitch) & 0xfffff;
        uint64_t end = begin + bytes;
        if (bytes >= 0x100000) { begin = 0; end = 0x100000; }
        for (uint64_t at = begin / 64; at <= (end - 1) / 64; ++at) {
            unsigned block = (unsigned)at & 16383;
            if (t->dependencies[block / 32] & (UINT32_C(1) << (block & 31))) return 1;
        }
    }
    return 0;
}

static uint32_t row_mask(unsigned begin, unsigned end) {
    if (begin >= end) return 0;
    unsigned first = begin / 16, last = (end + 15) / 16;
    return (last == 32 ? UINT32_MAX : (UINT32_C(1) << last) - 1) & (UINT32_MAX << first);
}

static void invalidate_rows(texture *t, unsigned offset, size_t bytes) {
    unsigned base = (t->page & 15) * 128 + (t->page & 16 ? 0x80000 : 0);
    unsigned row = t->bytes / 256;
    if (offset + bytes <= base) return;
    unsigned first = offset >= base + row ? (offset - base - row) / 2048 + 1 : 0;
    unsigned last = (offset + bytes - 1 - base) / 2048;
    if (last > 255) last = 255;
    for (unsigned y = first; y <= last; ++y) {
        unsigned begin = base + y * 2048, end = begin + row;
        if (begin < offset + bytes && end > offset) {
            unsigned left = offset > begin ? offset - begin : 0;
            unsigned right = offset + bytes < end ? offset + bytes - begin : row;
            t->dirty[y] |= row_mask(left, right);
        }
        unsigned region_begin = base + y * 2048 + t->region_begin;
        unsigned region_end = base + y * 2048 + t->region_end;
        if (t->region_end && y >= t->region_top && y < t->region_bottom &&
            region_begin < offset + bytes && region_end > offset)
            t->region_end = 0;
    }
}

static void invalidate_row_range(texture *t, unsigned top, unsigned bottom,
                                 unsigned begin, unsigned end) {
    if (top >= 256) return;
    if (bottom > 256) bottom = 256;
    uint32_t mask = row_mask(begin, end);
    for (unsigned y = top; y < bottom; ++y) t->dirty[y] |= mask;
    if (t->region_end && top < t->region_bottom && t->region_top < bottom &&
        begin < t->region_end && t->region_begin < end) t->region_end = 0;
}

static void invalidate_rect(texture *t, unsigned offset, unsigned bytes, unsigned rows) {
    unsigned source_left = (t->page & 15) * 128, source_row = t->bytes / 256;
    unsigned source_first = source_row < 2048 - source_left ? source_row : 2048 - source_left;
    unsigned write_left = offset & 2047, write_top = (offset & 0xfffff) / 2048;
    unsigned write_first = bytes < 2048 - write_left ? bytes : 2048 - write_left;
    /* Both a source row and a write may spill into the next physical row.
     * Intersect their horizontal pieces, then map circular atlas rows back
     * to the cached page's 256 logical rows. */
    for (unsigned source = 0; source < 2; ++source) {
        unsigned sx = source ? 0 : source_left;
        unsigned sw = source ? source_row - source_first : source_first;
        unsigned origin = source ? source_first : 0;
        if (!sw) continue;
        for (unsigned write = 0; write < 2; ++write) {
            unsigned wx = write ? 0 : write_left;
            unsigned ww = write ? bytes - write_first : write_first;
            unsigned left = sx > wx ? sx : wx;
            unsigned right = sx + sw < wx + ww ? sx + sw : wx + ww;
            if (left >= right) continue;
            unsigned begin = origin + left - sx, end = origin + right - sx;
            if (rows >= 512) {
                invalidate_row_range(t, 0, 256, begin, end);
                continue;
            }
            unsigned top = (write_top + write - (t->page & 16 ? 256 : 0) - source) & 511;
            invalidate_row_range(t, top, top + rows, begin, end);
            if (top + rows > 512)
                invalidate_row_range(t, 0, top + rows - 512, begin, end);
        }
    }
}

static void native_atlas_write_rect_impl(const uint8_t *atlas, unsigned offset, size_t pitch,
                                        size_t bytes, unsigned rows) {
    WORK(writes, 1);
    uint64_t previous_epoch = atlas_write_epoch;
    ++atlas_write_epoch;
    if (tracked_atlas == atlas) {
        for (transient_texture *t = transients; t; t = t->next)
            if (t->atlas == atlas && t->epoch == previous_epoch &&
                !snapshot_write_overlap(t, offset, pitch, bytes, rows))
                t->epoch = atlas_write_epoch;
    }
    if (tracked_atlas != atlas) {
        char error[256];
        if (active && background_replay(active, error, sizeof error)) {
            fprintf(stderr, "[packet-gpu] %s\n", error);
            abort();
        }
        if (active) active->background_mode = BACKGROUND_IDLE;
        ++atlas_revision;
    }
    if (tracked_atlas != atlas) {
        tracked_atlas = atlas;
        for (unsigned i = 0; i < CACHE_COUNT; ++i) {
            cache[i].verified_atlas = NULL;
            cache[i].region_end = 0;
            memset(cache[i].dirty, 0xff, sizeof cache[i].dirty);
        }
        memset(palette_keys, 0, sizeof palette_keys);
        memset(small_palettes, 0, sizeof small_palettes);
        for (unsigned i = 0; i < REMAP_COUNT; ++i) remaps[i].valid = 0;
        shared_dirty = 1;
    }
    if (!bytes || !rows) return;
    int reset = bytes >= 0x100000 ||
                (pitch == 2048 && bytes == 2048 && rows >= 512);
    for (target *t = targets; t; t = t->next) {
        if (t->atlas != atlas) continue;
        if (background_affected(t, offset, pitch, bytes, rows)) {
            char error[256];
            if (background_replay(t, error, sizeof error)) {
                fprintf(stderr, "[packet-gpu] %s\n", error);
                abort();
            }
            t->background_mode = BACKGROUND_IDLE;
            t->background_valid = 0;
            t->background_count = 0;
            t->pending_key.valid = 0;
            memset(t->dependencies, 0, sizeof t->dependencies);
        }
        if (reset) {
            /* Initializers may clear GPU targets before writing CPU pixels.
             * Do not warm old deferred regions in that interval. */
            t->warm_pending = 0;
            t->warm_budget = 0;
            t->pal4_draws = 0;
        }
    }
    for (unsigned i = 0; i < 64; ++i) {
        if (palette_keys[i].valid && palette_keys[i].atlas == atlas &&
            write_overlap(palette_keys[i].offset, palette_keys[i].count * 2,
                          offset, pitch, bytes, rows))
            palette_keys[i].valid = 0;
    }
    for (unsigned i = 0; i < 256; ++i) {
        if (small_palettes[i].valid &&
            write_overlap(small_palettes[i].source, 32, offset, pitch, bytes, rows))
            small_palettes[i].valid = 0;
    }
    for (unsigned i = 0; i < CACHE_COUNT; ++i) {
        texture *t = &cache[i];
        if (!t->valid || t->verified_atlas != atlas) continue;
        if (pitch == 2048 && bytes <= 2048) {
            invalidate_rect(t, offset, (unsigned)bytes, rows);
            continue;
        }
        for (unsigned row = 0; row < rows; ++row) {
            unsigned at = (offset + (uint64_t)row * pitch) & 0xfffff;
            size_t count = bytes < 0x100000 - at ? bytes : 0x100000 - at;
            invalidate_rows(t, at, count);
            invalidate_rows(t, at + 0x100000, count);
            if (bytes > count) {
                invalidate_rows(t, 0, bytes - count);
                invalidate_rows(t, 0x100000, bytes - count);
            }
        }
    }
    for (unsigned i = 0; i < REMAP_COUNT; ++i) {
        remap_texture *t = &remaps[i];
        if (t->valid && t->verified_atlas == atlas &&
            write_overlap(t->clut * 32 & 0xfffff, 32, offset, pitch, bytes, rows)) {
            t->valid = 0;
            shared_dirty |= t->group < SHARED_GROUPS;
        }
        if (t->valid && t->verified_atlas == atlas &&
            remap_write_overlap(t, offset, pitch, bytes, rows)) {
            t->valid = 0;
            shared_dirty |= t->group < SHARED_GROUPS;
        }
    }
}

static void native_atlas_write_rect(const uint8_t *atlas, unsigned offset, size_t pitch,
                                   size_t bytes, unsigned rows) {
    NATIVE_DRAW_BEGIN();
    PROFILE_BEGIN();
    native_atlas_write_rect_impl(atlas, offset, pitch, bytes, rows);
    PROFILE_END(atlas);
    NATIVE_DRAW_END(atlas);
}

static void native_atlas_write(const uint8_t *atlas, unsigned offset, size_t bytes) {
    native_atlas_write_rect(atlas, offset, 0, bytes, 1);
}

static int fail(char *error, size_t size, const char *message) {
    snprintf(error, size, "%s", message);
    return -1;
}
static int hardware_fail(char *error, size_t size, const char *message) {
    snprintf(error, size, "%s; ready=%d, IRQ=%08lx, errors=%08lx, OPB=%08lx, vertices=%08lx",
             message, pvr_check_ready(), *(volatile uint32_t *)ASIC_ACK_A,
             *(volatile uint32_t *)ASIC_ACK_C,
             PVR_GET(PVR_TA_OPB_POS), PVR_GET(PVR_TA_VERTBUF_POS));
    return -1;
}

static unsigned word(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static int same_bytes(const uint8_t *a, const uint8_t *b, size_t bytes) {
    if (((uintptr_t)a ^ (uintptr_t)b) & 3) return !memcmp(a, b, bytes);
    /* Peel equal alignment so Newlib uses word reads instead of reading an
     * entire unaligned sampled row bytewise from VRAM. */
    while (bytes && ((uintptr_t)a & 3)) {
        if (*a++ != *b++) return 0;
        --bytes;
    }
    return !memcmp(a, b, bytes);
}
static unsigned rgb565(unsigned c) {
    return (c & 31) << 11 | (c & 0x3e0) << 1 | (c >> 10 & 31);
}
static unsigned argb1555(unsigned c) {
    return 0x8000 | (c & 31) << 10 | (c & 0x3e0) | (c >> 10 & 31);
}
static uint32_t tint(unsigned rgb, unsigned alpha) {
    unsigned r = (rgb & 255) * 2, g = (rgb >> 8 & 255) * 2, b = (rgb >> 16 & 255) * 2;
    return alpha << 24 | (r > 255 ? 255 : r) << 16 |
           (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}
static uint16_t twiddled[256];
static unsigned interleave(unsigned x) {
    x = (x | x << 4) & 0x0f0f;
    x = (x | x << 2) & 0x3333;
    return (x | x << 1) & 0x5555;
}
static unsigned twiddle(unsigned x) { return twiddled[x]; }
static unsigned modulate(unsigned c, const gex_gpu_material *m, int mask) {
    unsigned result = mask ? c & 0x8000 : 0;
    for (unsigned i = 0; i < 3; ++i)
        result |= (unsigned)m->modulation[(((m->rgb >> (i * 8)) & 255) >> 3) * 32 +
                                          ((c >> (i * 5)) & 31)] << (i * 5);
    return result;
}
static int modulates_to_zero(unsigned c, const gex_gpu_material *m) {
    return c && !(c & 0x8000) &&
           !m->modulation[(m->rgb & 255) / 8 * 32 + (c & 31)] &&
           !m->modulation[(m->rgb >> 8 & 255) / 8 * 32 + (c >> 5 & 31)] &&
           !m->modulation[(m->rgb >> 16 & 255) / 8 * 32 + (c >> 10 & 31)];
}

static void context(pvr_poly_cxt_t *c, pvr_ptr_t image, int format, int w, int h) {
    if (image)
        pvr_poly_cxt_txr(c, PVR_LIST_TR_POLY, format, w, h, image, PVR_FILTER_NONE);
    else pvr_poly_cxt_col(c, PVR_LIST_TR_POLY);
    c->gen.culling = PVR_CULLING_NONE;
    c->depth.comparison = PVR_DEPTHCMP_ALWAYS;
    c->depth.write = PVR_DEPTHWRITE_DISABLE;
    c->gen.alpha = PVR_ALPHA_ENABLE;
    c->blend.src = PVR_BLEND_SRCALPHA;
    c->blend.dst = PVR_BLEND_INVSRCALPHA;
    if (image) c->txr.env = PVR_TXRENV_MODULATEALPHA;
}

static void transient_header(transient_texture *texture, unsigned pass,
                             unsigned width, unsigned height, pvr_poly_hdr_t *header) {
    if (!texture->image_header_valid[pass]) {
        pvr_poly_cxt_t c;
        context(&c, texture->image[pass], PVR_TXRFMT_ARGB1555, width, height);
        pvr_poly_compile(&texture->image_header[pass], &c);
        texture->image_header_valid[pass] = 1;
    }
    *header = texture->image_header[pass];
}

static void vertices(pvr_vertex_t v[4], float x, float y, float w, float h,
                     float u, float tv, float du, float dv, uint32_t color) {
    for (unsigned i = 0; i < 4; ++i) {
        v[i] = (pvr_vertex_t){
            .flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
            .x = x + (i & 1 ? w : 0), .y = y + (i & 2 ? h : 0), .z = 1,
            .u = u + (i & 1 ? du : 0), .v = tv + (i & 2 ? dv : 0),
            .argb = color, .oargb = 0
        };
    }
}

static void picture_quad(pvr_poly_hdr_t *header, pvr_vertex_t v[4],
                         pvr_ptr_t image, unsigned shade) {
    pvr_poly_cxt_t c;
    context(&c, image, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, TW, TH);
    c.blend.src = PVR_BLEND_ONE;
    c.blend.dst = PVR_BLEND_ZERO;
    pvr_poly_compile(header, &c);
    vertices(v, 0, 0, 320, 240, 0, 0, 320.0f / TW, 240.0f / TH,
             0xff000000 | shade << 16 | shade << 8 | shade);
}

static void clear_texture_reads(void) {
    for (unsigned i = 0; i < CACHE_COUNT; ++i) cache[i].read_end = 0;
    for (unsigned i = 0; i < REMAP_COUNT; ++i) remaps[i].queued = 0;
}

static void merge_texture_reads(const texture_read *reads, const uint8_t *regions) {
    for (unsigned i = 0; i < CACHE_COUNT; ++i) {
        const texture_read *read = &reads[i];
        texture *t = &cache[i];
        if (!read->end) continue;
        if (!t->read_end) {
            t->read_begin = read->begin; t->read_end = read->end;
            t->read_top = read->top; t->read_bottom = read->bottom;
        } else {
            if (read->begin < t->read_begin) t->read_begin = read->begin;
            if (read->end > t->read_end) t->read_end = read->end;
            if (read->top < t->read_top) t->read_top = read->top;
            if (read->bottom > t->read_bottom) t->read_bottom = read->bottom;
        }
    }
    for (unsigned i = 0; i < REMAP_COUNT; ++i)
        if (regions[i]) remaps[i].queued = 1;
}

static void rebuild_texture_reads(void) {
    clear_texture_reads();
    merge_texture_reads(current_reads, current_remaps);
    for (target *t = targets; t; t = t->next)
        if (t->saved_size) merge_texture_reads(t->saved_reads, t->saved_remaps);
}

static void clear_current_texture_reads(void) {
    memset(current_reads, 0, sizeof current_reads);
    memset(current_remaps, 0, sizeof current_remaps);
    rebuild_texture_reads();
}

static void retire_command_resources(void) {
    clear_current_transients();
    release_transients();
    clear_current_texture_reads();
    if (saved_count) return;
    palette_used = 0;
    memset(palette_banks, 0, sizeof palette_banks);
    for (unsigned i = 0; i < SHARED_GROUPS; ++i) shared_groups[i].bank = -1;
}

static int flush_commands_impl(int wait_for_render, char *error, size_t size) {
    if (!active || !command_size) return 0;
    if (wait_for_render && command_clear_only) {
        if (pvr_wait_render_done())
            return hardware_fail(error, size, "PVR clear completion failed");
        sq_clr(current_image(active), TW * TH * sizeof(uint16_t));
        active->known_clear = 1;
        if (last_presented == active) last_presented = NULL;
    } else {
        if (last_presented == active) last_presented = NULL;
        WORK(resolves, 1);
        if (pvr_wait_ready()) return hardware_fail(error, size, "PVR tile accelerator wait failed");
        if (wait_for_render) {
            uint32_t width = TW, height = TH;
            pvr_scene_begin_txr(active->image[active->current ^ 1], &width, &height);
        } else {
            pvr_scene_begin();
        }
        if (pvr_list_begin(PVR_LIST_TR_POLY))
            return fail(error, size, "PVR ordered list initialization failed");
        pvr_poly_hdr_t header __attribute__((aligned(32)));
        pvr_vertex_t v[4] __attribute__((aligned(32)));
        if (!command_replaces) {
            WORK(preservation, 1);
            picture_quad(&header, v, current_image(active), 255);
            if (pvr_prim(&header, sizeof header) || pvr_prim(v, sizeof v))
                return hardware_fail(error, size, "PVR framebuffer preservation failed");
        }
        if (pvr_prim(commands, command_size))
            return hardware_fail(error, size, "PVR primitive submission failed");
        if (pvr_list_finish() || pvr_scene_finish())
            return hardware_fail(error, size, "PVR list submission failed");
        if (pvr_wait_ready()) return hardware_fail(error, size, "PVR registration completion failed");
        if (pvr_wait_render_done())
            return hardware_fail(error, size, "PVR framebuffer rendering completion failed");
        if (!wait_for_render) {
            uint32_t address = PVR_GET(PVR_RENDER_ADDR);
            if ((address & ~((uint32_t)PVR_RAM_SIZE - 1)) ||
                address > PVR_RAM_SIZE - 320 * 240 * 2)
                return fail(error, size, "invalid PVR render framebuffer address");
            const uint32_t *source = (const uint32_t *)(PVR_RAM_BASE + address);
            uint32_t *destination = sq_lock(active->image[active->current ^ 1]);
            for (unsigned y = 0; y < 240; ++y)
                sq_fast_cpy(destination + y * (TW / 2), source + y * (320 / 2), 320 * 2 / 32);
            sq_wait();
            sq_unlock();
        }
        active->current ^= 1;
        active->known_clear = 0;
    }
    command_size = 0;
    command_replaces = command_clear_only = 0;
    retire_command_resources();
    return 0;
}

static int flush_commands(int wait_for_render, char *error, size_t size) {
    PROFILE_BEGIN();
    int result = flush_commands_impl(wait_for_render, error, size);
    PROFILE_END(flush);
    return result;
}

static int finish(char *error, size_t size) {
    if (active && background_replay(active, error, size)) return -1;
    if ((pictures[0] || pictures[1]) && active && command_size && !command_clear_only)
        return fail(error, size, "PVR intermediate scene required before presentation");
    return flush_commands(1, error, size);
}

static int select_target(target *t, char *error, size_t size) {
    if (active != t) {
        if (active && background_stop(active, error, size)) return -1;
        if (t->saved_size > command_capacity) {
            uint8_t *expanded = memalign(32, t->saved_size);
            if (!expanded) return fail(error, size, "cannot restore PVR picture commands");
            memcpy(expanded, commands, command_size);
            free(commands);
            commands = expanded;
            command_capacity = t->saved_size;
        }
        if (command_size && command_clear_only) {
            if (finish(error, size)) return -1;
        } else if (command_size) {
            uint8_t *saved = malloc(command_size);
            if (!saved) return fail(error, size, "cannot defer PVR picture commands");
            if (capture_transients(&active->saved_transients, &active->saved_transient_count,
                                   error, size)) {
                free(saved);
                return -1;
            }
            memcpy(saved, commands, command_size);
            active->saved_commands = saved;
            active->saved_size = command_size;
            active->saved_header = last_header;
            active->saved_replaces = command_replaces;
            active->saved_clear_only = command_clear_only;
            memcpy(active->saved_reads, current_reads, sizeof current_reads);
            memcpy(active->saved_remaps, current_remaps, sizeof current_remaps);
            ++saved_count;
            command_size = 0;
            clear_current_transients();
            memset(current_reads, 0, sizeof current_reads);
            memset(current_remaps, 0, sizeof current_remaps);
            command_replaces = command_clear_only = 0;
        }
        if (t->saved_size) {
            memcpy(commands, t->saved_commands, t->saved_size);
            command_size = t->saved_size;
            last_header = t->saved_header;
            command_replaces = t->saved_replaces;
            command_clear_only = t->saved_clear_only;
            release_saved_transients(t, 1);
            memcpy(current_reads, t->saved_reads, sizeof current_reads);
            memcpy(current_remaps, t->saved_remaps, sizeof current_remaps);
            free(t->saved_commands);
            t->saved_commands = NULL;
            t->saved_size = 0;
            --saved_count;
        }
    }
    active = t;
    return 0;
}

static void release_texture(texture *t) {
    if (t->source) pvr_mem_free(t->source);
    for (unsigned j = 0; j < 3; ++j)
        if (t->image[j]) pvr_mem_free(t->image[j]);
    cache_bytes -= t->storage;
    memset(t, 0, sizeof *t);
}

static void release_remap(remap_texture *t) {
    if (t->image) {
        unsigned hash = remap_hash(t->page, t->clut, t->alpha);
        uint16_t *link = &remap_heads[hash];
        while (*link && &remaps[*link - 1] != t) link = &remaps[*link - 1].next;
        if (*link) *link = t->next;
        shared_dirty |= t->group < SHARED_GROUPS;
    }
    size_t storage = t->dimension * t->dimension * (t->group == SHARED_GROUPS ? 2 : 1);
    remap_bytes -= storage;
    cache_bytes -= storage;
    if (t->image) pvr_mem_free(t->image);
    memset(t, 0, sizeof *t);
}

static void release_cache(void) {
    for (unsigned i = 0; i < CACHE_COUNT; ++i) release_texture(&cache[i]);
    cache_hint = 0;
    for (unsigned i = 0; i < REMAP_COUNT; ++i) release_remap(&remaps[i]);
    remap_bytes = 0;
    memset(remap_heads, 0, sizeof remap_heads);
    memset(shared_groups, 0, sizeof shared_groups);
    shared_dirty = 0;
    for (unsigned i = 0; i < SHARED_GROUPS; ++i) shared_groups[i].bank = -1;
}

static void zero_texture_memory(pvr_ptr_t memory, size_t bytes);

static void *create(int width, int height, char *error, size_t size) {
    if (width != 320 || height != 240) {
        fail(error, size, "PVR packet targets require native 320x240");
        return NULL;
    }
    if (!users) {
        last_presented = NULL;
        presentation_paced = 0;
#if GEX_PROFILE_RENDERER
        if (!report_registered) {
            if (atexit(report_profile)) {
                fail(error, size, "cannot register PVR diagnostic reporting");
                return NULL;
            }
            report_registered = 1;
        }
        memset(&work, 0, sizeof work);
        memset(&timing, 0, sizeof timing);
        display_seen = 0;
        memset(&native_draw_cpu, 0, sizeof native_draw_cpu);
#endif
        memset(programmed_palettes, 0, sizeof programmed_palettes);
        for (unsigned i = 0; i < SHARED_GROUPS; ++i) shared_groups[i].bank = -1;
        for (unsigned i = 0; i < 256; ++i) twiddled[i] = interleave(i);
        if (!vid_mode || vid_mode->width != 320 || vid_mode->height != 240) {
            fail(error, size, "PVR requires the native 320x240 video mode");
            return NULL;
        }
        pvr_init_params_t params = {
            .opb_sizes = {0, 0, PVR_BINSIZE_32, 0, 0},
            .vertex_buf_size = 256 * 1024,
            .autosort_disabled = 1,
            .opb_overflow_count = 16,
            .vbuf_doublebuf_disabled = 0
        };
        if (pvr_init(&params)) {
            fail(error, size, "PVR initialization failed");
            return NULL;
        }
        PVR_SET(PVR_SCALER_CFG, PVR_GET(PVR_SCALER_CFG) & ~1u);
        pvr_set_pal_format(PVR_PAL_ARGB8888);
        /* KOS enables framebuffer dithering by default. */
        PVR_SET(PVR_FB_CFG_2, PVR_GET(PVR_FB_CFG_2) & ~8u);
        /* CPU-built commands are copied synchronously to TA store queues;
         * cached RAM avoids uncached VRAM reads and writes on every append. */
        commands = memalign(32, COMMAND_BYTES);
        command_capacity = commands ? COMMAND_BYTES : 0;
        if (!commands) {
            free(commands);
            commands = NULL;
            command_capacity = 0;
            pvr_shutdown();
            fail(error, size, "cannot allocate PVR submission storage");
            return NULL;
        }
    }
    target *t = calloc(1, sizeof *t);
    if (t) {
        for (unsigned i = 0; i < 2; ++i) {
            t->image[i] = pvr_mem_malloc(TW * TH * 2);
            if (t->image[i]) zero_texture_memory(t->image[i], TW * TH * 2);
        }
    }
    if (!t || !t->image[0] || !t->image[1]) {
        if (t) {
            for (unsigned i = 0; i < 2; ++i)
                if (t->image[i]) pvr_mem_free(t->image[i]);
            free(t);
        }
        if (!users) {
            free(commands); commands = NULL; command_capacity = 0;
            pvr_shutdown();
        }
        fail(error, size, "cannot allocate PVR framebuffer textures");
        return NULL;
    }
    if (!users) {
        previous_scheduler_hz = thd_get_hz();
        if (thd_set_hz(1000)) {
            for (unsigned i = 0; i < 2; ++i) pvr_mem_free(t->image[i]);
            free(t);
            free(commands); commands = NULL; command_capacity = 0;
            pvr_shutdown();
            fail(error, size, "cannot select millisecond PVR scheduling");
            return NULL;
        }
    }
    t->known_clear = 1;
    ++users;
    t->next = targets;
    targets = t;
    if (users == 1)
        fprintf(stderr, "[packet-gpu] PVR 320x240; presorted blending; native PAL4/PAL8 textures\n");
    return t;
}

static void destroy(void *data) {
    target *t = data;
    if (active == t) {
        command_size = 0;
        command_replaces = command_clear_only = 0;
        active = NULL;
        retire_command_resources();
    }
    if (t->saved_size) {
        release_saved_transients(t, 0);
        free(t->saved_commands);
        t->saved_commands = NULL;
        t->saved_size = 0;
        --saved_count;
        rebuild_texture_reads();
        release_transients();
    }
    for (unsigned i = 0; i < 2; ++i)
        if (t->image[i]) pvr_mem_free(t->image[i]);
    free(t->background);
    for (unsigned i = 0; i < 2; ++i) if (pictures[i] == t) pictures[i] = NULL;
    if (t->history) pvr_mem_free(t->history);
    if (last_presented == t) last_presented = NULL;
    target **link = &targets;
    while (*link != t) link = &(*link)->next;
    *link = t->next;
    free(t);
    if (!--users) {
        release_cache();
        ++atlas_write_epoch;
        release_transients();
        tracked_atlas = NULL;
        memset(palette_keys, 0, sizeof palette_keys);
        memset(small_palettes, 0, sizeof small_palettes);
        free(commands); commands = NULL; command_capacity = 0;
        pvr_shutdown();
        if (thd_set_hz(previous_scheduler_hz)) {
            fprintf(stderr, "[packet-gpu] cannot restore scheduler frequency\n");
            abort();
        }
        command_size = palette_used = 0;
        saved_count = 0;
        memset(palette_banks, 0, sizeof palette_banks);
        command_replaces = command_clear_only = 0;
        cache_clock = 0;
    }
}

static int upload(void *data, const uint16_t *pixels, char *error, size_t size) {
    target *t = data;
    if (t->owned) return fail(error, size, "PVR live framebuffer CPU replacement is forbidden");
    if (background_stop(t, error, size) ||
        select_target(t, error, size) || finish(error, size)) return -1;
    t->known_clear = 0;
    if (last_presented == t) last_presented = NULL;
    uint16_t *out = t->image[t->current];
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x)
            out[y * TW + x] = (uint16_t)rgb565(pixels[y * 320 + x]);
    return 0;
}

static int download(void *data, uint16_t *pixels, char *error, size_t size) {
    target *t = data;
    if (t->owned) return fail(error, size, "PVR live framebuffer readback is forbidden");
    if (background_stop(t, error, size) ||
        select_target(t, error, size) || finish(error, size)) return -1;
    const uint16_t *in = current_image(t);
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x) {
            unsigned c = in[y * TW + x];
            pixels[y * 320 + x] = (uint16_t)((c >> 11) | (c >> 1 & 0x3e0) | (c & 31) << 10);
        }
    return 0;
}

static texture *texture_region(const gex_gpu_material *m, int raw, int u0, int u1, int v0, int v1,
                               char *error, size_t size);

static texture *page_texture(const gex_gpu_material *m, const gex_gpu_span *spans,
                             size_t count, char *error, size_t size) {
    int u0 = 256, u1 = 0, v0 = 256, v1 = 0;
    for (size_t i = 0; i < count; ++i) {
        const gex_gpu_span *s = &spans[i];
        double divisor = s->mode == 1 ? 65536 : 1;
        int first = s->phase / s->scale;
        int last = (s->count - 1 + s->phase) / s->scale;
        int a = (int)floor((s->u + first * s->du) / divisor);
        int b = (int)floor((s->u + last * s->du) / divisor);
        int c = (int)floor((s->v + first * s->dv) / divisor);
        int d = (int)floor((s->v + last * s->dv) / divisor);
        if (s->reserved[0] > 1) {
            int last_row = (s->reserved[0] - 1) * s->reserved[1] / (int)divisor;
            d += last_row;
        }
        if (b < a) { int swap = a; a = b; b = swap; }
        if (d < c) { int swap = c; c = d; d = swap; }
        if (a < 0 || b > 255) { a = 0; b = 255; }
        if (c < 0 || d > 255) { c = 0; d = 255; }
        if (a < u0) u0 = a;
        if (b + 1 > u1) u1 = b + 1;
        if (c < v0) v0 = c;
        if (d + 1 > v1) v1 = d + 1;
    }
    return texture_region(m, 0, u0, u1, v0, v1, error, size);
}

static void pack_row(texture *t, const gex_gpu_material *m, int raw,
                     unsigned y, unsigned begin, unsigned end) {
    unsigned format = m->page >> 7 & 3;
    unsigned row = format == 0 ? 128 : format == 1 ? 256 : 512;
    if (format < 2) {
        /* PVR paletted words pack two neighboring rows. Preserve the
         * other row from the snapshot, not from potentially dirty RAM. */
        const uint8_t *a = t->source + (y & ~1u) * row;
        const uint8_t *b = a + row;
        uint16_t *out = t->image[0];
        for (unsigned x = begin; x < end; ++x) {
            if (format == 0)
                out[twiddle(y / 2) | twiddle(x) << 1] =
                    (a[x] & 15) | (b[x] & 15) << 4 | (a[x] >> 4) << 8 | (b[x] >> 4) << 12;
            else
                out[twiddle(x) | twiddle(y / 2) << 1] = a[x] | (unsigned)b[x] << 8;
        }
    } else {
        for (unsigned x = begin / 2; x < end / 2; ++x) {
            unsigned c = word(t->source + y * row + x * 2);
            int visible = c != 0;
            if (!raw && (m->opcode & ~2u) == 0x64 && !(m->opcode & 2) &&
                (m->rgb & 255) != 128 && (m->rgb >> 8 & 255) != 128 &&
                (m->rgb >> 16 & 255) != 128) c = modulate(c, m, 0);
            int masked = (m->opcode & 2) && (c & 0x8000);
            unsigned at = twiddle(y) | twiddle(x) << 1;
            unsigned color = visible ? argb1555(c) : 0;
            if (raw) {
                ((uint16_t *)t->image[0])[at] = (uint16_t)color;
                if (t->image[1]) {
                    ((uint16_t *)t->image[1])[at] = c & 0x8000 ? 0 : (uint16_t)color;
                    ((uint16_t *)t->image[2])[at] = c & 0x8000 ? (uint16_t)color : 0;
                }
            } else {
                ((uint16_t *)t->image[0])[at] = masked ? 0 : (uint16_t)color;
                if (t->image[1])
                    ((uint16_t *)t->image[1])[at] = masked ? (uint16_t)color : 0;
            }
        }
    }
}

/* PVR VRAM does not accept byte writes, even for an unaligned source. */
static void copy_vram_words(volatile uint16_t *destination, const uint8_t *source,
                            size_t words) {
    typedef uint32_t alias32 __attribute__((__may_alias__));
    if (words && ((uintptr_t)destination & 2)) {
        *destination++ = (uint16_t)(source[0] | (unsigned)source[1] << 8);
        source += 2;
        --words;
    }
    if (!((uintptr_t)source & 3)) {
        while (words >= 2) {
            *(volatile uint32_t *)destination = *(const alias32 *)source;
            destination += 2;
            source += 4;
            words -= 2;
        }
    } else {
        while (words >= 2) {
            uint32_t value = source[0] | (uint32_t)source[1] << 8 |
                             (uint32_t)source[2] << 16 | (uint32_t)source[3] << 24;
            *(volatile uint32_t *)destination = value;
            destination += 2;
            source += 4;
            words -= 2;
        }
    }
    if (words)
        *destination = (uint16_t)(source[0] | (unsigned)source[1] << 8);
}

static void update_row(texture *t, const gex_gpu_material *m,
                       unsigned y, unsigned begin, unsigned end, int raw) {
    if (begin >= end) return;
    unsigned format = m->page >> 7 & 3;
    unsigned row = format == 0 ? 128 : format == 1 ? 256 : 512;
    unsigned base = (m->page & 15) * 128 + (m->page & 16 ? 0x80000 : 0);
    unsigned at = (base + y * 2048 + begin) & 0xfffff;
    unsigned bytes = end - begin;
    WORK(refresh_bytes, bytes);
    unsigned first = bytes < 0x100000 - at ? bytes : 0x100000 - at;
    /* Dirty blocks and the atlas wrap boundary are 16-byte aligned. */
    copy_vram_words((uint16_t *)(t->source + y * row + begin), m->atlas + at, first / 2);
    copy_vram_words((uint16_t *)(t->source + y * row + begin + first),
                    m->atlas, (bytes - first) / 2);
    pack_row(t, m, raw, y, begin, end);
}

static int refresh_reads(const texture *t, const gex_gpu_material *m,
                         unsigned begin, unsigned end, unsigned top, unsigned bottom) {
    if (!t->read_end) return 0;
    uint32_t mask = row_mask(begin, end) & row_mask(t->read_begin, t->read_end);
    if (!mask) return 0;
    unsigned first = t->read_top, last = t->read_bottom;
    if ((t->page >> 7 & 3) < 2) {
        first &= ~1u;
        last = (last + 1) & ~1u;
    }
    if (top > first) first = top;
    if (bottom < last) last = bottom;
    for (unsigned y = first; y < last; ++y)
        if (t->verified_atlas != m->atlas || (t->dirty[y] & mask)) return 1;
    return 0;
}

static void update_region(texture *t, const gex_gpu_material *m,
                          unsigned begin, unsigned end, unsigned top, unsigned bottom, int raw) {
    unsigned format = m->page >> 7 & 3;
    unsigned row = format == 0 ? 128 : format == 1 ? 256 : 512;
    uint32_t mask = row_mask(begin, end);
    if (t->verified_atlas != m->atlas) {
        memset(t->dirty, 0xff, sizeof t->dirty);
        t->verified_atlas = m->atlas;
    }
    /* Refresh only requested blocks; unrelated queued samples retain their
     * old snapshot until a later request retires or replaces them. */
    for (unsigned y = top; y < bottom; ++y) {
        uint32_t dirty = t->dirty[y] & mask;
        for (unsigned bit = 0; bit < row / 16;) {
            if (!(dirty & (UINT32_C(1) << bit))) { ++bit; continue; }
            unsigned begin = bit++;
            while (bit < row / 16 && (dirty & (UINT32_C(1) << bit))) ++bit;
            update_row(t, m, y, begin * 16, bit * 16, raw);
        }
        t->dirty[y] &= ~mask;
    }
    t->region_begin = begin; t->region_end = end;
    t->region_top = top; t->region_bottom = bottom;
}

static int evict_texture(texture *protected, size_t bytes, int allow_snapshot,
                         char *error, size_t size) {
    texture *victim = NULL;
    remap_texture *remapped = NULL;
    transient_texture **snapshot = allow_snapshot ? oldest_idle_snapshot() : NULL;
    if (snapshot) {
        if (pvr_wait_render_done())
            return hardware_fail(error, size, "PVR texture reclamation completion failed");
        free_transient(snapshot);
        return 0;
    }
    for (unsigned fenced = 0; fenced < 2; ++fenced) {
        /* Prefer an allocation large enough to open the requested hole. */
        for (unsigned pass = 0; pass < 2; ++pass) {
            size_t minimum = pass ? 0 : bytes;
            for (unsigned i = 0; i < CACHE_COUNT; ++i)
                if (&cache[i] != protected && cache[i].storage && !cache[i].read_end &&
                    (cache[i].bytes >= minimum || cache[i].source_bytes >= minimum) &&
                    (!victim || cache[i].used < victim->used)) victim = &cache[i];
            for (unsigned i = 0; i < REMAP_COUNT; ++i)
                if (remaps[i].image && !remaps[i].queued &&
                    remaps[i].dimension * remaps[i].dimension *
                        (remaps[i].group == SHARED_GROUPS ? 2u : 1u) >= minimum &&
                    (!remapped || remaps[i].used < remapped->used))
                    remapped = &remaps[i];
            if (victim || remapped) break;
        }
        if (victim || remapped) break;
        if (fenced) break;
        if (finish(error, size)) return -1;
    }
    if (pvr_wait_render_done())
        return hardware_fail(error, size, "PVR texture reclamation completion failed");
    if (!victim && !remapped) return fail(error, size, "PVR texture memory cannot be reclaimed");
    if (remapped && (!victim || remapped->used < victim->used)) release_remap(remapped);
    else release_texture(victim);
    return 0;
}

static int reserve_texture(texture *t, size_t storage, char *error, size_t size) {
    while (cache_bytes + storage > CACHE_BYTES ||
           cache_bytes - retained_snapshot_bytes + storage > CACHE_BYTES - SNAPSHOT_BYTES) {
        int page_pressure = cache_bytes - retained_snapshot_bytes + storage >
                            CACHE_BYTES - SNAPSHOT_BYTES;
        if (evict_texture(t, 0, !page_pressure, error, size)) return -1;
    }
    return 0;
}

static int cache_can_reserve(texture *protected, size_t storage, size_t replaced) {
    size_t available = CACHE_BYTES - cache_bytes + replaced;
    size_t page_available = CACHE_BYTES - SNAPSHOT_BYTES -
                            (cache_bytes - retained_snapshot_bytes) + replaced;
    for (transient_texture *t = transients; t; t = t->next)
        if (t->retained_bytes && !t->current && !t->saved_refs && !transient_pinned(t))
            available += t->retained_bytes;
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
        if (&cache[i] != protected && !cache[i].read_end) {
            available += cache[i].storage;
            page_available += cache[i].storage;
        }
    for (unsigned i = 0; i < REMAP_COUNT; ++i)
        if (remaps[i].image && !remaps[i].queued) {
            size_t bytes = remaps[i].dimension * remaps[i].dimension *
                           (remaps[i].group == SHARED_GROUPS ? 2u : 1u);
            available += bytes;
            page_available += bytes;
        }
    return available >= storage && page_available >= storage;
}

static pvr_ptr_t allocate_texture(texture *protected, size_t bytes,
                                  const char *purpose, unsigned key,
                                  unsigned variant, unsigned flags,
                                  char *error, size_t size) {
    unsigned reclaimed = 0;
    for (;;) {
        pvr_ptr_t memory = pvr_mem_malloc(bytes);
        if (memory) {
            if (reclaimed)
                fprintf(stderr, "[packet-gpu] recovered %lu-byte VRAM allocation "
                        "purpose=%s key=%x variant=%x flags=%x after %u cache evictions\n",
                        (unsigned long)bytes, purpose, key, variant, flags, reclaimed);
            return memory;
        }
        WORK(allocation_retries, 1);
        if (!reclaimed)
            fprintf(stderr, "[packet-gpu] reclaiming VRAM after allocation failure: "
                    "purpose=%s key=%x variant=%x flags=%x request=%lu cache=%lu free=%lu\n",
                    purpose, key, variant, flags, (unsigned long)bytes, (unsigned long)cache_bytes,
                    (unsigned long)pvr_mem_available());
        if (evict_texture(protected, bytes, 1, error, size)) {
            pvr_mem_stats();
            return NULL;
        }
        ++reclaimed;
    }
}

static pvr_ptr_t allocate_texture_without_drain(texture *protected, size_t bytes,
                                                char *error, size_t size) {
    for (;;) {
        pvr_ptr_t memory = pvr_mem_malloc(bytes);
        if (memory) return memory;
        int reclaimable = 0;
        for (unsigned i = 0; i < CACHE_COUNT; ++i)
            if (&cache[i] != protected && cache[i].storage && !cache[i].read_end)
                reclaimable = 1;
        for (unsigned i = 0; i < REMAP_COUNT; ++i)
            if (remaps[i].image && !remaps[i].queued)
                reclaimable = 1;
        if (oldest_idle_snapshot()) reclaimable = 1;
        if (!reclaimable) return NULL;
        if (evict_texture(protected, bytes, 1, error, size)) return NULL;
    }
}

static transient_texture *snapshot_picture_image(const target *source, char *error, size_t size) {
    transient_texture *snapshot = calloc(1, sizeof *snapshot);
    if (!snapshot) {
        fail(error, size, "cannot allocate PVR converted texture record");
        return NULL;
    }
    snapshot->next = transients;
    transients = snapshot;
    transient_pin pin = {snapshot, active_transient_pin};
    active_transient_pin = &pin;
    snapshot->image[0] =
        allocate_texture(NULL, TW * TH * 2, "picture-copy", 0, 0, 0, error, size);
    if (!snapshot->image[0]) {
        active_transient_pin = pin.previous;
        release_transients();
        return NULL;
    }
    uint32_t *destination = sq_lock(snapshot->image[0]);
    sq_fast_cpy(destination, current_image(source), TW * TH * 2 / 32);
    sq_wait();
    sq_unlock();
    snapshot->current = 1;
    active_transient_pin = pin.previous;
    return snapshot;
}

static unsigned sampled_color(const gex_gpu_material *m, unsigned u, unsigned v) {
    unsigned format = m->page >> 7 & 3;
    unsigned base = (m->page & 15) * 128 + (m->page & 16 ? 0x80000 : 0);
    unsigned at = (base + v * 2048 +
                   (format == 0 ? u / 2 : format == 1 ? u : u * 2)) & 0xfffff;
    if (format == 2) return word(m->atlas + at);
    unsigned index = m->atlas[at];
    if (format == 0) index = index >> ((u & 1) * 4) & 15;
    return word(m->atlas + ((m->clut * 32 + index * 2) & 0xffffe));
}

static void snapshot_bounds(unsigned u0, unsigned u1, unsigned v0, unsigned v1,
                            unsigned *left, unsigned *top, unsigned *dimension) {
    *dimension = 8;
    *left = u0;
    *top = v0;
    while (*dimension < u1 - u0 || *dimension < v1 - v0) *dimension *= 2;
    if (*left + *dimension > 256) *left = 256 - *dimension;
    if (*top + *dimension > 256) *top = 256 - *dimension;
}

static transient_texture *find_snapshot(const gex_gpu_material *m,
                                        unsigned left, unsigned top, unsigned dimension,
                                        const uint32_t zero_mask[8]) {
    if (m->atlas != tracked_atlas || !active_transient_pin) return NULL;
    unsigned bucket = snapshot_texture_bucket(m->page, m->clut, m->opcode & 2,
                                              left, top, dimension);
    for (transient_texture *t = snapshot_textures[bucket]; t; t = t->snapshot_next)
        if (t->atlas == m->atlas && t->epoch == atlas_write_epoch &&
            t->page == m->page && t->clut == m->clut &&
            t->alpha == (m->opcode & 2) && t->origin_u == left &&
            t->origin_v == top && t->dimension == dimension &&
            !memcmp(t->zero_mask, zero_mask, sizeof t->zero_mask)) {
            t->used = ++cache_clock;
            active_transient_pin->texture = t;
            return t;
        }
    return NULL;
}

static transient_texture *snapshot_texture(const gex_gpu_material *m,
                                           unsigned u0, unsigned u1,
                                           unsigned v0, unsigned v1,
                                           const uint32_t zero_mask[8],
                                           char *error, size_t size) {
    unsigned dimension, left, top;
    snapshot_bounds(u0, u1, v0, v1, &left, &top, &dimension);
    if (!active_transient_pin) {
        fail(error, size, "PVR converted texture has no submission scope");
        return NULL;
    }
    transient_texture *existing = find_snapshot(m, left, top, dimension, zero_mask);
    if (existing) return existing;
    transient_texture *t = calloc(1, sizeof *t);
    if (!t) {
        fail(error, size, "cannot allocate PVR converted texture record");
        return NULL;
    }
    t->next = transients;
    transients = t;
    active_transient_pin->texture = t;
    size_t bytes = dimension * dimension * sizeof(uint16_t);
    t->image[0] = allocate_texture(NULL, bytes, "texture-snapshot", m->page,
                                   m->clut, m->opcode, error, size);
    if (t->image[0] && (m->opcode & 2))
        t->image[1] = allocate_texture(NULL, bytes, "texture-snapshot-alpha", m->page,
                                       m->clut, m->opcode, error, size);
    if (!t->image[0] || ((m->opcode & 2) && !t->image[1])) {
        active_transient_pin->texture = NULL;
        transients = t->next;
        for (unsigned i = 0; i < 2; ++i)
            if (t->image[i]) pvr_mem_free(t->image[i]);
        free(t);
        return NULL;
    }
    unsigned masked = 0;
    for (unsigned i = 0; i < 8; ++i) masked |= zero_mask[i];
    for (unsigned y = 0; y < dimension; y += 2)
        for (unsigned x = 0; x < dimension; x += 2) {
            unsigned c[4] = {
                sampled_color(m, left + x, top + y),
                sampled_color(m, left + x, top + y + 1),
                sampled_color(m, left + x + 1, top + y),
                sampled_color(m, left + x + 1, top + y + 1)
            };
            uint16_t opaque[4], alpha[4];
            for (unsigned i = 0; i < 4; ++i) {
                unsigned value = c[i] && (!masked || modulate(c[i], m, 1)) ?
                    argb1555(c[i]) : 0;
                int half = (m->opcode & 2) && (c[i] & 0x8000);
                opaque[i] = half ? 0 : (uint16_t)value;
                alpha[i] = half ? (uint16_t)value : 0;
            }
            unsigned at = (twiddle(y) | twiddle(x) << 1) / 2;
            uint32_t *out = t->image[0];
            out[at] = opaque[0] | (uint32_t)opaque[1] << 16;
            out[at + 1] = opaque[2] | (uint32_t)opaque[3] << 16;
            if (t->image[1]) {
                out = t->image[1];
                out[at] = alpha[0] | (uint32_t)alpha[1] << 16;
                out[at + 1] = alpha[2] | (uint32_t)alpha[3] << 16;
            }
        }
    t->atlas = m->atlas;
    t->epoch = atlas_write_epoch;
    t->page = m->page;
    t->clut = m->clut;
    t->alpha = m->opcode & 2;
    memcpy(t->zero_mask, zero_mask, sizeof t->zero_mask);
    t->origin_u = left;
    t->origin_v = top;
    t->dimension = dimension;
    t->used = ++cache_clock;
    size_t storage = bytes * ((m->opcode & 2) ? 2 : 1);
    if (m->atlas == tracked_atlas && storage <= SNAPSHOT_BYTES) {
        int fenced = 0;
        while (storage > SNAPSHOT_BYTES - (retained_snapshot_bytes - retained_title_bytes) ||
               storage > CACHE_BYTES - cache_bytes) {
            int kind = storage > SNAPSHOT_BYTES - (retained_snapshot_bytes - retained_title_bytes) ? 0 : -1;
            transient_texture **victim = oldest_idle_snapshot_kind(kind);
            if (!victim) break;
            if (!fenced && pvr_wait_render_done()) {
                active_transient_pin->texture = NULL;
                free_transient(&transients);
                hardware_fail(error, size, "PVR snapshot reclamation completion failed");
                return NULL;
            }
            fenced = 1;
            free_transient(victim);
        }
    }
    if (m->atlas == tracked_atlas &&
        storage <= SNAPSHOT_BYTES - (retained_snapshot_bytes - retained_title_bytes) &&
        storage <= CACHE_BYTES - cache_bytes) {
        t->retained_bytes = storage;
        retained_snapshot_bytes += storage;
        cache_bytes += storage;
    }
    unsigned bucket = snapshot_texture_bucket(t->page, t->clut, t->alpha,
                                              t->origin_u, t->origin_v, t->dimension);
    t->snapshot_next = snapshot_textures[bucket];
    t->snapshot_bucket = bucket + 1;
    snapshot_textures[bucket] = t;
    WORK(snapshots, 1);
    return t;
}

static void zero_texture_memory(pvr_ptr_t memory, size_t bytes) {
    volatile uint32_t *out = memory;
    for (size_t i = 0; i < bytes / sizeof *out; ++i) out[i] = 0;
}

static texture *texture_region_base(const gex_gpu_material *m, int raw,
                                    int u0, int u1, int v0, int v1, int avoid_drain,
                                    char *error, size_t size) {
    /* With avoid_drain, NULL without an error requests a compact snapshot. */
    WORK(page_lookups, 1);
    unsigned format = m->page >> 7 & 3;
    raw = format == 2 && raw;
    size_t row = format == 0 ? 128 : format == 1 ? 256 : 512;
    size_t bytes = row * 256;
    unsigned base = (m->page & 15) * 128 + (m->page & 16 ? 0x80000 : 0);
    size_t begin = format == 0 ? (unsigned)u0 / 2 : format == 1 ? (unsigned)u0 : (unsigned)u0 * 2;
    size_t end = format == 0 ? ((unsigned)u1 + 1) / 2 : format == 1 ? (unsigned)u1 : (unsigned)u1 * 2;
    unsigned rgb = format == 2 && !raw ? m->rgb : 0;
    unsigned opcode = format == 2 && !raw ? m->opcode : 0;
    texture *stale = NULL;
    uint64_t used = ++cache_clock;
    unsigned hint = cache_hint;
    for (unsigned scan = 0; scan < CACHE_COUNT; ++scan) {
        unsigned i = !scan ? hint : scan <= hint ? scan - 1 : scan;
        texture *t = &cache[i];
        WORK(page_checks, 1);
        if (!t->valid || t->page != m->page || t->rgb != rgb ||
            t->opcode != opcode || t->raw != (unsigned)raw || t->bytes != bytes ||
            (format == 2 && !raw && memcmp(t->source + bytes, m->modulation, 1024))) continue;
        cache_hint = i;
        if (m->atlas == tracked_atlas && t->verified_atlas == m->atlas &&
            t->region_end >= end && t->region_begin <= begin &&
            t->region_top <= (unsigned)v0 && t->region_bottom >= (unsigned)v1) {
            t->used = used;
            return t;
        }
        int equal = 1;
        /* Pages at the atlas's right edge also contain wrapped framebuffer
         * bytes. Only sampled texels may invalidate a cached CEL texture. */
        for (int y = v0; equal && y < v1; ++y) {
            unsigned at = (base + y * 2048 + begin) & 0xfffff;
            size_t bytes = end - begin;
            size_t first = bytes < 0x100000 - at ? bytes : 0x100000 - at;
            if (m->atlas == tracked_atlas && t->verified_atlas == m->atlas &&
                !(t->dirty[y] & row_mask(begin, end)))
                continue;
            equal = same_bytes(t->source + y * row + begin, m->atlas + at, first) &&
                    same_bytes(t->source + y * row + begin + first, m->atlas, bytes - first);
            WORK(compared_bytes, bytes);
            if (equal) {
                /* Unsampled GPU bytes are unspecified, even when their
                 * zero source snapshot matches the newly requested texels. */
                pack_row(t, m, raw, y, begin, end);
            }
            if (equal && m->atlas == tracked_atlas) {
                if (t->verified_atlas != m->atlas) {
                    memset(t->dirty, 0xff, sizeof t->dirty);
                    t->verified_atlas = m->atlas;
                }
                /* A partial comparison cannot verify neighboring bytes in
                 * its first/last dirty block. The exact region cache can. */
                t->dirty[y] &= ~row_mask((begin + 15) & ~15u, end & ~15u);
            }
        }
        if (equal) {
            if (m->atlas == tracked_atlas &&
                (!t->region_end || (end - begin) * (v1 - v0) >=
                (t->region_end - t->region_begin) * (t->region_bottom - t->region_top))) {
                t->region_begin = begin; t->region_end = end;
                t->region_top = v0; t->region_bottom = v1;
            }
            t->used = used;
            return t;
        }
        stale = t;
    }
    texture *t = stale;
    if (t && m->atlas == tracked_atlas && (format < 2 || raw)) {
        WORK(texture_refreshes, 1);
        if (refresh_reads(t, m, begin, end, v0, v1)) {
            texture *replacement = NULL;
            for (unsigned i = 0; i < CACHE_COUNT; ++i) {
                texture *candidate = &cache[i];
                if (candidate == t || candidate->read_end) continue;
                if (!replacement || candidate->used < replacement->used)
                    replacement = candidate;
            }
            if (replacement) {
                t = replacement;
                goto rebuild;
            }
            if (avoid_drain) return NULL;
            if (finish(error, size)) return NULL;
        }
        update_region(t, m, begin, end, v0, v1, 1);
        t->used = used;
        return t;
    }
    if (!t) {
        for (unsigned i = 0; i < CACHE_COUNT; ++i)
            if (!cache[i].read_end && (!t || cache[i].used < t->used)) t = &cache[i];
        if (!t)
            for (unsigned i = 0; i < CACHE_COUNT; ++i)
                if (!t || cache[i].used < t->used) t = &cache[i];
    }
rebuild:
    if (avoid_drain && t->valid && t->read_end) return NULL;
    if (t->valid && t->read_end && finish(error, size)) return NULL;
    t->verified_atlas = NULL;
    t->region_begin = t->region_end = t->region_top = t->region_bottom = 0;
    memset(t->dirty, 0xff, sizeof t->dirty);
    t->valid = 0;
    size_t source_bytes = bytes + (format == 2 && !raw ? 1024 : 0);
    int masked = format == 2 && !raw && (m->opcode & 2);
    if (t->bytes != bytes || t->source_bytes != source_bytes ||
        (!raw && !!t->image[1] != masked)) {
        size_t storage = source_bytes + bytes * (masked ? 2 : 1);
        if (avoid_drain && !cache_can_reserve(t, storage, t->storage)) return NULL;
        release_texture(t);
        /* Keep small paletted pages resident without allowing direct-color
         * pages to consume VRAM reserved for picture buffers and PVR binning. */
        if (reserve_texture(t, storage, error, size)) return NULL;
        t->bytes = bytes;
        t->source_bytes = source_bytes;
        t->storage = storage;
        cache_bytes += storage;
        t->source = avoid_drain ?
            allocate_texture_without_drain(t, source_bytes, error, size) :
            allocate_texture(t, source_bytes, "page-source", m->page,
                             m->rgb, m->opcode, error, size);
        if (t->source)
            t->image[0] = avoid_drain ?
                allocate_texture_without_drain(t, bytes, error, size) :
                allocate_texture(t, bytes, "page-image", m->page,
                                 m->rgb, m->opcode, error, size);
        if (t->image[0] && masked)
            t->image[1] = avoid_drain ?
                allocate_texture_without_drain(t, bytes, error, size) :
                allocate_texture(t, bytes, "page-alpha", m->page,
                                 m->rgb, m->opcode, error, size);
        if (!t->source || !t->image[0] || (masked && !t->image[1])) {
            release_texture(t);
            return NULL;
        }
    }
    if (!t->source || !t->image[0] || (masked && !t->image[1])) {
        fail(error, size, "cannot allocate PVR page cache");
        return NULL;
    }
    WORK(page_builds, 1);
    /* Unsampled source bytes remain readable for lazy alpha creation.
     * GPU bytes are initialized only when requested, including equal zeros. */
    zero_texture_memory(t->source, source_bytes);
    t->page = m->page; t->rgb = rgb; t->opcode = opcode; t->raw = raw;
    if (format == 2 && !raw)
        copy_vram_words((uint16_t *)(t->source + bytes), m->modulation, 1024 / 2);
    unsigned top = format < 2 ? (unsigned)v0 & ~1u : (unsigned)v0;
    unsigned bottom = format < 2 ? ((unsigned)v1 + 1) & ~1u : (unsigned)v1;
    update_region(t, m, (unsigned)begin & ~15u, ((unsigned)end + 15) & ~15u,
                  top, bottom, raw);
    t->valid = 1;
    if (m->atlas != tracked_atlas) t->verified_atlas = NULL;
    t->used = used;
    cache_hint = (unsigned)(t - cache);
    return t;
}

static texture *texture_region_impl(const gex_gpu_material *m, int raw, int u0, int u1,
                                    int v0, int v1, int avoid_drain, char *error, size_t size) {
    texture *t = texture_region_base(m, raw, u0, u1, v0, v1, avoid_drain, error, size);
    if (!t || !t->raw || !(m->opcode & 2) || t->image[1]) return t;
    /* Opaque and alpha draws share one source and opaque image. Create the
     * two alpha passes lazily, without modifying an image in a queued draw. */
    if (avoid_drain && !cache_can_reserve(t, t->bytes * 2, 0)) return NULL;
    if (reserve_texture(t, t->bytes * 2, error, size)) return NULL;
    pvr_ptr_t unmasked = avoid_drain ?
        allocate_texture_without_drain(t, t->bytes, error, size) :
        allocate_texture(t, t->bytes, "page-alpha", m->page,
                         m->rgb, m->opcode, error, size);
    pvr_ptr_t masked = unmasked ?
        (avoid_drain ? allocate_texture_without_drain(t, t->bytes, error, size) :
         allocate_texture(t, t->bytes, "page-alpha", m->page, m->rgb, m->opcode,
                          error, size)) : NULL;
    if (!unmasked || !masked) {
        if (unmasked) pvr_mem_free(unmasked);
        if (masked) pvr_mem_free(masked);
        if (avoid_drain && (!error || !size || !*error)) return NULL;
        fprintf(stderr, "[packet-gpu] alpha allocation failed: page=%lx opcode=%lx "
                "cache=%lu free=%lu request=%lu\n",
                (unsigned long)m->page, (unsigned long)m->opcode,
                (unsigned long)cache_bytes, (unsigned long)pvr_mem_available(),
                (unsigned long)(t->bytes * 2));
        pvr_mem_stats();
        return NULL;
    }
    const uint8_t *source = __builtin_assume_aligned(t->source, 4);
    uint32_t *opaque = unmasked, *alpha = masked;
    for (unsigned y = 0; y < 256; y += 2)
        for (unsigned x = 0; x < 256; x += 2) {
            uint32_t a, b;
            memcpy(&a, source + y * 512 + x * 2, sizeof a);
            memcpy(&b, source + (y + 1) * 512 + x * 2, sizeof b);
            if (!(a | b) && (x + 2 <= (unsigned)u0 || x >= (unsigned)u1 ||
                             y + 2 <= (unsigned)v0 || y >= (unsigned)v1)) continue;
            unsigned at = (twiddle(y) | twiddle(x) << 1) / 2;
            for (unsigned col = 0; col < 2; ++col) {
                unsigned ca = a >> (col * 16) & 0xffff, cb = b >> (col * 16) & 0xffff;
                unsigned va = ca ? argb1555(ca) : 0, vb = cb ? argb1555(cb) : 0;
                opaque[at + col] = (ca & 0x8000 ? 0 : va) |
                                  (cb & 0x8000 ? 0 : vb) << 16;
                alpha[at + col] = (ca & 0x8000 ? va : 0) |
                                 (cb & 0x8000 ? vb : 0) << 16;
            }
        }
    t->image[1] = unmasked; t->image[2] = masked;
    /* Previously sampled zeros outside this request also need alpha pixels
     * before an exact-region or unchanged-source hit can reuse them. */
    memset(t->dirty, 0xff, sizeof t->dirty);
    t->region_end = 0;
    t->storage += t->bytes * 2;
    cache_bytes += t->bytes * 2;
    return t;
}

static texture *texture_region(const gex_gpu_material *m, int raw, int u0, int u1, int v0, int v1,
                               char *error, size_t size) {
    return texture_region_impl(m, raw, u0, u1, v0, v1, 0, error, size);
}

static uint32_t *palette_colors(const gex_gpu_material *m, int raw, unsigned count,
                                uint32_t decoded[256]) {
    unsigned source = m->clut * 32 & 0xfffff;
    uint32_t *colors = decoded;
    unsigned slot = (uint32_t)((source / 32) * UINT32_C(2654435761)) >> 24;
    int cached = raw && count == 16 && m->atlas == tracked_atlas;
    if (cached && small_palettes[slot].valid && small_palettes[slot].source == source &&
        small_palettes[slot].alpha == (m->opcode & 2)) {
        colors = small_palettes[slot].colors;
    } else {
        for (unsigned i = 0; i < count; ++i) {
            unsigned c = word(m->atlas + ((m->clut * 32 + i * 2) & 0xffffe));
            if (!raw) c = modulate(c, m, 1);
            unsigned alpha = !c ? 0 : (m->opcode & 2) && (c & 0x8000) ? 128 : 255;
            colors[i] = alpha << 24 | ((c & 31) * 255 / 31) << 16 |
                        ((c >> 5 & 31) * 255 / 31) << 8 | (c >> 10 & 31) * 255 / 31;
        }
        if (cached) {
            small_palettes[slot].source = source;
            small_palettes[slot].alpha = m->opcode & 2;
            small_palettes[slot].valid = 1;
            small_palettes[slot].mapped = 0;
            small_palettes[slot].blocked = 0;
            memcpy(small_palettes[slot].colors, colors, 16 * sizeof *colors);
        }
    }
    return colors;
}

static int shared_bank(unsigned offset) {
    for (unsigned i = 0; i < SHARED_GROUPS; ++i)
        if (shared_groups[i].bank == (int)(offset / 256)) return 1;
    return 0;
}

static int palette(const gex_gpu_material *m, int raw, int allow_drain,
                   const uint32_t zero_mask[8],
                   char *error, size_t size) {
    unsigned count = (m->page >> 7 & 3) == 0 ? 16 : 256;
    WORK(pal4_calls, count == 16);
    WORK(pal8_calls, count == 256);
    unsigned source = m->clut * 32 & 0xfffff;
    if (raw && m->atlas == tracked_atlas)
        for (unsigned i = 0; i + count <= palette_used; i += count)
            if (!shared_bank(i) &&
                (palette_banks[i / 512] & (UINT32_C(1) << (i / 16 & 31))) &&
                palette_keys[i / 16].valid && palette_keys[i / 16].atlas == m->atlas &&
                palette_keys[i / 16].offset == source && palette_keys[i / 16].count == count &&
                palette_keys[i / 16].alpha == (m->opcode & 2) &&
                !memcmp(palette_keys[i / 16].zero_mask, zero_mask,
                        sizeof palette_keys[i / 16].zero_mask))
                return i / count;
    uint32_t decoded[256];
    uint32_t *colors = palette_colors(m, raw, count, decoded);
    if (raw) {
        if (colors != decoded) memcpy(decoded, colors, count * sizeof *colors);
        colors = decoded;
        for (unsigned i = 0; i < count; ++i)
            if (zero_mask[i / 32] & (UINT32_C(1) << (i & 31))) colors[i] = 0;
    }
    unsigned banks = count / 16;
    uint32_t mask = (UINT32_C(1) << banks) - 1;
    unsigned offset = 1024;
    for (unsigned i = 0; i + count <= palette_used; i += count)
        if (!shared_bank(i) && (palette_banks[i / 512] >> (i / 16 & 31) & mask) == mask &&
            !memcmp(palettes + i, colors, count * sizeof *colors)) {
            offset = i;
            goto remembered;
        }
    for (unsigned bank = 0; bank + banks <= 64; bank += banks)
        if (!(palette_banks[bank / 32] & (mask << (bank & 31)))) {
            offset = bank * 16;
            break;
        }
    if (offset + count > 1024) {
        if (!allow_drain) return -2;
        WORK(palette_drains, 1);
        WORK(pal8_drains, count == 256);
        if (finish(error, size)) return -1;
        offset = 0;
    }
    program_palette(offset, colors, count);
    /* PAL8 writes replace sixteen PAL4 banks. Source keys from retired
     * lists must not claim that an overwritten bank still holds its CLUT. */
    for (unsigned i = 0; i < 64; ++i)
        if (i * 16 < offset + count && i * 16 + palette_keys[i].count > offset)
            palette_keys[i].valid = 0;
    palette_banks[offset / 512] |= mask << (offset / 16 & 31);
    if (palette_used < offset + count) palette_used = offset + count;
remembered:
    palette_keys[offset / 16].atlas = m->atlas;
    palette_keys[offset / 16].offset = source;
    palette_keys[offset / 16].count = count;
    palette_keys[offset / 16].alpha = m->opcode & 2;
    if (raw) memcpy(palette_keys[offset / 16].zero_mask, zero_mask,
                    sizeof palette_keys[offset / 16].zero_mask);
    palette_keys[offset / 16].valid = raw && m->atlas == tracked_atlas;
    return (int)(offset / count);
}

static uint16_t *shared_color_slot(unsigned group, uint32_t color) {
    unsigned at = ((color ^ (color >> 16)) * UINT32_C(2654435761)) >> 23;
    while (shared_groups[group].lookup[at] &&
           shared_groups[group].colors[shared_groups[group].lookup[at] - 1] != color)
        at = (at + 1) & 511;
    return &shared_groups[group].lookup[at];
}

static int shared_palette(unsigned group, int allow_drain, char *error, size_t size) {
    if (shared_groups[group].bank < 0) {
        unsigned bank;
        for (bank = 0; bank < 64; bank += 16)
            if (!(palette_banks[bank / 32] & (UINT32_C(0xffff) << (bank & 31)))) break;
        if (bank == 64) {
            if (!allow_drain) return -2;
            WORK(palette_drains, 1);
            if (finish(error, size)) return -1;
            bank = 0;
        }
        shared_groups[group].bank = (int)(bank / 16);
        shared_groups[group].written = 0;
        palette_banks[bank / 32] |= UINT32_C(0xffff) << (bank & 31);
        if (palette_used < bank * 16 + 256) palette_used = bank * 16 + 256;
        for (unsigned i = bank; i < bank + 16; ++i) palette_keys[i].valid = 0;
    }
    unsigned offset = (unsigned)shared_groups[group].bank * 256;
    /* Existing indices never move. Appending unused entries cannot change
     * colors sampled by already queued draws in this group. */
    unsigned written = shared_groups[group].written;
    program_palette(offset + written, shared_groups[group].colors + written,
                    shared_groups[group].count - written);
    shared_groups[group].written = shared_groups[group].count;
    return shared_groups[group].bank;
}

static void defer_warming(const gex_gpu_material *m, int u0, int u1, int v0, int v1) {
    if (active->warm_pending) return;
    active->warm_material = *m;
    active->warm_region[0] = u0; active->warm_region[1] = u1;
    active->warm_region[2] = v0; active->warm_region[3] = v1;
    active->warm_pending = 1;
}

static remap_texture *remapped_region_impl(const gex_gpu_material *m, int u0, int u1, int v0, int v1,
                                     int warm, char *error, size_t size) {
    WORK(remap_lookups, 1);
    unsigned code = m->opcode & ~2u;
    if ((code != 0x64 && code != 0x2c) || !m->atlas ||
        (m->page >> 7 & 3) != 0 || m->atlas != tracked_atlas || !active) return NULL;
    unsigned left = (unsigned)u0 & ~7u, top = (unsigned)v0 & ~7u, dimension = 8;
    while (dimension < (unsigned)(u1 - u0) || dimension < (unsigned)(v1 - v0)) dimension *= 2;
    if (dimension > 128) {
        WORK(remap_bounds, 1);
        return NULL;
    }
    size_t bytes = dimension * dimension * (m->opcode & 2 ? 1 : 2);
    if (left + dimension < (unsigned)u1) left = (unsigned)u1 - dimension;
    if (top + dimension < (unsigned)v1) top = (unsigned)v1 - dimension;
    if (left + dimension > 256) left = 256 - dimension;
    if (top + dimension > 256) top = 256 - dimension;
    uint64_t used = ++cache_clock;
    unsigned hash = remap_hash(m->page, m->clut, m->opcode & 2);
    for (unsigned index = remap_heads[hash]; index; index = remaps[index - 1].next) {
        WORK(remap_checks, 1);
        remap_texture *t = &remaps[index - 1];
        if (t->valid && t->page == m->page && t->clut == m->clut &&
            t->alpha == (m->opcode & 2) && t->verified_atlas == m->atlas &&
            t->origin_u <= (unsigned)u0 && t->origin_v <= (unsigned)v0 &&
            t->origin_u + t->dimension >= (unsigned)u1 &&
            t->origin_v + t->dimension >= (unsigned)v1) {
            t->used = used;
            WORK(remap_hits, 1);
            return t;
        }
    }
    if (!warm || !active->warm_budget || dimension * dimension > active->warm_bytes) {
        WORK(remap_budget, 1);
        return NULL;
    }
    remap_texture *victim = NULL;
    for (unsigned i = 0; i < REMAP_COUNT; ++i) {
        remap_texture *t = &remaps[i];
        if (!victim || (!t->valid && victim->valid) ||
            (t->valid == victim->valid && t->used < victim->used)) victim = t;
    }
    if ((command_size || saved_count) &&
        (victim->image || remap_bytes + bytes > REMAP_BYTES ||
         cache_bytes + bytes > CACHE_BYTES ||
         cache_bytes - retained_snapshot_bytes + bytes > CACHE_BYTES - SNAPSHOT_BYTES)) {
        WORK(remap_queued, 1);
        defer_warming(m, u0, u1, v0, v1);
        return NULL;
    }
    if (victim->image) {
        if (finish(error, size)) return NULL;
        release_remap(victim);
        if (prune_shared_colors(error, size)) return NULL;
    }
    unsigned base = (m->page & 15) * 128 + (m->page & 16 ? 0x80000 : 0);
    unsigned needed = 0;
    unsigned group = SHARED_GROUPS;
    uint8_t map[16];
    uint16_t direct[16];
    if (!(m->opcode & 2)) {
        for (unsigned i = 0; i < 16; ++i) {
            unsigned color = word(m->atlas + ((m->clut * 32 + i * 2) & 0xffffe));
            direct[i] = color ? (uint16_t)argb1555(color) : 0;
        }
    } else {
        uint32_t decoded[256];
        const uint32_t *colors = palette_colors(m, 1, 16, decoded);
        for (unsigned y = 0; y < dimension; ++y)
            for (unsigned x = 0; x < dimension; x += 2) {
                unsigned u = left + x;
                unsigned at = (base + (top + y) * 2048 + u / 2) & 0xfffff;
                unsigned packed = m->atlas[at];
                if (u & 1) packed = packed >> 4 | (unsigned)m->atlas[at + 1] << 4;
                needed |= 1u << (packed & 15);
                needed |= 1u << (packed >> 4 & 15);
            }
        unsigned source = m->clut * 32 & 0xfffff;
        unsigned slot = (uint32_t)((source / 32) * UINT32_C(2654435761)) >> 24;
    retry_colors:
        group = SHARED_GROUPS;
        if (small_palettes[slot].blocked &&
            (needed & small_palettes[slot].blocked) == small_palettes[slot].blocked)
            goto color_space_failed;
        if ((small_palettes[slot].mapped & needed) == needed) {
            group = small_palettes[slot].group;
            memcpy(map, small_palettes[slot].index_map, sizeof map);
        } else {
            for (unsigned g = 0; g < SHARED_GROUPS; ++g) {
                unsigned count = shared_groups[g].count;
                uint32_t added[16];
                unsigned added_index[16];
                unsigned extra = 0, next_index = 0;
                for (unsigned i = 0; i < 16; ++i) {
                    if (!(needed & (1u << i))) continue;
                    unsigned indexed = *shared_color_slot(g, colors[i]);
                    unsigned index = indexed ? indexed - 1 : count;
                    if (index == count) {
                        unsigned j = 0;
                        while (j < extra && added[j] != colors[i]) ++j;
                        if (j < extra) index = added_index[j];
                        else {
                            while (next_index < 256 && (shared_groups[g].occupied[next_index / 32] &
                                                        (UINT32_C(1) << (next_index & 31))))
                                ++next_index;
                            if (next_index == 256) {
                                extra = 17;
                                break;
                            }
                            index = next_index++;
                            added_index[extra] = index;
                            added[extra++] = colors[i];
                        }
                    }
                    map[i] = (uint8_t)index;
                }
                if (extra > 16) continue;
                for (unsigned i = 0; i < extra; ++i) {
                    unsigned index = added_index[i];
                    shared_groups[g].colors[index] = added[i];
                    shared_groups[g].occupied[index / 32] |= UINT32_C(1) << (index & 31);
                    *shared_color_slot(g, added[i]) = (uint16_t)(index + 1);
                    if (shared_groups[g].count <= index) shared_groups[g].count = index + 1;
                    if (index < shared_groups[g].written) shared_groups[g].written = index;
                }
                group = g;
                break;
            }
        }
    color_space_failed:
        if (group == SHARED_GROUPS) {
            if (!command_size && !saved_count && active->reclaim_budget) {
                remap_texture *cold = NULL;
                for (unsigned i = 0; i < REMAP_COUNT; ++i)
                    if (&remaps[i] != victim && remaps[i].valid &&
                        remaps[i].group < SHARED_GROUPS &&
                        remaps[i].used < active->reclaim_cutoff &&
                        (!cold || remaps[i].used < cold->used))
                        cold = &remaps[i];
                if (cold) {
                    --active->reclaim_budget;
                    if (finish(error, size)) return NULL;
                    release_remap(cold);
                    WORK(remap_reclaims, 1);
                    if (prune_shared_colors(error, size)) return NULL;
                    goto retry_colors;
                }
            }
            small_palettes[slot].blocked = needed;
            defer_warming(m, u0, u1, v0, v1);
            WORK(remap_colors, 1);
            return NULL;
        }
        unsigned first = 0;
        while (!(needed & (1u << first))) ++first;
        /* Unused source indices must not pin unused shared colors at pruning. */
        for (unsigned i = 0; i < 16; ++i)
            if (!(needed & (1u << i))) map[i] = map[first];
        small_palettes[slot].mapped = needed;
        small_palettes[slot].group = group;
        memcpy(small_palettes[slot].index_map, map, sizeof map);
    }
    --active->warm_budget;
    active->warm_bytes -= dimension * dimension;
    while (remap_bytes + bytes > REMAP_BYTES) {
        remap_texture *oldest = NULL;
        for (unsigned i = 0; i < REMAP_COUNT; ++i)
            if (remaps[i].image && (!oldest || remaps[i].used < oldest->used)) oldest = &remaps[i];
        if (!oldest) {
            fail(error, size, "PVR remap exceeds cache budget");
            return NULL;
        }
        if (finish(error, size)) return NULL;
        release_remap(oldest);
    }
    if (reserve_texture(NULL, bytes, error, size)) return NULL;
    victim->image = allocate_texture(NULL, bytes, "remap", m->page,
                                     m->clut, m->opcode, error, size);
    if (!victim->image) return NULL;
    WORK(remap_builds, 1);
    for (unsigned y = 0; y < dimension; y += 2)
        for (unsigned x = 0; x < dimension; x += 2) {
            unsigned u = left + x;
            unsigned at = (base + (top + y) * 2048 + u / 2) & 0xfffff;
            unsigned a = m->atlas[at], b = m->atlas[at + 2048];
            if (u & 1) {
                a = a >> 4 | (unsigned)m->atlas[at + 1] << 4;
                b = b >> 4 | (unsigned)m->atlas[at + 2049] << 4;
            }
            unsigned at_out = twiddle(y) | twiddle(x) << 1;
            uint32_t *out = victim->image;
            if (group == SHARED_GROUPS) {
                out[at_out / 2] = direct[a & 15] | (uint32_t)direct[b & 15] << 16;
                out[at_out / 2 + 1] = direct[a >> 4 & 15] | (uint32_t)direct[b >> 4 & 15] << 16;
            } else {
                /* PAL8 twiddling makes a 2x2 texel block one aligned word. */
                out[at_out / 4] = map[a & 15] | (uint32_t)map[b & 15] << 8 |
                    (uint32_t)map[a >> 4 & 15] << 16 | (uint32_t)map[b >> 4 & 15] << 24;
            }
        }
    victim->page = m->page;
    victim->clut = m->clut;
    victim->alpha = m->opcode & 2;
    victim->verified_atlas = m->atlas;
    victim->group = group;
    victim->header_format = -1;
    victim->origin_u = left; victim->origin_v = top; victim->dimension = dimension;
    if (group != SHARED_GROUPS) memcpy(victim->index_map, map, sizeof map);
    victim->source_indices = (uint16_t)needed;
    victim->valid = 1;
    victim->used = used;
    victim->next = remap_heads[hash];
    remap_heads[hash] = (uint16_t)(victim - remaps + 1);
    remap_bytes += bytes;
    cache_bytes += bytes;
    return victim;
}

static remap_texture *remapped_region(const gex_gpu_material *m, int u0, int u1, int v0, int v1,
                                     int warm, char *error, size_t size) {
    PROFILE_BEGIN();
    remap_texture *result = remapped_region_impl(m, u0, u1, v0, v1, warm, error, size);
    PROFILE_END(remap);
    return result;
}

static float texture_coordinate(draw_texture t, float coordinate, int vertical) {
    return t.converted ?
        (coordinate * 256 - (vertical ? t.converted->origin_v : t.converted->origin_u)) /
        t.converted->dimension : t.region ?
        (coordinate * 256 - (vertical ? t.region->origin_v : t.region->origin_u)) /
        t.region->dimension : coordinate;
}

static int prune_shared_colors(char *error, size_t size) {
    if (!shared_dirty || command_size || saved_count) return 0;
    if (finish(error, size)) return -1;
    uint32_t live[SHARED_GROUPS][8] = {{0}};
    for (unsigned i = 0; i < REMAP_COUNT; ++i)
        if (remaps[i].valid && remaps[i].group < SHARED_GROUPS)
            for (unsigned j = 0, mask = remaps[i].source_indices; mask; ++j, mask >>= 1) {
                if (!(mask & 1)) continue;
                unsigned index = remaps[i].index_map[j];
                live[remaps[i].group][index / 32] |= UINT32_C(1) << (index & 31);
            }
    for (unsigned i = 0; i < 256; ++i) {
        small_palettes[i].blocked = 0;
        if (small_palettes[i].mapped)
            for (unsigned j = 0, mask = small_palettes[i].mapped; mask; ++j, mask >>= 1) {
                if (!(mask & 1)) continue;
                unsigned index = small_palettes[i].index_map[j];
                if (!(live[small_palettes[i].group][index / 32] & (UINT32_C(1) << (index & 31)))) {
                    small_palettes[i].mapped = 0;
                    break;
                }
            }
    }
    for (unsigned i = 0; i < SHARED_GROUPS; ++i) {
        memcpy(shared_groups[i].occupied, live[i], sizeof live[i]);
        memset(shared_groups[i].lookup, 0, sizeof shared_groups[i].lookup);
        for (unsigned j = 0; j < shared_groups[i].count; ++j)
            if (live[i][j / 32] & (UINT32_C(1) << (j & 31)))
                *shared_color_slot(i, shared_groups[i].colors[j]) = (uint16_t)(j + 1);
    }
    shared_dirty = 0;
    return 0;
}

static int native_texture_region_impl(const gex_gpu_material *m, int u0, int u1, int v0, int v1,
                                 draw_texture *t, int *bank, char *error, size_t size) {
    uint32_t zero_mask[8] = {0};
    int masked = 0;
    unsigned format = m->page >> 7 & 3;
    if (format == 2 && !(m->opcode & 2)) {
        unsigned left, top, dimension;
        snapshot_bounds(u0, u1, v0, v1, &left, &top, &dimension);
        t->converted = find_snapshot(m, left, top, dimension, zero_mask);
        if (t->converted) {
            *bank = 0;
            return 0;
        }
    }
    if (format < 2) {
        /* Software discards indexed colors after modulation, but PVR applies
         * tint after palette lookup. Preserve those discarded indices in alpha. */
        unsigned count = format == 0 ? 16 : 256;
        for (unsigned i = 0; i < count; ++i) {
            unsigned c = word(m->atlas + ((m->clut * 32 + i * 2) & 0xffffe));
            if (modulates_to_zero(c, m)) {
                zero_mask[i / 32] |= UINT32_C(1) << (i & 31);
                masked = 1;
            }
        }
    }
    char remap_error[256];
    remap_error[0] = 0;
    /* Remapped images and shared palettes have tint-independent identities. */
    if (format == 0 && active && !masked) {
        ++active->pal4_draws;
        /* Background retention can lower the live draw count without making
         * existing remaps obsolete. Pressure gates building, not cache hits. */
        t->region = remapped_region(m, u0, u1, v0, v1, active->warm_budget != 0,
                                   remap_error, sizeof remap_error);
    }
    if (!t->region && remap_error[0]) {
        return fail(error, size, remap_error);
    }
    if (!t->region) {
        gex_gpu_material neutral = *m;
        neutral.rgb = 0x808080;
        char page_error[256] = {0};
        t->page = texture_region_impl(&neutral, 1, u0, u1, v0, v1, 1,
                                      page_error, sizeof page_error);
        if (!t->page) {
            if (page_error[0]) return fail(error, size, page_error);
            t->converted = snapshot_texture(m, u0, u1, v0, v1, zero_mask, error, size);
            if (!t->converted) return -1;
            *bank = 0;
            return 0;
        }
    }
    *bank = t->region ? (t->region->group == SHARED_GROUPS ? 0 :
                        shared_palette(t->region->group, 0, error, size)) :
        format < 2 ? palette(m, 1, 0, zero_mask, error, size) : 0;
    if (*bank == -2) {
        t->converted = snapshot_texture(m, u0, u1, v0, v1, zero_mask, error, size);
        if (!t->converted) return -1;
        t->page = NULL;
        t->region = NULL;
        *bank = 0;
    }
    return *bank < 0 ? -1 : 0;
}

static int native_texture_region(const gex_gpu_material *m, int u0, int u1, int v0, int v1,
                                 draw_texture *t, int *bank, char *error, size_t size) {
    PROFILE_BEGIN();
    int result = native_texture_region_impl(m, u0, u1, v0, v1, t, bank, error, size);
    PROFILE_END(texture);
    return result;
}

static int append(const pvr_poly_hdr_t *header, const pvr_vertex_t v[4],
                  char *error, size_t size) {
    command_clear_only = 0;
    int same_header = command_size && !memcmp(header, &last_header, sizeof *header);
    size_t bytes = (same_header ? 0 : sizeof *header) + sizeof(pvr_vertex_t) * 4;
    /* Palette selectors remain valid across command-buffer drains. */
    if (command_size + bytes > command_capacity &&
        command_size + bytes <= COMMAND_MAX_BYTES) {
        size_t capacity = command_capacity * 2;
        if (capacity > COMMAND_MAX_BYTES) capacity = COMMAND_MAX_BYTES;
        uint8_t *expanded = memalign(32, capacity);
        if (!expanded) return fail(error, size, "cannot expand PVR command buffer");
        memcpy(expanded, commands, command_size);
        free(commands);
        commands = expanded;
        command_capacity = capacity;
    }
    if (command_size + bytes > command_capacity) {
        unsigned used = palette_used;
        uint32_t banks[2];
        int shared_banks[SHARED_GROUPS];
        for (unsigned i = 0; i < SHARED_GROUPS; ++i) shared_banks[i] = shared_groups[i].bank;
        memcpy(banks, palette_banks, sizeof banks);
        if (finish(error, size)) return -1;
        palette_used = used;
        memcpy(palette_banks, banks, sizeof banks);
        for (unsigned i = 0; i < SHARED_GROUPS; ++i) shared_groups[i].bank = shared_banks[i];
        /* Every new TA scene needs its own first header. */
        same_header = 0;
    }
    WORK(headers, 1);
    WORK(same_headers, same_header);
    if (!same_header) {
        memcpy(commands + command_size, header, sizeof *header);
        command_size += sizeof *header;
        last_header = *header;
    }
    memcpy(commands + command_size, v, sizeof(pvr_vertex_t) * 4);
    command_size += sizeof(pvr_vertex_t) * 4;
    if (active_transient_pin && active_transient_pin->texture)
        active_transient_pin->texture->current = 1;
    return 0;
}

static int append_replacement(const pvr_poly_hdr_t *header, const pvr_vertex_t v[4],
                              char *error, size_t size) {
    /* Only opaque full-target draws may retire earlier commands. Resource
     * fences remain intact; later partial draws still preserve this image. */
    WORK(discarded_bytes, command_size);
    command_size = 0;
    clear_current_transients();
    release_transients();
    clear_current_texture_reads();
    command_replaces = 1;
    return append(header, v, error, size);
}

static int append_texture(const pvr_poly_hdr_t *header, const pvr_vertex_t v[4],
                          texture *t, remap_texture *region,
                          unsigned u0, unsigned u1, unsigned v0, unsigned v1,
                          char *error, size_t size) {
    if (append(header, v, error, size)) return -1;
    if (region) {
        region->queued = 1;
        current_remaps[region - remaps] = 1;
    }
    if (!t) return 0;
    unsigned format = t->page >> 7 & 3;
    unsigned begin = format == 0 ? u0 / 2 : format == 1 ? u0 : u0 * 2;
    unsigned end = format == 0 ? (u1 + 1) / 2 : format == 1 ? u1 : u1 * 2;
    texture_read *read = &current_reads[t - cache];
    if (!read->end) {
        read->begin = begin; read->end = end;
        read->top = v0; read->bottom = v1;
    } else {
        if (begin < read->begin) read->begin = begin;
        if (end > read->end) read->end = end;
        if (v0 < read->top) read->top = v0;
        if (v1 > read->bottom) read->bottom = v1;
    }
    /* Record after append: a capacity drain may have retired earlier reads. */
    if (!t->read_end) {
        t->read_begin = begin; t->read_end = end;
        t->read_top = v0; t->read_bottom = v1;
    } else {
        if (begin < t->read_begin) t->read_begin = begin;
        if (end > t->read_end) t->read_end = end;
        if (v0 < t->read_top) t->read_top = v0;
        if (v1 > t->read_bottom) t->read_bottom = v1;
    }
    return 0;
}

static int native_clear(void *data, char *error, size_t size) {
    NATIVE_DRAW_BEGIN();
    target *t = data;
#if GEX_PROFILE_RENDERER
    t->draw_cpu_begin = native_draw_begin;
#endif
    unsigned previous_draws = t->pal4_draws;
    t->pal4_draws = 0;
    t->pending_key.valid = 0;
    if (background_stop(t, error, size) || select_target(t, error, size)) return -1;
    t->warm_budget = t->background_count >= 64 || previous_draws > 64 ? WARM_REGIONS : 0;
    t->warm_bytes = WARM_BYTES;
    t->reclaim_budget = WARM_REGIONS;
    /* Only reclaim regions unused throughout the preceding frame. */
    t->reclaim_cutoff = t->warm_previous;
    t->warm_previous = cache_clock;
    if (t->warm_budget) {
        /* Vacant slots let later draws warm without evicting queued images.
         * Retire only invalid regions, before this frame queues its clear. */
        if (!command_size && !saved_count) {
            for (unsigned reclaimed = 0; reclaimed < WARM_REGIONS; ++reclaimed) {
                remap_texture *stale = NULL;
                for (unsigned i = 0; i < REMAP_COUNT; ++i)
                    if (remaps[i].image && !remaps[i].valid &&
                        (!stale || remaps[i].used < stale->used))
                        stale = &remaps[i];
                if (!stale) break;
                if (!reclaimed) {
                    if (finish(error, size)) return -1;
                    if (pvr_wait_render_done())
                        return hardware_fail(error, size, "PVR stale remap completion failed");
                }
                release_remap(stale);
                WORK(remap_reclaims, 1);
            }
        }
        if (prune_shared_colors(error, size)) return -1;
        if (t->warm_pending) {
            t->warm_pending = 0;
            if (t->warm_material.atlas == tracked_atlas) {
                char warm_error[256] = {0};
                remapped_region(&t->warm_material, t->warm_region[0], t->warm_region[1],
                                t->warm_region[2], t->warm_region[3], 1,
                                warm_error, sizeof warm_error);
                if (warm_error[0]) return fail(error, size, warm_error);
            }
        }
        for (unsigned i = 0; i < WARM_REGIONS && t->background_count &&
             !t->background_valid && t->warm_budget; ++i) {
            const background_polygon *p = &t->background[t->warm_at++ % t->background_count];
            if (!p->prepared_valid) continue;
            char warm_error[256];
            warm_error[0] = 0;
            remapped_region(&p->material, p->region[0], p->region[1], p->region[2],
                            p->region[3], 1, warm_error, sizeof warm_error);
            if (warm_error[0]) return fail(error, size, warm_error);
        }
    }
    if (t->owned) {
        if (!t->background) t->background = calloc(BACKGROUND_POLYGONS, sizeof *t->background);
        if (!t->background) return fail(error, size, "cannot allocate PVR background draw records");
        if (t->background_revision != atlas_revision) {
            t->background_valid = 0;
            t->background_count = 0;
        }
        t->background_revision = atlas_revision;
        if (!t->background_count) memset(t->dependencies, 0, sizeof t->dependencies);
        t->background_at = 0;
        t->background_mode = t->background_count ?
            (t->background_valid ? BACKGROUND_REUSE : BACKGROUND_VERIFY) : BACKGROUND_RECORD;
    }
    pvr_poly_cxt_t c;
    pvr_poly_hdr_t header;
    pvr_vertex_t v[4];
    context(&c, NULL, 0, 0, 0);
    c.blend.src = PVR_BLEND_ONE; c.blend.dst = PVR_BLEND_ZERO;
    pvr_poly_compile(&header, &c);
    vertices(v, 0, 0, 320, 240, 0, 0, 0, 0, 0xff000000);
    int result = append_replacement(&header, v, error, size);
    if (!result) command_clear_only = 1;
    NATIVE_DRAW_END(clear);
    return result;
}

static int native_copy(void *destination, void *source, unsigned shade, char *error, size_t size) {
    target *src = source;
    target *dst = destination;
    if (!src || !dst) return fail(error, size, "invalid PVR framebuffer copy target");
    if (shade > 255) return fail(error, size, "invalid PVR framebuffer shade");
    if (src == dst && shade == 255) return 0;
    if (background_stop(src, error, size) ||
        background_stop(dst, error, size)) return -1;
    if (!shade) {
        return native_clear(dst, error, size);
    }
    if (shade < 255 && (src->saved_size || (active == src && command_size))) {
        if (native_copy(dst, src, 255, error, size) ||
            select_target(dst, error, size)) return -1;
        pvr_poly_cxt_t c;
        pvr_poly_hdr_t header;
        pvr_vertex_t v[4];
        context(&c, NULL, 0, 0, 0);
        pvr_poly_compile(&header, &c);
        vertices(v, 0, 0, 320, 240, 0, 0, 0, 0, (255 - shade) << 24);
        return append(&header, v, error, size);
    }
    if (shade == 255 && src != dst && src->saved_size) {
        if (dst == active) {
            if (src->saved_size > command_capacity) {
                uint8_t *expanded = memalign(32, src->saved_size);
                if (!expanded) return fail(error, size, "cannot restore PVR picture commands");
                free(commands);
                commands = expanded;
                command_capacity = src->saved_size;
            }
            if (!src->saved_replaces) {
                uint32_t *out = sq_lock(current_image(dst));
                sq_fast_cpy(out, current_image(src), TW * TH * 2 / 32);
                sq_wait();
                sq_unlock();
            }
            memcpy(commands, src->saved_commands, src->saved_size);
            command_size = src->saved_size;
            last_header = src->saved_header;
            command_replaces = src->saved_replaces;
            command_clear_only = src->saved_clear_only;
            clear_current_transients();
            for (size_t i = 0; i < src->saved_transient_count; ++i)
                src->saved_transients[i]->current = 1;
            memcpy(current_reads, src->saved_reads, sizeof current_reads);
            memcpy(current_remaps, src->saved_remaps, sizeof current_remaps);
            rebuild_texture_reads();
            release_transients();
            return 0;
        }
        {
            uint8_t *snapshot = malloc(src->saved_size);
            if (!snapshot) return fail(error, size, "cannot defer PVR picture snapshot");
            transient_texture **snapshot_transients = NULL;
            if (clone_saved_transients(src, &snapshot_transients, error, size)) {
                free(snapshot);
                return -1;
            }
            memcpy(snapshot, src->saved_commands, src->saved_size);
            if (!src->saved_replaces) {
                uint32_t *out = sq_lock(current_image(dst));
                sq_fast_cpy(out, current_image(src), TW * TH * 2 / 32);
                sq_wait();
                sq_unlock();
            }
            if (dst->saved_size) {
                release_saved_transients(dst, 0);
                free(dst->saved_commands);
                --saved_count;
            }
            dst->saved_commands = snapshot;
            dst->saved_size = src->saved_size;
            dst->saved_header = src->saved_header;
            dst->saved_replaces = src->saved_replaces;
            dst->saved_clear_only = src->saved_clear_only;
            dst->saved_transients = snapshot_transients;
            dst->saved_transient_count = src->saved_transient_count;
            memcpy(dst->saved_reads, src->saved_reads, sizeof dst->saved_reads);
            memcpy(dst->saved_remaps, src->saved_remaps, sizeof dst->saved_remaps);
            ++saved_count;
            rebuild_texture_reads();
            release_transients();
            return 0;
        }
    }
    if (src->saved_size && (select_target(src, error, size) || finish(error, size)))
        return -1;
    if (shade == 255 && src != dst && active == src && command_size) {
        uint8_t *snapshot;
        transient_texture **snapshot_transients = NULL;
        size_t snapshot_transient_count = 0;
        snapshot = malloc(command_size);
        if (!snapshot) return fail(error, size, "cannot defer PVR picture snapshot");
        if (capture_transients(&snapshot_transients, &snapshot_transient_count, error, size)) {
            free(snapshot);
            return -1;
        }
        memcpy(snapshot, commands, command_size);
        if (!command_replaces) {
            uint32_t *out = sq_lock(current_image(dst));
            sq_fast_cpy(out, current_image(src), TW * TH * 2 / 32);
            sq_wait();
            sq_unlock();
        }
        if (dst->saved_size) {
            release_saved_transients(dst, 0);
            free(dst->saved_commands);
            dst->saved_commands = NULL;
            dst->saved_size = 0;
            --saved_count;
            release_transients();
            rebuild_texture_reads();
        }
        dst->saved_commands = snapshot;
        dst->saved_size = command_size;
        dst->saved_header = last_header;
        dst->saved_replaces = command_replaces;
        dst->saved_clear_only = command_clear_only;
        dst->saved_transients = snapshot_transients;
        dst->saved_transient_count = snapshot_transient_count;
        memcpy(dst->saved_reads, current_reads, sizeof current_reads);
        memcpy(dst->saved_remaps, current_remaps, sizeof current_remaps);
        ++saved_count;
        return 0;
    }
    if ((active == src && command_size && finish(error, size)) ||
        select_target(dst, error, size)) return -1;
    transient_texture *snapshot = snapshot_picture_image(src, error, size);
    if (!snapshot) return -1;
    transient_pin pin = {snapshot, active_transient_pin};
    active_transient_pin = &pin;
    pvr_poly_hdr_t header;
    pvr_vertex_t v[4];
    picture_quad(&header, v, snapshot->image[0], shade);
    int result = append_replacement(&header, v, error, size);
    active_transient_pin = pin.previous;
    if (result) release_transients();
    return result;
}

static int native_blit(void *data, int x, int y, int width, int height,
                       const uint8_t *pixels, int pitch, int key, char *error, size_t size) {
    if (!pixels || width <= 0 || height <= 0 || width > TW || height > TH ||
        pitch < width * 2 || x < 0 || y < 0 || x + width > 320 || y + height > 240)
        return fail(error, size, "invalid PVR asset blit");
    if (background_stop(data, error, size) || select_target(data, error, size)) return -1;
    unsigned tw = 8, th = 8;
    while (tw < (unsigned)width) tw *= 2;
    while (th < (unsigned)height) th *= 2;
    transient_texture *texture = calloc(1, sizeof *texture);
    if (!texture) return fail(error, size, "cannot allocate PVR asset texture record");
    texture->next = transients;
    transients = texture;
    transient_pin pin = {texture, active_transient_pin};
    active_transient_pin = &pin;
    size_t bytes = tw * th * sizeof(uint16_t);
    texture->image[0] = allocate_texture(NULL, bytes, "asset-blit", 0, 0, 0, error, size);
    if (!texture->image[0]) {
        active_transient_pin = pin.previous;
        release_transients();
        return -1;
    }
    uint16_t *out = texture->image[0];
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col) {
            unsigned c = word(pixels + row * pitch + col * 2);
            out[row * tw + col] = (int)c == key ? 0 : (uint16_t)argb1555(c);
        }
    pvr_poly_cxt_t c;
    pvr_poly_hdr_t header;
    pvr_vertex_t v[4];
    context(&c, out, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED, tw, th);
    pvr_poly_compile(&header, &c);
    vertices(v, (float)x, (float)y, (float)width, (float)height,
             0, 0, (float)width / tw, (float)height / th, 0xffffffff);
    int result = append(&header, v, error, size);
    active_transient_pin = pin.previous;
    if (result) release_transients();
    return result;
}

static int native_present(void *data, char *error, size_t size) {
    target *t = data;
    if (background_stop(t, error, size) || select_target(t, error, size)) return -1;
    if (!command_size && last_presented != t) {
        if (t->known_clear) {
            if (native_clear(t, error, size)) return -1;
        } else {
            pvr_poly_hdr_t header;
            pvr_vertex_t v[4];
            picture_quad(&header, v, current_image(t), 255);
            if (append_replacement(&header, v, error, size)) return -1;
        }
    }
    uint64_t deadline = timer_us_gettime64() + 100000;
    while (presentation_paced && (unsigned)pvr_get_vbl_count() - last_present_vblank < 1) {
        if (timer_us_gettime64() >= deadline)
            return fail(error, size, "PVR presentation pacing timed out");
        thd_sleep(1);
    }
    if (command_size) {
        int result = flush_commands(0, error, size);
        if (result || display_wait(PVR_GET(PVR_RENDER_ADDR), error, size)) return -1;
        last_presented = t;
    } else {
        unsigned vblank = (unsigned)pvr_get_vbl_count();
        deadline = timer_us_gettime64() + 100000;
        while ((unsigned)pvr_get_vbl_count() == vblank) {
            if (timer_us_gettime64() >= deadline)
                return fail(error, size, "PVR presentation pacing timed out");
            thd_sleep(1);
        }
    }
    last_present_vblank = (unsigned)pvr_get_vbl_count();
    presentation_paced = 1;
#if GEX_PROFILE_RENDERER
    if (profile_display(error, size)) return -1;
    uint64_t cpu, now = gex_packet_gpu_profile_clock(&cpu);
    if (timing.last_time) {
        uint64_t dt = now - timing.last_time;
        timing.intervals[timing.samples++] = dt;
        timing.interval_sum += dt;
        uint64_t elapsed_cpu = cpu >= timing.last_cpu ? cpu - timing.last_cpu : 0;
        timing.cpu_sum += elapsed_cpu;
        if (elapsed_cpu > timing.cpu_max) {
            timing.cpu_max = elapsed_cpu;
            timing.cpu_max_draw = t->draw_cpu_begin >= timing.last_cpu &&
                                  t->draw_cpu_begin <= cpu ? cpu - t->draw_cpu_begin : 0;
            timing.max_native = (struct native_cpu){
                native_draw_cpu.clear - timing.last_native.clear,
                native_draw_cpu.atlas - timing.last_native.atlas,
                native_draw_cpu.packet - timing.last_native.packet,
                native_draw_cpu.polygon - timing.last_native.polygon};
            timing.cpu_max_native = timing.max_native.clear + timing.max_native.atlas +
                                    timing.max_native.packet + timing.max_native.polygon;
            timing.max_work = (struct frame_work){
                work.polygons - timing.last_work.polygons,
                work.remap_builds - timing.last_work.remaps,
                work.page_builds - timing.last_work.pages,
                work.refresh_bytes - timing.last_work.refresh,
                work.palette_drains - timing.last_work.drains};
        }
        timing.over_budget += dt > 33333;
    } else {
        memset(&work, 0, sizeof work);
    }
    timing.last_time = now;
    timing.last_cpu = cpu;
    timing.last_native = native_draw_cpu;
    if (timing.samples == 30) {
        for (unsigned i = 1; i < timing.samples; ++i) {
            uint64_t value = timing.intervals[i];
            unsigned j = i;
            while (j && timing.intervals[j - 1] > value) {
                timing.intervals[j] = timing.intervals[j - 1];
                --j;
            }
            timing.intervals[j] = value;
        }
        if (report_count < sizeof reports / sizeof *reports) {
            reports[report_count].end = timing.frame;
            reports[report_count].over_budget = timing.over_budget;
            reports[report_count].mean = timing.interval_sum / timing.samples;
            reports[report_count].p95 = timing.intervals[28];
            reports[report_count].maximum = timing.intervals[29];
            reports[report_count].cpu = timing.cpu_sum / timing.samples;
            reports[report_count].cpu_max = timing.cpu_max;
            reports[report_count].cpu_max_draw = timing.cpu_max_draw;
            reports[report_count].cpu_max_native = timing.cpu_max_native;
            reports[report_count].max_native = timing.max_native;
            reports[report_count].max_work = timing.max_work;
            reports[report_count].work = work;
            ++report_count;
            if (work.allocation_retries || timing.over_budget >= 5)
                report_profile();
            report_count = 0;
        } else {
            ++dropped_reports;
        }
        memset(&work, 0, sizeof work);
        timing.samples = timing.over_budget = 0;
        timing.interval_sum = timing.cpu_sum = timing.cpu_max = timing.cpu_max_draw = 0;
        timing.cpu_max_native = 0;
        memset(&timing.max_native, 0, sizeof timing.max_native);
        memset(&timing.max_work, 0, sizeof timing.max_work);
    }
    timing.last_work = (struct frame_work){
        work.polygons, work.remap_builds, work.page_builds, work.refresh_bytes, work.palette_drains};
    ++timing.frame;
#endif
    static int shown;
    if (!shown) {
        fprintf(stderr, "[dreamcast] presented first GPU framebuffer: 320x240; readbacks forbidden\n");
        shown = 1;
    }
    return 0;
}

static int native_movie(void *data, const uint8_t *pixels, int width, int height, int pitch,
                        char *error, size_t size) {
    if (!pixels || width <= 0 || height <= 0 || width > TW || height > TH || pitch < width * 2)
        return fail(error, size, "PVR movie texture dimensions are unsupported");
    target *t = data;
    if (background_stop(t, error, size) || select_target(t, error, size)) return -1;
    t->background_valid = 0;
    t->background_count = t->background_at = 0;
    t->pending_key.valid = 0;
    if (native_clear(t, error, size)) return -1;
    transient_texture *frame = calloc(1, sizeof *frame);
    if (!frame) return fail(error, size, "cannot allocate PVR asset texture record");
    frame->next = transients;
    transients = frame;
    transient_pin pin = {frame, active_transient_pin};
    active_transient_pin = &pin;
    frame->image[0] = allocate_texture(NULL, TW * TH * 2, "movie", 0, 0, 0, error, size);
    if (!frame->image[0]) {
        active_transient_pin = pin.previous;
        release_transients();
        return -1;
    }
    if (!((uintptr_t)pixels & 7) && !(pitch & 7) && !((width * 2) & 31)) {
        uint32_t *out = sq_lock(frame->image[0]);
        for (int row = 0; row < height; ++row)
            sq_fast_cpy(out + row * (TW / 2), pixels + row * pitch, (size_t)width * 2 / 32);
        sq_wait();
        sq_unlock();
    } else {
        for (int row = 0; row < height; ++row)
            copy_vram_words((uint16_t *)frame->image[0] + row * TW,
                            pixels + row * pitch, (size_t)width);
    }
    float w = 320, h = (float)height * 320 / width;
    if (h > 240) { h = 240; w = (float)width * 240 / height; }
    pvr_poly_cxt_t c;
    pvr_poly_hdr_t header;
    pvr_vertex_t v[4];
    context(&c, frame->image[0], PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, TW, TH);
    c.blend.src = PVR_BLEND_ONE; c.blend.dst = PVR_BLEND_ZERO;
    pvr_poly_compile(&header, &c);
    vertices(v, (320 - w) * 0.5f, (240 - h) * 0.5f, w, h,
             0, 0, (float)width / TW, (float)height / TH, 0xffffffff);
    int result = append(&header, v, error, size);
    active_transient_pin = pin.previous;
    if (result) {
        release_transients();
        return -1;
    }
    return native_present(data, error, size);
}

static void native_bind(void *data, const uint8_t *atlas, unsigned plane) {
    target *t = data;
    if (plane > 1) { fprintf(stderr, "[packet-gpu] invalid picture plane\n"); abort(); }
    if (t->atlas != atlas || t->plane != plane || !t->owned || pictures[plane] != t) {
        char error[256];
        if (active && background_replay(active, error, sizeof error)) {
            fprintf(stderr, "[packet-gpu] %s\n", error);
            abort();
        }
        if (active) active->background_mode = BACKGROUND_IDLE;
        ++atlas_revision;
        t->background_valid = 0;
        t->background_count = 0;
    }
    t->atlas = atlas; t->plane = plane; t->owned = 1;
    pictures[plane] = t;
}

static unsigned clip_polygon(const gex_gpu_vertex *input, unsigned count,
                             gex_gpu_vertex *output, unsigned axis, float edge, int greater) {
    unsigned used = 0;
    for (unsigned i = 0; i < count; ++i) {
        gex_gpu_vertex a = input[i], b = input[(i + 1) % count];
        float av = axis == 0 ? a.x : axis == 1 ? a.y : axis == 2 ? a.u : a.v;
        float bv = axis == 0 ? b.x : axis == 1 ? b.y : axis == 2 ? b.u : b.v;
        int ai = greater ? av >= edge : av <= edge;
        int bi = greater ? bv >= edge : bv <= edge;
        if (ai) output[used++] = a;
        if (ai != bi) {
            float t = (edge - av) / (bv - av);
            output[used++] = (gex_gpu_vertex){a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
        }
    }
    return used;
}

static uint32_t polygon_header(const gex_gpu_material *m, draw_texture tx, int bank,
                               unsigned pass, pvr_poly_hdr_t *header) {
    unsigned code = m->opcode & ~2u, format = m->page >> 7 & 3;
    int textured = code == 0x64 || code == 0x2c;
    int fmt = format == 0 ? PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_4BPP_PAL(bank) :
              format == 1 ? PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(bank) :
                            PVR_TXRFMT_ARGB1555;
    if (tx.converted) fmt = PVR_TXRFMT_ARGB1555;
    if (tx.region) fmt = tx.region->group == SHARED_GROUPS ? PVR_TXRFMT_ARGB1555 :
                        PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(bank);
    unsigned image = pass + (tx.page && tx.page->raw && (m->opcode & 2) ? 1 : 0);
    if (textured && tx.converted) {
        transient_header(tx.converted, pass, tx.converted->dimension,
                         tx.converted->dimension, header);
    } else if (textured && tx.region &&
        tx.region->header_format == fmt) {
        *header = tx.region->header;
    } else if (textured && tx.page &&
               tx.page->header_valid[image] && tx.page->header_format[image] == fmt) {
        *header = tx.page->header[image];
    } else {
        pvr_poly_cxt_t c;
        context(&c, !textured ? NULL :
                    tx.converted ? tx.converted->image[pass] :
                    tx.region ? tx.region->image : tx.page->image[image],
                fmt, tx.converted ? tx.converted->dimension :
                     tx.region ? tx.region->dimension : 256,
                tx.converted ? tx.converted->dimension :
                     tx.region ? tx.region->dimension : 256);
        pvr_poly_compile(header, &c);
        if (textured && !tx.converted && tx.region) {
            tx.region->header = *header;
            tx.region->header_format = fmt;
        } else if (textured && !tx.converted) {
            tx.page->header[image] = *header;
            tx.page->header_format[image] = fmt;
            tx.page->header_valid[image] = 1;
        }
    }
    if (!textured)
        return ((m->opcode & 2) ? 128u : 255u) << 24 | (m->rgb & 255) << 16 |
               (m->rgb & 0xff00) | (m->rgb >> 16 & 255);
    if (format < 2 ||
        (code == 0x64 && !(m->opcode & 2) && (m->rgb & 255) != 128 &&
         (m->rgb >> 8 & 255) != 128 && (m->rgb >> 16 & 255) != 128))
        return tint(m->rgb, pass ? 128 : 255);
    return pass ? 0x80ffffff : 0xffffffff;
}

static int samples_picture(const gex_gpu_material *m, unsigned plane,
                           float u0, float u1, float v0, float v1) {
    target *src = pictures[plane];
    if (!src || src->atlas != m->atlas) return 0;
    int x = (int)(m->page & 15) * 64, y = m->page & 16 ? 256 : 0;
    unsigned format = m->page >> 7 & 3;
    int divisor = format == 0 ? 4 : format == 1 ? 2 : 1;
    if (x >= 320 && x + 256 / divisor <= 1024) return 0;
    int py = 8 + (int)plane * 240;
    for (int uy = (int)floorf(v0 / 256); uy <= (int)floorf(v1 / 256); ++uy)
        for (int ux = (int)floorf(u0 / 256); ux <= (int)floorf(u1 / 256); ++ux) {
            float min_u = u0 - ux * 256, max_u = u1 - ux * 256 + 0.0001f;
            float min_v = v0 - uy * 256, max_v = v1 - uy * 256 + 0.0001f;
            if (min_u < 0) min_u = 0;
            if (max_u > 256) max_u = 256;
            if (min_v < 0) min_v = 0;
            if (max_v > 256) max_v = 256;
            for (unsigned carry = 0; carry < 2; ++carry)
                if (x + min_u / divisor < 320 + carry * 1024 &&
                    x + max_u / divisor > carry * 1024 &&
                    y + carry + min_v < py + 224 && y + carry + max_v > py)
                    return 1;
        }
    return 0;
}

static int texel_floor(float value) {
    int truncated = (int)value;
    return truncated - (value < truncated);
}

static float pixel_ceil(float value) {
    /* Larger finite floats already have no fractional bits. Keep infinities
     * and NaNs unchanged without an out-of-range integer conversion. */
    if (!(value > -8388608.0f && value < 8388608.0f)) return value;
    int truncated = (int)value;
    return (float)(truncated + (value > truncated));
}

/* Keep general clipping's arrays and register spills out of rectangular draws. */
static int __attribute__((noinline)) polygon_pieces(const gex_gpu_vertex q[4], int left, int top, int right, int bottom,
                          const gex_gpu_material *m, draw_texture tx, int bank,
                          const float bounds[8], char *error, size_t size) {
    float x0 = bounds[0], x1 = bounds[1], y0 = bounds[2], y1 = bounds[3];
    float u0 = bounds[4], u1 = bounds[5], v0 = bounds[6], v1 = bounds[7];
    unsigned code = m->opcode & ~2u, format = m->page >> 7 & 3;
    int textured = code == 0x64 || code == 0x2c;
    int min_u = (int)floorf(u0), max_u = (int)ceilf(u1);
    int min_v = (int)floorf(v0), max_v = (int)ceilf(v1);
    if (min_u < 0 || max_u > 255) { min_u = 0; max_u = 255; }
    if (min_v < 0 || max_v > 255) { min_v = 0; max_v = 255; }
    unsigned passes = textured && (format == 2 || tx.converted) &&
                      (m->opcode & 2) ? 2 : 1;
    int ux0 = textured ? (int)floorf(u0 / 256) : 0;
    int ux1 = textured ? (int)floorf(u1 / 256) : 0;
    int vy0 = textured ? (int)floorf(v0 / 256) : 0;
    int vy1 = textured ? (int)floorf(v1 / 256) : 0;
    if (ux1 - ux0 > 4 || vy1 - vy0 > 4)
        return fail(error, size, "PVR polygon wrap range too large");
    for (int uy = vy0; uy <= vy1; ++uy)
        for (int ux = ux0; ux <= ux1; ++ux) {
            float u_edges[5] = {0};
            unsigned nu = 1;
            u_edges[nu++] = 256;
            for (unsigned ui = 0; ui + 1 < nu; ++ui) {
                float v_edges[6] = {0};
                unsigned nv = 1;
                v_edges[nv++] = 256;
                for (unsigned vi = 0; vi + 1 < nv; ++vi) {
                    gex_gpu_vertex a[24], b[24];
                    static const unsigned order[] = {0, 1, 3, 2};
                    float piece_u0 = ux * 256.0f + u_edges[ui];
                    float piece_u1 = ux * 256.0f + u_edges[ui + 1];
                    float piece_v0 = uy * 256.0f + v_edges[vi];
                    float piece_v1 = uy * 256.0f + v_edges[vi + 1];
                    if (x1 <= left || x0 >= right || y1 <= top || y0 >= bottom ||
                        (textured && (u1 < piece_u0 || u0 > piece_u1 ||
                                      v1 < piece_v0 || v0 > piece_v1)))
                        continue;
                    int inside = x0 >= left && x1 <= right && y0 >= top && y1 <= bottom &&
                        (!textured || (u0 >= piece_u0 && u1 <= piece_u1 &&
                                       v0 >= piece_v0 && v1 <= piece_v1));
                    for (unsigned i = 0; i < 4; ++i)
                        a[i] = q[order[i]];
                    unsigned count = 4;
                    const float edges[] = {(float)left, (float)right, (float)top, (float)bottom,
                                           piece_u0, piece_u1, piece_v0, piece_v1};
                    const unsigned axes[] = {0, 0, 1, 1, 2, 2, 3, 3};
                    for (unsigned edge = 0; !inside && edge < (textured ? 8u : 4u) && count; ++edge) {
                        if (edge & 1 ? bounds[edge] <= edges[edge] : bounds[edge] >= edges[edge])
                            continue;
                        count = clip_polygon(a, count, b, axes[edge], edges[edge], !(edge & 1));
                        memcpy(a, b, count * sizeof *a);
                    }
                    if (count < 3)
                        continue;
                    float min_x = inside ? x0 : a[0].x, max_x = inside ? x1 : min_x;
                    float min_y = inside ? y0 : a[0].y, max_y = inside ? y1 : min_y;
                    for (unsigned i = 1; !inside && i < count; ++i) {
                        if (a[i].x < min_x) min_x = a[i].x;
                        if (a[i].x > max_x) max_x = a[i].x;
                        if (a[i].y < min_y) min_y = a[i].y;
                        if (a[i].y > max_y) max_y = a[i].y;
                    }
                    if (pixel_ceil(min_x - 0.5f) >= pixel_ceil(max_x - 0.5f) ||
                        pixel_ceil(min_y - 0.5f) >= pixel_ceil(max_y - 0.5f))
                        continue;
                    for (unsigned pass = 0; pass < passes; ++pass) {
                        pvr_poly_hdr_t header;
                        uint32_t color = polygon_header(m, tx, bank, pass, &header);
                        for (unsigned i = 1; i + 1 < count; ++i) {
                            gex_gpu_vertex triangle[] = {a[0], a[i], a[i + 1], a[i + 1]};
                            if (count == 4) {
                                triangle[1] = a[1]; triangle[2] = a[3]; triangle[3] = a[2];
                            }
                            pvr_vertex_t v[4];
                            for (unsigned j = 0; j < 4; ++j)
                                v[j] = (pvr_vertex_t){
                                    .flags = j == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                                    .x = triangle[j].x, .y = triangle[j].y, .z = 1,
                                    .u = (triangle[j].u - ux * 256) / 256,
                                    .v = (triangle[j].v - uy * 256) / 256,
                                    .argb = color, .oargb = 0};
                            for (unsigned j = 0; j < 4; ++j) {
                                v[j].u = texture_coordinate(tx, v[j].u, 0);
                                v[j].v = texture_coordinate(tx, v[j].v, 1);
                            }
                            if (append_texture(&header, v, tx.page, tx.region,
                                               min_u, max_u + 1, min_v, max_v + 1,
                                               error, size)) return -1;
                            if (count == 4) break;
                        }
                    }
                }
            }
        }
    return 0;
}

static int finite_float(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}

static float integral_sample_bias(float position, float extent, float origin, float step) {
    if (position > -8388608.0f && position < 8388608.0f &&
        extent > 0 && extent < 8388608.0f &&
        origin > -1024 && origin < 1024 && step > -1024 && step < 1024 &&
        position == (float)(int)position && extent == (float)(int)extent &&
        origin == (float)(int)origin && step == (float)(int)step)
        return 0.5f;
    return 1.0f / 1024;
}

static int fit_rectangle_axis(float position, float end, float origin, float far,
                              int clip_min, int clip_max, float *start_uv, float *end_uv) {
    float extent = fabsf(end - position);
    if (!(position > -8388608.0f && position < 8388608.0f &&
          end > -8388608.0f && end < 8388608.0f &&
          origin >= -1024 && origin <= 1024 && far >= -1024 && far <= 1024 &&
          extent >= 1 && extent < 8388608.0f) ||
        position != (float)(int)position || end != (float)(int)end ||
        origin != (float)(int)origin || far != (float)(int)far)
        return 0;
    int width = (int)extent, span = (int)far - (int)origin;
    if (span % width == 0) return 0;
    int flip = position > end;
    int low = (int)(flip ? end : position), high = (int)(flip ? position : end);
    if (low < clip_min) low = clip_min;
    if (high > clip_max) high = clip_max;
    if (low >= high) return 0;
    int first = flip ? (int)position - high : low - (int)position;
    int count = high - low;
    int64_t fixed_step = (int64_t)span * 65536 / width;
    int mismatch = 0;
    for (int i = first; i < first + count; ++i) {
        int expected = (int)origin + flip + gex_pvr_fixed_floor((int64_t)i * fixed_step);
        float current = origin + flip + i * ((float)span / width) + 1.0f / 1024;
        if (texel_floor(current) != expected) {
            mismatch = 1;
            break;
        }
    }
    if (!mismatch) return 0;
    gex_pvr_axis_fit fit;
    if (!gex_pvr_fit_axis(width, span, first, count, &fit)) return 0;
    float a = origin + flip + fit.first - fit.step * 0.5f;
    float b = a + fit.step * count;
    if (!(a >= -1024 && a <= 1024 && b >= -1024 && b <= 1024)) return 0;
    float actual_step = (b - a) / count;
    for (int i = 0; i < count; ++i) {
        int expected = (int)origin + flip + gex_pvr_fixed_floor((int64_t)(first + i) * fixed_step);
        float sample = a + (i + 0.5f) * actual_step;
        if (!(sample > expected && sample < expected + 1)) return 0;
    }
    *start_uv = a;
    *end_uv = b;
    return 1;
}

static int native_polygon_prepare_impl(void *data, const gex_gpu_vertex input[4], int left, int top, int right,
                                  int bottom, unsigned sample_scale, const gex_gpu_material *m, background_polygon *prepared,
                                  char *error, size_t size) {
    WORK(polygons, 1);
    if (prepared) prepared->prepared_valid = 0;
    if (left >= right || top >= bottom)
        return 0;
    if (select_target(data, error, size))
        return -1;
    unsigned code = m->opcode & ~2u, format = m->page >> 7 & 3;
    int textured = code == 0x64 || code == 0x2c;
    if (textured && format == 3)
        return fail(error, size, "invalid PVR polygon texture format");
    gex_gpu_vertex q[4];
    float u0, u1, v0, v1, x0, x1, y0, y1;
    memcpy(q, input, sizeof q);
    float du = 0, dv = 0;
    if (q[0].x == q[2].x && q[1].x == q[3].x && q[0].y == q[1].y && q[2].y == q[3].y) {
        float w = fabsf(q[3].x - q[0].x), h = fabsf(q[3].y - q[0].y);
        if (!w || !h)
            return 0;
        float step_u = (q[3].u - q[0].u) / w, step_v = (q[3].v - q[0].v) / h;
        float bias_u = textured && q[0].u == q[2].u && q[1].u == q[3].u ?
            integral_sample_bias(q[0].x, w, q[0].u, step_u) : 1.0f / 1024;
        float bias_v = textured && q[0].v == q[1].v && q[2].v == q[3].v ?
            integral_sample_bias(q[0].y, h, q[0].v, step_v) : 1.0f / 1024;
        float ou = (q[0].x > q[3].x ? 1 : 0) - step_u * 0.5f + bias_u;
        float ov = (q[0].y > q[3].y ? 1 : 0) - step_v * 0.5f + bias_v;
        for (unsigned i = 0; i < 4; ++i) {
            q[i].u += ou;
            q[i].v += ov;
        }
        if (code == 0x2c && sample_scale == 1 &&
            input[0].u == input[2].u && input[1].u == input[3].u &&
            input[0].v == input[1].v && input[2].v == input[3].v) {
            float start, end;
            if (fit_rectangle_axis(input[0].x, input[3].x, input[0].u, input[3].u,
                                   left, right, &start, &end)) {
                float low = fmaxf(fminf(input[0].x, input[3].x), left);
                float high = fminf(fmaxf(input[0].x, input[3].x), right);
                q[0].x = q[2].x = input[0].x > input[3].x ? high : low;
                q[1].x = q[3].x = input[0].x > input[3].x ? low : high;
                q[0].u = q[2].u = start;
                q[1].u = q[3].u = end;
            }
            if (fit_rectangle_axis(input[0].y, input[3].y, input[0].v, input[3].v,
                                   top, bottom, &start, &end)) {
                float low = fmaxf(fminf(input[0].y, input[3].y), top);
                float high = fminf(fmaxf(input[0].y, input[3].y), bottom);
                q[0].y = q[1].y = input[0].y > input[3].y ? high : low;
                q[2].y = q[3].y = input[0].y > input[3].y ? low : high;
                q[0].v = q[1].v = start;
                q[2].v = q[3].v = end;
            }
        }
    }
    int rectangle = q[0].x == q[2].x && q[1].x == q[3].x &&
                    q[0].y == q[1].y && q[2].y == q[3].y &&
                    q[0].u == q[2].u && q[1].u == q[3].u &&
                    q[0].v == q[1].v && q[2].v == q[3].v;
    u0 = q[0].u; u1 = u0; v0 = q[0].v; v1 = v0;
    x0 = q[0].x; x1 = x0; y0 = q[0].y; y1 = y0;
    /* A rectangle's other vertices repeat the two diagonal corners. */
    for (unsigned i = 0; i < 4; i += rectangle ? 3 : 1) {
        if (!finite_float(q[i].x) || !finite_float(q[i].y) ||
            !finite_float(q[i].u) || !finite_float(q[i].v))
            return fail(error, size, "invalid PVR polygon vertex");
        if (q[i].u < u0)
            u0 = q[i].u;
        if (q[i].u > u1)
            u1 = q[i].u;
        if (q[i].v < v0)
            v0 = q[i].v;
        if (q[i].v > v1)
            v1 = q[i].v;
        if (q[i].x < x0) x0 = q[i].x;
        if (q[i].x > x1) x1 = q[i].x;
        if (q[i].y < y0) y0 = q[i].y;
        if (q[i].y > y1) y1 = q[i].y;
    }
    if (u0 < -1024 || u1 > 1024 || v0 < -1024 || v1 > 1024)
        return fail(error, size, "PVR polygon texture coordinates out of range");
    if (x1 <= left || x0 >= right || y1 <= top || y0 >= bottom)
        return 0;
    float sample_u0 = u0, sample_u1 = u1, sample_v0 = v0, sample_v1 = v1;
    float cx0 = x0 > left ? x0 : left, cx1 = x1 < right ? x1 : right;
    float cy0 = y0 > top ? y0 : top, cy1 = y1 < bottom ? y1 : bottom;
    int a = 0, b = 0, c = 0, d = 0;
    if (rectangle) {
        float first_x = pixel_ceil(cx0 - 0.5f) + 0.5f;
        float last_x = pixel_ceil(cx1 - 0.5f) - 0.5f;
        float first_y = pixel_ceil(cy0 - 0.5f) + 0.5f;
        float last_y = pixel_ceil(cy1 - 0.5f) - 0.5f;
        if (first_x > last_x || first_y > last_y) return 0;
        du = (q[1].u - q[0].u) / (q[1].x - q[0].x);
        dv = (q[2].v - q[0].v) / (q[2].y - q[0].y);
        /* Finite UVs are already bounded to +/-1024, so integer conversion
         * implements floor without libm's general-purpose classification. */
        a = texel_floor(q[0].u + (first_x - q[0].x) * du);
        b = texel_floor(q[0].u + (last_x - q[0].x) * du);
        c = texel_floor(q[0].v + (first_y - q[0].y) * dv);
        d = texel_floor(q[0].v + (last_y - q[0].y) * dv);
        /* Ownership checks concern sampled texels, not vertices outside
         * the viewport or the exclusive far edge of a texture rectangle. */
        sample_u0 = a < b ? a : b; sample_u1 = a > b ? a : b;
        sample_v0 = c < d ? c : d; sample_v1 = c > d ? c : d;
    }
    if (textured)
        for (unsigned plane = 0; plane < 2; ++plane)
            if (samples_picture(m, plane, sample_u0, sample_u1, sample_v0, sample_v1)) {
                snprintf(error, size,
                         "PVR framebuffer sampling is unsupported: page=%x clut=%x plane=%u UV=%.2f,%.2f..%.2f,%.2f XY=%.1f,%.1f..%.1f,%.1f",
                         m->page, m->clut, plane, u0, v0, u1, v1, x0, y0, x1, y1);
                return -1;
            }
    int min_u = rectangle ? (int)sample_u0 : texel_floor(sample_u0);
    int max_u = rectangle ? (int)sample_u1 : (int)pixel_ceil(sample_u1);
    int min_v = rectangle ? (int)sample_v0 : texel_floor(sample_v0);
    int max_v = rectangle ? (int)sample_v1 : (int)pixel_ceil(sample_v1);
    if (min_u < 0 || max_u > 255) { min_u = 0; max_u = 255; }
    if (min_v < 0 || max_v > 255) { min_v = 0; max_v = 255; }
    if (textured && format < 2) {
        unsigned clut = m->clut * 32 & 0xfffff;
        for (unsigned plane = 0; plane < 2; ++plane)
            if (pictures[plane] && pictures[plane]->atlas == m->atlas &&
                write_overlap(clut, format == 0 ? 32 : 512,
                              (8 + plane * 240) * 2048, 2048, 640, 224))
                return fail(error, size, "PVR framebuffer-as-CLUT is unsupported without readback");
    }
    draw_texture tx = {0};
    int bank = 0;
    if (textured) {
        if (native_texture_region(m, min_u, max_u + 1, min_v, max_v + 1,
                                  &tx, &bank, error, size)) return -1;
    }
    if (bank < 0)
        return -1;
    unsigned passes = textured && (format == 2 || tx.converted) &&
                      (m->opcode & 2) ? 2 : 1;
    if (rectangle) {
        int first_u = texel_floor(a / 256.0f), last_u = texel_floor(b / 256.0f);
        int first_v = texel_floor(c / 256.0f), last_v = texel_floor(d / 256.0f);
        if (!textured || (first_u == last_u && first_v == last_v)) {
            float rectangle[8];
            for (unsigned i = 0; i < 2; ++i) {
                const gex_gpu_vertex *corner = &q[i * 3];
                float x = corner->x < cx0 ? cx0 : corner->x > cx1 ? cx1 : corner->x;
                float y = corner->y < cy0 ? cy0 : corner->y > cy1 ? cy1 : corner->y;
                rectangle[i] = x;
                rectangle[2 + i] = y;
                rectangle[4 + i] = (q[0].u + (x - q[0].x) * du - first_u * 256) / 256;
                rectangle[6 + i] = (q[0].v + (y - q[0].y) * dv - first_v * 256) / 256;
            }
            if (prepared && passes == 1) {
                memcpy(prepared->prepared, rectangle, sizeof rectangle);
                prepared->region[0] = min_u; prepared->region[1] = max_u + 1;
                prepared->region[2] = min_v; prepared->region[3] = max_v + 1;
                prepared->prepared_valid = 1;
            }
            for (unsigned i = 0; i < 2; ++i) {
                rectangle[4 + i] = texture_coordinate(tx, rectangle[4 + i], 0);
                rectangle[6 + i] = texture_coordinate(tx, rectangle[6 + i], 1);
            }
            for (unsigned pass = 0; pass < passes; ++pass) {
                pvr_poly_hdr_t header;
                uint32_t color = polygon_header(m, tx, bank, pass, &header);
                pvr_vertex_t vertices[4];
                for (unsigned i = 0; i < 4; ++i)
                    vertices[i] = (pvr_vertex_t){
                        .flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                        .x = rectangle[i & 1], .y = rectangle[2 + (i >> 1)], .z = 1,
                        .u = rectangle[4 + (i & 1)], .v = rectangle[6 + (i >> 1)],
                        .argb = color, .oargb = 0};
                if (append_texture(&header, vertices, tx.page, tx.region,
                                   min_u, max_u + 1, min_v, max_v + 1, error, size)) return -1;
            }
            return 0;
        }
    }
    const float bounds[] = {x0, x1, y0, y1, u0, u1, v0, v1};
    return polygon_pieces(q, left, top, right, bottom, m, tx, bank, bounds, error, size);
}

static int native_polygon_prepare(void *data, const gex_gpu_vertex input[4], int left, int top, int right,
                                  int bottom, unsigned sample_scale, const gex_gpu_material *m, background_polygon *prepared,
                                  char *error, size_t size) {
    PROFILE_BEGIN();
    transient_pin pin = {NULL, active_transient_pin};
    active_transient_pin = &pin;
    int result = native_polygon_prepare_impl(data, input, left, top, right, bottom, sample_scale,
                                            m, prepared, error, size);
    active_transient_pin = pin.previous;
    PROFILE_END(prepare);
    return result;
}

static int native_polygon_draw(void *data, const gex_gpu_vertex input[4], int left, int top, int right,
                               int bottom, unsigned sample_scale, const gex_gpu_material *m, char *error, size_t size) {
    return native_polygon_prepare(data, input, left, top, right, bottom, sample_scale, m, NULL, error, size);
}

static int background_draw_impl(target *t, const background_polygon *p, unsigned rgb,
                                char *error, size_t size) {
    gex_gpu_material material = p->material;
    material.rgb = rgb;
    if (!p->prepared_valid)
        return native_polygon_draw(t, p->vertices, p->clip[0], p->clip[1],
                                   p->clip[2], p->clip[3], p->sample_scale, &material, error, size);
    WORK(prepared, 1);
    if (select_target(t, error, size)) return -1;
    unsigned code = material.opcode & ~2u;
    int textured = code == 0x64 || code == 0x2c;
    draw_texture tx = {0};
    int bank = 0;
    if (textured) {
        if (native_texture_region(&material, p->region[0], p->region[1],
                                  p->region[2], p->region[3], &tx, &bank, error, size)) return -1;
    }
    if (bank < 0) return -1;
    float u[2], v[2];
    for (unsigned i = 0; i < 2; ++i) {
        u[i] = texture_coordinate(tx, p->prepared[4 + i], 0);
        v[i] = texture_coordinate(tx, p->prepared[6 + i], 1);
    }
    unsigned passes = tx.converted && (material.opcode & 2) ? 2 : 1;
    for (unsigned pass = 0; pass < passes; ++pass) {
        pvr_poly_hdr_t header;
        uint32_t color = polygon_header(&material, tx, bank, pass, &header);
        pvr_vertex_t vertices[4];
        for (unsigned i = 0; i < 4; ++i)
            vertices[i] = (pvr_vertex_t){
                .flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                .x = p->prepared[i & 1], .y = p->prepared[2 + (i >> 1)],
                .z = 1, .u = u[i & 1], .v = v[i >> 1],
                .argb = color, .oargb = 0};
        if (append_texture(&header, vertices, tx.page, tx.region, p->region[0], p->region[1],
                           p->region[2], p->region[3], error, size)) return -1;
    }
    return 0;
}

static int background_draw(target *t, const background_polygon *p, unsigned rgb,
                           char *error, size_t size) {
    transient_pin pin = {NULL, active_transient_pin};
    active_transient_pin = &pin;
    int result = background_draw_impl(t, p, rgb, error, size);
    active_transient_pin = pin.previous;
    return result;
}

/* Only an exact prefix after a black clear is reusable. This preserves packet
 * order without assuming that all tile packets precede objects or HUD draws. */
static void background_accept(target *t, unsigned count) {
    t->background_mode = BACKGROUND_IDLE;
    t->background_at = 0;
    t->background_valid = count >= BACKGROUND_MINIMUM;
    t->background_count = t->background_valid ? count : 0;
}

static int background_replay(target *t, char *error, size_t size) {
    if (t->background_mode != BACKGROUND_REUSE) return 0;
    unsigned count = t->background_at;
    t->background_mode = BACKGROUND_IDLE;
    t->background_at = 0;
    for (unsigned i = 0; i < count; ++i) {
        const background_polygon *p = &t->background[i];
        if (background_draw(t, p, p->material.rgb, error, size))
            return -1;
    }
    return 0;
}

static int background_stop(target *t, char *error, size_t size) {
    if (!t || !t->background_mode) return 0;
    unsigned count = t->background_at;
    if (!count) {
        t->background_mode = BACKGROUND_IDLE;
        return 0;
    }
    t->background_misses = 0;
    if (t->background_mode == BACKGROUND_RECORD) {
        t->background_count = count;
        t->background_valid = 0;
        t->background_mode = BACKGROUND_IDLE;
        t->background_at = 0;
        return 0;
    }
    if (t->background_mode == BACKGROUND_REDRAW) {
        t->background_mode = BACKGROUND_IDLE;
        t->background_at = 0;
        return 0;
    }
    if (t->background_mode == BACKGROUND_VERIFY) {
        background_accept(t, count);
        return 0;
    }
    if (background_replay(t, error, size)) return -1;
    /* Verify a shortened prefix next frame before reusing its records. */
    t->background_mode = BACKGROUND_IDLE;
    t->background_at = 0;
    t->background_count = count;
    t->background_valid = 0;
    return 0;
}

static int background_advance(target *t, char *error, size_t size) {
    if (++t->background_at != t->background_count) return 0;
    t->background_misses = 0;
    if (t->background_mode == BACKGROUND_REDRAW) {
        t->background_mode = BACKGROUND_IDLE;
        t->background_at = 0;
        return 0;
    }
    if (t->background_mode == BACKGROUND_VERIFY) {
        background_accept(t, t->background_at);
        return 0;
    }
    return background_replay(t, error, size);
}

static void background_dependencies(target *t, const gex_gpu_vertex input[4],
                                    const gex_gpu_material *m, const background_polygon *p) {
    unsigned code = m->opcode & ~2u;
    if (code != 0x64 && code != 0x2c) return;
    unsigned format = m->page >> 7 & 3;
    /* Include nearest-sample adjustment and mirrored/wrapped page edges.
     * Coarse 64-byte bits deliberately overestimate, never omit, dependencies. */
    int left, right, top, bottom;
    if (p->prepared_valid) {
        left = p->region[0] - 2; right = p->region[1] + 2;
        top = p->region[2] - 2; bottom = p->region[3] + 2;
    } else {
        float u0 = input[0].u, u1 = u0, v0 = input[0].v, v1 = v0;
        for (unsigned i = 1; i < 4; ++i) {
            u0 = fminf(u0, input[i].u); u1 = fmaxf(u1, input[i].u);
            v0 = fminf(v0, input[i].v); v1 = fmaxf(v1, input[i].v);
        }
        left = (int)floorf(u0) - 2; right = (int)ceilf(u1) + 2;
        top = (int)floorf(v0) - 2; bottom = (int)ceilf(v1) + 2;
    }
    if (left < 0 || right > 256) { left = 0; right = 256; }
    if (bottom - top >= 256) { top = 0; bottom = 256; }
    unsigned begin = format == 0 ? (unsigned)left / 2 :
                     format == 1 ? (unsigned)left : (unsigned)left * 2;
    unsigned end = format == 0 ? ((unsigned)right + 1) / 2 :
                   format == 1 ? (unsigned)right : (unsigned)right * 2;
    unsigned base = (m->page & 15) * 128 + (m->page & 16 ? 0x80000 : 0);
    for (int y = top; y < bottom; ++y)
        dependency_range(t, base + ((unsigned)y & 255) * 2048 + begin, end - begin);
    if (format < 2)
        dependency_range(t, m->clut * 32 & 0xfffff, format == 0 ? 32 : 512);
}

static int native_packet_impl(void *data, const uint8_t *packet, const gex_raster_state *s,
                         int offset_x, int offset_y, char *error, size_t size) {
    target *t = data;
    t->pending_key.valid = 0;
    if (!packet || !t->background_mode) return 0;
    unsigned code = packet[7] & ~2u;
    unsigned bytes = code == 0x2c ? 36 : code == 0x64 ? 16 : code == 0x60 ? 12 :
                     code == 0x68 || code == 0x70 || code == 0x78 ? 8 : 0;
    if (!bytes) return 0;
    packet_key key = {
        .atlas = s->atlas, .modulation = s->modulation,
        .state = {(int)(code == 0x2c ? word(packet + 22) : s->page),
                  s->left, s->top, s->right, s->bottom, s->origin_y,
                  s->layout, s->pixel_scale, s->offset_x, s->world_clip, s->world_left,
                  s->world_top, s->world_right, s->world_bottom, offset_x, offset_y},
        .valid = 1
    };
    /* The linked-list successor is not part of a draw's identity. */
    memcpy(key.packet, packet + 4, bytes);
    if ((t->background_mode == BACKGROUND_REUSE ||
         t->background_mode == BACKGROUND_REDRAW) &&
        t->background_revision == atlas_revision) {
        const background_polygon *p = &t->background[t->background_at];
        const packet_key *previous = &p->key;
        if (previous->valid && !memcmp(key.packet + 3, previous->packet + 3, sizeof key.packet - 3) &&
            !memcmp(key.state, previous->state, sizeof key.state) &&
            key.atlas == previous->atlas && key.modulation == previous->modulation) {
            unsigned rgb = key.packet[0] | (unsigned)key.packet[1] << 8 |
                           (unsigned)key.packet[2] << 16;
            if (t->background_mode == BACKGROUND_REDRAW || rgb != p->material.rgb) {
                unsigned at = t->background_at;
                if (background_replay(t, error, size)) return -1;
                t->background_mode = BACKGROUND_REDRAW;
                t->background_at = at;
                if (background_draw(t, p, rgb, error, size))
                    return -1;
            }
            t->pending_key.valid = 0;
            if (select_target(t, error, size) || background_advance(t, error, size)) return -1;
            WORK(reused, 1);
            return 1;
        }
    }
    t->pending_key = key;
    return 0;
}

static int native_packet(void *data, const uint8_t *packet, const gex_raster_state *s,
                         int offset_x, int offset_y, char *error, size_t size) {
    NATIVE_DRAW_BEGIN();
    PROFILE_BEGIN();
    int result = native_packet_impl(data, packet, s, offset_x, offset_y, error, size);
    PROFILE_END(packet);
    NATIVE_DRAW_END(packet);
    return result;
}

static int native_polygon_impl(void *data, const gex_gpu_vertex input[4], int left, int top,
                          int right, int bottom, unsigned sample_scale, const gex_gpu_material *m,
                          char *error, size_t size) {
    target *t = data;
    if (select_target(t, error, size)) {
        t->pending_key.valid = 0;
        return -1;
    }
    if (!t->background_mode) {
        t->pending_key.valid = 0;
        return native_polygon_draw(t, input, left, top, right, bottom, sample_scale, m, error, size);
    }
    packet_key key = t->pending_key;
    t->pending_key.valid = 0;
    unsigned code = m->opcode & ~2u;
    int textured = code == 0x64 || code == 0x2c;
    unsigned format = m->page >> 7 & 3;
    int eligible = !textured || (format < 3 && m->atlas == tracked_atlas);
    if (textured && format == 2 && eligible) {
        float u0 = input[0].u, u1 = u0, v0 = input[0].v, v1 = v0;
        for (unsigned i = 0; i < 4; ++i) {
            if (!finite_float(input[i].u) || !finite_float(input[i].v) ||
                input[i].u < -1024 || input[i].u > 1024 ||
                input[i].v < -1024 || input[i].v > 1024) {
                eligible = 0;
                break;
            }
            if (input[i].u < u0) u0 = input[i].u;
            if (input[i].u > u1) u1 = input[i].u;
            if (input[i].v < v0) v0 = input[i].v;
            if (input[i].v > v1) v1 = input[i].v;
        }
        for (unsigned plane = 0; eligible && plane < 2; ++plane)
            if (samples_picture(m, plane, u0 - 1, u1 + 1, v0 - 1, v1 + 1))
                eligible = 0;
    }
    int clip[] = {left, top, right, bottom};
    if ((t->background_mode == BACKGROUND_REUSE || t->background_mode == BACKGROUND_VERIFY) && eligible &&
        t->background_revision == atlas_revision) {
        background_polygon *p = &t->background[t->background_at];
        const gex_gpu_material *previous = &p->material;
        if (p->sample_scale == sample_scale &&
            !memcmp(input, p->vertices, sizeof p->vertices) &&
            !memcmp(clip, p->clip, sizeof clip) &&
            m->page == previous->page && m->clut == previous->clut &&
            m->opcode == previous->opcode && m->rgb == previous->rgb &&
            m->atlas == previous->atlas && m->modulation == previous->modulation) {
            if (t->background_mode == BACKGROUND_VERIFY &&
                native_polygon_prepare(t, input, left, top, right, bottom, sample_scale, m, p, error, size))
                return -1;
            p->key = key;
            return background_advance(t, error, size);
        }
    }
    if (t->background_mode >= BACKGROUND_REUSE || !eligible) {
        if ((t->background_mode == BACKGROUND_REUSE ||
             t->background_mode == BACKGROUND_VERIFY) &&
            !t->background_at && ++t->background_misses >= 3) {
            t->background_count = 0;
            t->background_valid = 0;
        }
        if (background_stop(t, error, size)) return -1;
    }
    background_polygon *record = t->background_mode == BACKGROUND_RECORD ?
                                 &t->background[t->background_at] : NULL;
    if (native_polygon_prepare(t, input, left, top, right, bottom, sample_scale, m, record, error, size)) return -1;
    if (t->background_mode == BACKGROUND_RECORD) {
        background_polygon *p = &t->background[t->background_at++];
        memcpy(p->vertices, input, sizeof p->vertices);
        memcpy(p->clip, clip, sizeof clip);
        p->material = *m;
        p->key = key;
        p->sample_scale = sample_scale;
        background_dependencies(t, input, m, p);
        if (t->background_at == BACKGROUND_POLYGONS)
            return background_stop(t, error, size);
    }
    return 0;
}

static int native_polygon(void *data, const gex_gpu_vertex input[4], int left, int top,
                          int right, int bottom, const gex_gpu_material *m,
                          char *error, size_t size) {
    NATIVE_DRAW_BEGIN();
    PROFILE_BEGIN();
    int result = native_polygon_impl(data, input, left, top, right, bottom, 0, m, error, size);
    PROFILE_END(polygon);
    NATIVE_DRAW_END(polygon);
    return result;
}

static int native_polygon_scaled(void *data, const gex_gpu_vertex input[4], int left, int top,
                                 int right, int bottom, int scale, const gex_gpu_material *m,
                                 char *error, size_t size) {
    NATIVE_DRAW_BEGIN();
    PROFILE_BEGIN();
    int result = scale > 0 ?
        native_polygon_impl(data, input, left, top, right, bottom, (unsigned)scale, m, error, size) :
        fail(error, size, "invalid PVR polygon scale");
    PROFILE_END(polygon);
    NATIVE_DRAW_END(polygon);
    return result;
}

static int draw(void *data, const gex_gpu_span *spans, size_t count, const gex_gpu_material *m,
                char *error, size_t size) {
    if (background_stop(data, error, size) || select_target(data, error, size))
        return -1;
    if (!count)
        return 0;
    for (size_t i = 0; i < count; ++i) {
        const gex_gpu_span *s = &spans[i];
        if (s->x < 0 || s->y < 0 || s->count > 320 - s->x || s->y >= 240 || s->count <= 0 ||
            s->scale < 1 || s->phase < 0 || (s->mode != 0 && s->mode != 1) || s->reserved[0] < 0 ||
            s->reserved[0] > 240 - s->y || (s->reserved[0] && s->dv != 0) ||
            (s->reserved[0] && s->reserved[1] != 0 &&
             s->reserved[1] != (s->mode == 1 ? 65536 : 1)) ||
            !isfinite(s->u) || !isfinite(s->v) || !isfinite(s->du) || !isfinite(s->dv))
            return fail(error, size, "invalid PVR pixel coverage");
    }
    unsigned code = m->opcode & ~2u, format = m->page >> 7 & 3;
    int textured = code == 0x64 || code == 0x2c;
    if (textured && format == 3)
        return fail(error, size, "unsupported PVR texture page format");
    texture *t = textured ? page_texture(m, spans, count, error, size) : NULL;
    if (textured && !t)
        return -1;
    int bank = textured && format < 2 ? palette(m, 0, 1, NULL, error, size) : 0;
    if (bank < 0)
        return -1;
    unsigned passes = textured && format == 2 && (m->opcode & 2) ? 2 : 1;
    pvr_poly_hdr_t header[2];
    uint32_t color[2] = {0xffffffff, 0x80ffffff};
    for (unsigned pass = 0; pass < passes; ++pass) {
        pvr_poly_cxt_t c;
        int fmt = format == 0   ? PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_4BPP_PAL(bank)
                  : format == 1 ? PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(bank)
                                : PVR_TXRFMT_ARGB1555;
        context(&c, textured ? t->image[pass] : NULL, fmt, 256, 256);
        if (!textured) {
            unsigned rgb = m->rgb;
            color[0] = ((m->opcode & 2) ? 128u : 255u) << 24 | (rgb & 255) << 16 | (rgb & 0xff00) |
                       (rgb >> 16 & 255);
        }
        pvr_poly_compile(&header[pass], &c);
    }
    for (size_t i = 0; i < count; ++i) {
        const gex_gpu_span *s = &spans[i];
        size_t rows = s->reserved[0] ? (unsigned)s->reserved[0] : 1;
        double row_step = s->reserved[0] ? s->reserved[1] : 0;
        if (!s->reserved[0] && i + 1 < count)
            row_step = spans[i + 1].v - s->v;
        if (!s->reserved[0] && (row_step == 0 || row_step == (s->mode == 1 ? 65536 : 1)))
            while (i + rows < count) {
                const gex_gpu_span *n = &spans[i + rows];
                if (n->reserved[0] || n->x != s->x || n->y != s->y + (int)rows || n->y >= 240 ||
                    n->count != s->count || n->mode != s->mode || n->scale != s->scale ||
                    n->phase != s->phase || n->u != s->u || n->du != s->du || n->dv != s->dv ||
                    n->v != s->v + rows * row_step)
                    break;
                double v0 = s->v / (s->mode == 1 ? 65536 : 1);
                if (floor(v0 / 256) != floor((v0 + rows * (row_step != 0)) / 256))
                    break;
                ++rows;
            }
        for (int x = 0; x < s->count;) {
            int k = (x + s->phase) / s->scale;
            double divisor = s->mode == 1 ? 65536 : 1;
            double u = (s->u + k * s->du) / divisor;
            double tv = (s->v + k * s->dv) / divisor;
            double du = s->du / divisor, dv = s->dv / divisor;
            int run = s->count - x;
            if (s->scale > 1)
                run = s->scale - (x + s->phase) % s->scale;
            if (dv != 0)
                run = 1;
            if (run > s->count - x)
                run = s->count - x;
            /* Texture wrap is explicit: PVR cannot interpolate across a
             * wrapped page edge. Constant texel runs stay single quads. */
            while (run > 1 && (floor(u / 256) != floor((u + (run - 1) * du) / 256) ||
                               floor(tv / 256) != floor((tv + (run - 1) * dv) / 256)))
                --run;
            float tu = (float)(u - floor(u / 256) * 256);
            float vt = (float)(tv - floor(tv / 256) * 256);
            float step = s->scale == 1 ? (float)du : 0;
            if (!step)
                tu = floorf(tu) + 0.5f;
            /* Fixed-point coverage samples at integer pixel positions. Move
             * PVR's half-pixel sample back, just inside the texel boundary. */
            else if (s->mode == 1)
                tu += 1.0f / 1024 - step * 0.5f;
            else if (step < 0)
                tu -= step;
            pvr_vertex_t v[4];
            for (size_t row = 0; row < rows;) {
                size_t block = rows - row;
                float sample_v = fmodf(vt + (row_step ? (float)row : 0), 256);
                if (row_step && block > (size_t)(256 - floorf(sample_v)))
                    block = (size_t)(256 - floorf(sample_v));
                int vertical = block > 1 && row_step != 0;
                sample_v = floorf(sample_v) + (vertical ? 0 : 0.5f);
                for (unsigned pass = 0; pass < passes; ++pass) {
                    vertices(v, (float)(s->x + x), (float)(s->y + row), (float)run, (float)block,
                             tu / 256, sample_v / 256, step * run / 256,
                             vertical ? (float)block / 256 : 0, color[pass]);
                    if (append_texture(&header[pass], v, t, NULL, 0, 256, 0, 256, error, size))
                        return -1;
                }
                row += block;
            }
            x += run;
        }
        if (!s->reserved[0])
            i += rows - 1;
    }
    return 0;
}

static uint16_t *history(void *data, size_t bytes, char *error, size_t size) {
    target *t = data;
    if (t->history && t->history_bytes != bytes) {
        fail(error, size, "PVR compatibility history size changed");
        return NULL;
    }
    if (!t->history) {
        t->history = pvr_mem_malloc(bytes);
        t->history_bytes = bytes;
    }
    if (!t->history)
        fail(error, size, "cannot allocate PVR compatibility history");
    return t->history;
}

static uint32_t direct_image_hash(const gex_gpu_image *image, unsigned passes) {
    uint32_t hash = 2166136261u;
    size_t count = (size_t)image->width * image->height;
    for (unsigned pass = 0; pass < passes; ++pass) {
        const uint16_t *pixels = pass ? image->half : image->opaque;
        for (size_t i = 0; i < count; ++i) hash = (hash ^ pixels[i]) * 16777619u;
    }
    return hash;
}

static int native_image_with_id(void *data, const gex_gpu_vertex q[4],
                                int left, int top, int right, int bottom,
                                const gex_gpu_image *image, uint64_t title_id,
                                unsigned rgb, int vertex_tint,
                                char *error, size_t size) {
    if (!data || !q || !image || !image->opaque || !image->width || !image->height ||
        image->width > 256 || image->height > 256)
        return fail(error, size, "invalid PVR direct image");
    for (unsigned i = 0; i < 4; ++i)
        if (!finite_float(q[i].x) || !finite_float(q[i].y) ||
            !finite_float(q[i].u) || !finite_float(q[i].v))
            return fail(error, size, "invalid PVR direct image vertex");
    if (q[0].x != q[2].x || q[1].x != q[3].x ||
        q[0].y != q[1].y || q[2].y != q[3].y ||
        q[0].u != q[2].u || q[1].u != q[3].u ||
        q[0].v != q[1].v || q[2].v != q[3].v ||
        q[0].x >= q[3].x || q[0].y >= q[3].y ||
        q[0].u < 0 || q[0].v < 0 || q[3].u > image->width ||
        q[3].v > image->height || q[0].u >= q[3].u || q[0].v >= q[3].v)
        return fail(error, size, "unsupported PVR direct image geometry");
    float x0 = fmaxf(q[0].x, left), x1 = fminf(q[3].x, right);
    float y0 = fmaxf(q[0].y, top), y1 = fminf(q[3].y, bottom);
    if (x0 >= x1 || y0 >= y1) return 0;
    if (background_stop(data, error, size) || select_target(data, error, size)) return -1;
    unsigned width = 8, height = 8;
    while (width < image->width) width *= 2;
    while (height < image->height) height *= 2;
    size_t bytes = (size_t)width * height * sizeof(uint16_t);
    unsigned passes = image->half ? 2 : 1;
    size_t count = (size_t)image->width * image->height;
    uint32_t hash = title_id ? 0 : direct_image_hash(image, passes);
    transient_texture *texture = NULL;
    for (transient_texture *t = title_id ? title_textures[title_texture_bucket(title_id)] : transients;
         t; t = title_id ? t->title_next : t->next)
        if ((title_id ? t->title_id == title_id :
             t->direct_pixels && !t->title_id && t->direct_hash == hash &&
             t->direct_count == count &&
             !memcmp(t->direct_pixels, image->opaque, count * sizeof(uint16_t)) &&
             (passes == 1 || !memcmp(t->direct_pixels + count, image->half,
                                     count * sizeof(uint16_t)))) &&
            t->direct_width == image->width &&
            t->direct_height == image->height && t->direct_passes == passes &&
            t->direct_count == count) {
            texture = t;
            break;
        }
    int cached_title = texture && title_id;
    transient_pin pin = {texture, active_transient_pin};
    active_transient_pin = &pin;
    int result = -1;
    uint32_t *staging = NULL;
    if (!texture) {
        staging = malloc(bytes);
        texture = calloc(1, sizeof *texture);
        if (!staging || !texture) {
            free(texture);
            fail(error, size, "cannot allocate PVR direct image staging");
            goto done;
        }
        texture->next = transients;
        transients = texture;
        pin.texture = texture;
        gex_title_cel cel = {.width = image->width, .height = image->height};
        for (unsigned pass = 0; pass < passes; ++pass) {
            const uint16_t *pixels = pass ? image->half : image->opaque;
            if (gex_title_cel_twiddle(&cel, pixels, count, width, height,
                                      staging, bytes / sizeof *staging) != GEX_TITLE_CEL_OK) {
                fail(error, size, "invalid PVR direct image staging");
                goto done;
            }
            char allocation_error[256] = {0};
            texture->image[pass] = allocate_texture_without_drain(NULL, bytes,
                                                                   allocation_error, sizeof allocation_error);
            if (!texture->image[pass]) {
                fail(error, size, allocation_error[0] ? allocation_error :
                     "cannot allocate PVR direct image texture");
                goto done;
            }
            volatile uint32_t *destination = texture->image[pass];
            for (size_t i = 0; i < bytes / sizeof *staging; ++i) destination[i] = staging[i];
        }
        size_t storage = bytes * passes;
        size_t limit = title_id ? TITLE_SNAPSHOT_BYTES : SNAPSHOT_BYTES;
        size_t used = title_id ? retained_title_bytes : retained_snapshot_bytes - retained_title_bytes;
        if (storage <= limit) {
            int fenced = 0;
            while (storage > limit - used ||
                   storage > CACHE_BYTES - cache_bytes) {
                int kind = storage > limit - used ? !!title_id : -1;
                transient_texture **victim = oldest_idle_snapshot_kind(kind);
                if (!victim) break;
                if (!fenced && pvr_wait_render_done()) {
                    hardware_fail(error, size, "PVR direct image reclamation completion failed");
                    goto done;
                }
                fenced = 1;
                free_transient(victim);
                used = title_id ? retained_title_bytes : retained_snapshot_bytes - retained_title_bytes;
            }
            if (storage <= limit - used &&
                storage <= CACHE_BYTES - cache_bytes) {
                if (!title_id) {
                    texture->direct_pixels = malloc(count * passes * sizeof(uint16_t));
                    if (!texture->direct_pixels) {
                        fail(error, size, "cannot retain PVR direct image pixels");
                        goto done;
                    }
                    memcpy(texture->direct_pixels, image->opaque, count * sizeof(uint16_t));
                    if (passes == 2)
                        memcpy(texture->direct_pixels + count, image->half,
                               count * sizeof(uint16_t));
                }
                texture->title_id = title_id;
                texture->direct_width = image->width;
                texture->direct_height = image->height;
                texture->direct_passes = passes;
                texture->direct_count = count;
                texture->direct_hash = hash;
                texture->retained_bytes = storage;
                retained_snapshot_bytes += storage;
                if (title_id) retained_title_bytes += storage;
                cache_bytes += storage;
                if (title_id) {
                    unsigned bucket = title_texture_bucket(title_id);
                    texture->title_next = title_textures[bucket];
                    title_textures[bucket] = texture;
                }
            }
        }
    }
    texture->used = ++cache_clock;
    float du = (q[3].u - q[0].u) / (q[3].x - q[0].x);
    float dv = (q[3].v - q[0].v) / (q[3].y - q[0].y);
    float ub = -du * 0.5f + integral_sample_bias(q[0].x, q[3].x - q[0].x, q[0].u, du);
    float vb = -dv * 0.5f + integral_sample_bias(q[0].y, q[3].y - q[0].y, q[0].v, dv);
    float u0 = (q[0].u + ub + (x0 - q[0].x) * du) / width;
    float v0 = (q[0].v + vb + (y0 - q[0].y) * dv) / height;
    for (unsigned pass = 0; pass < passes; ++pass) {
        pvr_poly_hdr_t header;
        pvr_vertex_t vertices_out[4];
        transient_header(texture, pass, width, height, &header);
        vertices(vertices_out, x0, y0, x1 - x0, y1 - y0, u0, v0,
                 (x1 - x0) * du / width, (y1 - y0) * dv / height,
                 vertex_tint ? tint(rgb, pass ? 128 : 255) :
                               pass ? 0x80ffffff : 0xffffffff);
        if (append(&header, vertices_out, error, size)) goto done;
    }
    result = 0;
done:
    free(staging);
    active_transient_pin = pin.previous;
    /* A retained hit creates no transient; frame retirement handles cleanup. */
    if (!cached_title || result) release_transients();
    return result;
}

static int native_image(void *data, const gex_gpu_vertex q[4],
                        int left, int top, int right, int bottom,
                        const gex_gpu_image *image, char *error, size_t size) {
    return native_image_with_id(data, q, left, top, right, bottom, image, 0, 0, 0, error, size);
}

static int native_title_image(void *data, const gex_gpu_vertex q[4],
                              int left, int top, int right, int bottom,
                              const gex_gpu_image *image, uint64_t title_id,
                              unsigned rgb, int vertex_tint,
                              char *error, size_t size) {
    return native_image_with_id(data, q, left, top, right, bottom, image, title_id,
                                rgb, vertex_tint, error, size);
}

static int native_title_packet(void *target, uint32_t address, const uint8_t *packet,
                               const gex_raster_state *state, char *error, size_t size) {
    return gex_dreamcast_title_packet(target, address, packet, state, native_title_image,
                                      error, size);
}

const gex_packet_gpu *gex_packet_gpu_get(void) {
    static const gex_gpu_title_ops title = {gex_dreamcast_title_record, native_title_packet,
                                            gex_dreamcast_title_forget,
                                            gex_dreamcast_title_reset,
#ifdef GEX_DREAMCAST_NATIVE_MAP
                                            gex_dreamcast_backdrop_record,
                                            gex_dreamcast_map_cel_record
#else
                                            NULL, NULL
#endif
    };
    static const gex_gpu_native_ops native = {native_clear,   native_copy, native_blit,
                                              native_present,                                               native_bind, native_polygon,
                                              native_movie, native_atlas_write, native_packet,
                                              native_atlas_write_rect, native_polygon_scaled,
                                              native_image, &title,
#ifdef GEX_DREAMCAST_NATIVE_MAP
                                              gex_dreamcast_map_chunk, gex_dreamcast_map_flipped_cel,
                                              gex_dreamcast_map_unflipped_cel,
                                              gex_dreamcast_map_scaled_cel
#else
                                              NULL, NULL, NULL, NULL
#endif
    };
    static const gex_packet_gpu backend = {create, destroy, upload, download,
                                           draw,   history, 1,      &native};
    return &backend;
}
