#include "title_draw.h"
#include "title_cel.h"
#include "title_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TITLE_PART_LIMIT = 512, TITLE_STORAGE_LIMIT = 2 * 1024 * 1024,
       TITLE_MARKER_SIZE = 8, TITLE_PAIR_SIZE = 16, TITLE_OPCODE = 0x7f,
       TITLE_BUCKETS = 4096, TITLE_DIRECT_BUCKETS = 256,
       TITLE_ASSET_BUCKETS = 256, MAP_LOCAL_PARTS = 16 };

typedef struct {
    uint8_t modulation[1024];
    uint32_t visibility[8];
    unsigned rgb, semitransparent;
    int valid, blended;
    uint64_t image_id;
    uint16_t planes[];
} title_colors;

typedef struct title_asset {
    gex_title_cel_snapshot image;
    title_colors *colors;
    size_t bytes;
    unsigned references, variants, backdrop;
    struct title_asset *owner;
    struct title_asset *next;
    struct title_asset *indexed_next;
} title_asset;

typedef struct title_draw title_draw;
typedef struct palette_transfer {
    uint32_t anchor;
    gex_gpu_palette_upload upload;
    struct palette_transfer *next;
} palette_transfer;

typedef struct title_marker {
    uint32_t address, root;
    title_draw *draw;
    struct title_marker *next;
    struct title_marker **link, *root_next, **root_link;
} title_marker;

struct title_draw {
    title_marker marker[2];
    union {
        struct {
            int x, y, wrap_x, wrap_y;
            unsigned rgb, semitransparent, world, backdrop;
            title_asset *asset;
        };
        gex_dreamcast_tile_draw tile;
    };
    unsigned is_tile;
    title_draw *next;
};

static title_asset *assets;
static title_asset *asset_index[TITLE_ASSET_BUCKETS];
static title_marker *markers[TITLE_BUCKETS];
static title_marker *direct_markers[TITLE_DIRECT_BUCKETS];
static title_marker **direct_tails[TITLE_DIRECT_BUCKETS];
static title_marker *root_markers[TITLE_DIRECT_BUCKETS];
static title_marker **root_tails[TITLE_DIRECT_BUCKETS];
static size_t direct_count;
static size_t storage;
static uint64_t next_image_id;
static palette_transfer *palette_transfers;
static palette_transfer **palette_tail = &palette_transfers;

static unsigned asset_bucket(uint32_t image) {
    return (image * UINT32_C(2654435761)) >> 24;
}

static void insert_asset(title_asset *asset) {
    unsigned bucket = asset_bucket(asset->image.image_address);
    asset->next = assets;
    assets = asset;
    asset->indexed_next = asset_index[bucket];
    asset_index[bucket] = asset;
}

static int make_room(size_t bytes) {
    for (title_asset *asset = assets;
         bytes > TITLE_STORAGE_LIMIT - storage && asset; asset = asset->next) {
        if (!asset->colors) continue;
        size_t used = sizeof(title_colors) + (size_t)asset->image.cel.width *
                      asset->image.cel.height * 2 * sizeof(uint16_t);
        free(asset->colors);
        asset->colors = NULL;
        asset->bytes -= used;
        storage -= used;
    }
    return bytes <= TITLE_STORAGE_LIMIT - storage;
}

static int fail(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    return -1;
}

static int read_bytes(const gex_gpu_guest_io *io, uint32_t address, void *out, size_t size) {
    return io->read(io->context, address, out, size);
}

static int read16(const gex_gpu_guest_io *io, uint32_t address, uint16_t *value) {
    uint8_t b[2];
    if (read_bytes(io, address, b, sizeof b)) return -1;
    *value = b[0] | (uint16_t)b[1] << 8;
    return 0;
}

static int read32(const gex_gpu_guest_io *io, uint32_t address, uint32_t *value) {
    uint8_t b[4];
    if (read_bytes(io, address, b, sizeof b)) return -1;
    *value = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}

static int matches_backdrop(const gex_gpu_guest_io *io, const gex_title_cel_snapshot *image) {
    if (io->matches)
        return io->matches(io->context, image->image_address,
                           image->source, image->source_size) == 1;
    uint8_t bytes[128];
    for (size_t offset = 0; offset < image->source_size; offset += sizeof bytes) {
        size_t count = image->source_size - offset;
        if (count > sizeof bytes) count = sizeof bytes;
        if (read_bytes(io, image->image_address + (uint32_t)offset, bytes, count) ||
            memcmp(bytes, image->source + offset, count)) return 0;
    }
    return 1;
}

static int capture_backdrop(const gex_gpu_guest_io *io, uint32_t palette,
                            gex_title_cel_snapshot *snapshot) {
    enum { HEADER = 28, WIDTH = 64, HEIGHT = 32, BYTES = WIDTH * HEIGHT / 2 };
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->source = malloc(HEADER + BYTES);
    snapshot->pixels = malloc(WIDTH * HEIGHT * sizeof *snapshot->pixels);
    if (!snapshot->source || !snapshot->pixels) {
        gex_title_cel_release(snapshot);
        return GEX_TITLE_CEL_NO_MEMORY;
    }
    if (read_bytes(io, 0x460258, snapshot->source, HEADER + BYTES) ||
        read_bytes(io, palette, snapshot->palette, 32)) {
        gex_title_cel_release(snapshot);
        return GEX_TITLE_CEL_INVALID;
    }
    for (unsigned i = 0; i < WIDTH * HEIGHT; ++i) {
        unsigned value = snapshot->source[HEADER + i / 2];
        snapshot->pixels[i] = 0x10000u | ((value >> ((i & 1) * 4)) & 15);
    }
    snapshot->cel = (gex_title_cel){0, 0, WIDTH, HEIGHT, 0, 1};
    snapshot->source_size = HEADER + BYTES;
    snapshot->image_address = 0x460258;
    snapshot->palette_address = palette;
    snapshot->palette_size = 32;
    return GEX_TITLE_CEL_OK;
}

static void write32(const gex_gpu_guest_io *io, uint32_t address, uint32_t value) {
    uint8_t b[4] = {(uint8_t)value, (uint8_t)(value >> 8),
                    (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    io->write(io->context, address, b, sizeof b);
}

static int same_asset(const gex_title_cel_snapshot *a, const gex_title_cel_snapshot *b) {
    const gex_title_cel *x = &a->cel, *y = &b->cel;
    return x->left == y->left && x->top == y->top && x->width == y->width &&
           x->height == y->height && x->format == y->format && x->tiles == y->tiles &&
           a->palette_size == b->palette_size &&
           !memcmp(a->palette, b->palette, a->palette_size) &&
           !memcmp(a->pixels, b->pixels, (size_t)x->width * x->height * sizeof *a->pixels);
}

static void discard_asset(title_asset *asset) {
    title_asset *owner = asset->owner;
    int variant = owner != asset;
    title_asset **link = &assets;
    while (*link != asset) link = &(*link)->next;
    *link = asset->next;
    link = &asset_index[asset_bucket(asset->image.image_address)];
    while (*link != asset) link = &(*link)->indexed_next;
    *link = asset->indexed_next;
    storage -= asset->bytes;
    free(asset->colors);
    if (!variant) gex_title_cel_release(&asset->image);
    else --owner->variants;
    free(asset);
    if (variant && !owner->variants && !owner->references) discard_asset(owner);
}

static void release_asset(title_asset *asset) {
    if (--asset->references || asset->variants) return;
    discard_asset(asset);
}

static void release_draw(title_draw *draw) {
    if (!draw->is_tile) release_asset(draw->asset);
    storage -= sizeof *draw;
    free(draw);
}

static void release_pending(title_draw *pending) {
    while (pending) {
        title_draw *next = pending->next;
        release_draw(pending);
        pending = next;
    }
}

static unsigned direct_bucket(uint32_t address) {
    return (address >> 2) * UINT32_C(2654435761) >> 24;
}

static void append_direct(title_marker *marker) {
    unsigned bucket = direct_bucket(marker->address);
    unsigned root_bucket = direct_bucket(marker->root);
    if (!direct_tails[bucket]) direct_tails[bucket] = &direct_markers[bucket];
    if (!root_tails[root_bucket]) root_tails[root_bucket] = &root_markers[root_bucket];
    marker->next = NULL;
    marker->link = direct_tails[bucket];
    *direct_tails[bucket] = marker;
    direct_tails[bucket] = &marker->next;
    marker->root_next = NULL;
    marker->root_link = root_tails[root_bucket];
    *root_tails[root_bucket] = marker;
    root_tails[root_bucket] = &marker->root_next;
}

static void unlink_direct(title_marker *marker) {
    unsigned bucket = direct_bucket(marker->address);
    unsigned root_bucket = direct_bucket(marker->root);
    *marker->link = marker->next;
    if (marker->next) marker->next->link = marker->link;
    else direct_tails[bucket] = marker->link;
    *marker->root_link = marker->root_next;
    if (marker->root_next) marker->root_next->root_link = marker->root_link;
    else root_tails[root_bucket] = marker->root_link;
}

int gex_dreamcast_palette_record(uint32_t anchor, const gex_gpu_palette_upload *upload) {
    if (!anchor || (anchor & 3) || !upload || !upload->source ||
        (upload->width != 16 && upload->width != 256) ||
        !make_room(sizeof(palette_transfer))) return 0;
    palette_transfer *transfer = malloc(sizeof *transfer);
    if (!transfer) return -1;
    *transfer = (palette_transfer){anchor, *upload, NULL};
    *palette_tail = transfer;
    palette_tail = &transfer->next;
    storage += sizeof *transfer;
    return 1;
}

int gex_dreamcast_palette_take(uint32_t anchor, gex_gpu_palette_upload *upload) {
    if (!upload) return 0;
    palette_transfer **link = &palette_transfers;
    while (*link && (*link)->anchor != anchor) link = &(*link)->next;
    if (!*link) return 0;
    palette_transfer *transfer = *link;
    *upload = transfer->upload;
    *link = transfer->next;
    if (!*link) palette_tail = link;
    storage -= sizeof *transfer;
    free(transfer);
    return 1;
}

void gex_dreamcast_palette_reset(void) {
    while (palette_transfers) {
        palette_transfer *transfer = palette_transfers;
        palette_transfers = transfer->next;
        storage -= sizeof *transfer;
        free(transfer);
    }
    palette_tail = &palette_transfers;
}

static void forget_palettes(uint32_t begin, uint32_t end) {
    palette_transfer **link = &palette_transfers;
    while (*link) {
        palette_transfer *transfer = *link;
        if (transfer->anchor >= end || (uint64_t)transfer->anchor + 4 <= begin) {
            link = &transfer->next;
            continue;
        }
        *link = transfer->next;
        if (!*link) palette_tail = link;
        storage -= sizeof *transfer;
        free(transfer);
    }
}

static int record_tiles(const gex_gpu_guest_io *io, const gex_dreamcast_tile_draw *tiles,
                        size_t count, const uint32_t *roots, const uint32_t *tails,
                        unsigned lists) {
    if ((!tiles && count) || !tails) return 0;
    for (unsigned i = 0; i < lists; ++i)
        if (!tails[i] || (tails[i] & 3)) return 0;
    for (unsigned i = 0; i < lists; ++i)
        for (unsigned j = i + 1; j < lists; ++j)
            if (tails[i] == tails[j]) return 0;
    title_draw *pending = NULL, **next = &pending;
    for (size_t i = 0; i < count; ++i) {
        if (tiles[i].layer >= lists / 2 || (tiles[i].palette_address && (!io || !io->prepare_palette)) ||
            !make_room(sizeof(title_draw))) {
            release_pending(pending);
            return 0;
        }
        title_draw *draw = malloc(sizeof *draw);
        if (!draw) {
            release_pending(pending);
            return -1;
        }
        storage += sizeof *draw;
        unsigned list = tiles[i].layer * 2;
        *draw = (title_draw){.marker = {{.address = tails[list], .root = roots[list], .draw = draw},
                                        {.address = tails[list + 1], .root = roots[list + 1], .draw = draw}},
                             .tile = tiles[i], .is_tile = 1};
        *next = draw;
        next = &draw->next;
    }
    while (pending) {
        title_draw *draw = pending;
        pending = draw->next;
        /* Palette uploads can retire anchors used by earlier direct tiles. */
        if (draw->tile.palette_address)
            draw->tile.clut = io->prepare_palette(io->context, draw->tile.palette_address);
        append_direct(&draw->marker[0]);
        append_direct(&draw->marker[1]);
        direct_count += 2;
    }
    return 1;
}

int gex_dreamcast_title_record_tiles(const gex_gpu_guest_io *io,
                                     const gex_dreamcast_tile_draw *tiles, size_t count,
                                     const uint32_t tails[4]) {
    static const uint32_t roots[4] = {0x46bc64, 0x46bc60, 0x46bc58, 0x46bc5c};
    return record_tiles(io, tiles, count, roots, tails, 4);
}

int gex_dreamcast_title_record_cel_tiles(const gex_dreamcast_tile_draw *tiles, size_t count,
                                         const uint32_t roots[2], const uint32_t tails[2]) {
    if (!roots || !roots[0] || !roots[1] || roots[0] == roots[1]) return 0;
    return record_tiles(NULL, tiles, count, roots, tails, 2);
}

void gex_dreamcast_title_merge_lists(const uint32_t source[2], const uint32_t roots[2],
                                      const uint32_t tails[2]) {
    for (unsigned copy = 0; copy < 2; ++copy) {
        title_marker *moved = NULL, **last = &moved;
        title_marker *marker = root_markers[direct_bucket(source[copy])];
        while (marker) {
            title_marker *next = marker->root_next;
            if (marker->root == source[copy]) {
                unlink_direct(marker);
                if (marker->address == source[copy]) marker->address = tails[copy];
                marker->root = roots[copy];
                marker->next = NULL;
                *last = marker;
                last = &marker->next;
            }
            marker = next;
        }
        while (moved) {
            marker = moved;
            moved = marker->next;
            append_direct(marker);
        }
    }
}

static void release_marker(title_marker *marker) {
    title_draw *draw = marker->draw;
    marker->address = 0;
    if (!draw->marker[0].address && !draw->marker[1].address)
        release_draw(draw);
}

static void forget_direct(uint32_t begin, uint32_t end) {
    if (!direct_count) return;
    uint32_t start = begin >= 3 ? begin - 3 : 0;
    start &= ~3u;
    for (uint64_t at = start; at < end; at += 4) {
        unsigned bucket = direct_bucket((uint32_t)at);
        title_marker **link = &direct_markers[bucket];
        while (*link) {
            title_marker *marker = *link;
            if (marker->address != at || marker->address >= end ||
                (uint64_t)marker->address + 4 <= begin) {
                link = &marker->next;
                continue;
            }
            unlink_direct(marker);
            --direct_count;
            release_marker(marker);
        }
    }
}

void gex_dreamcast_title_reset_lists(uint32_t first, uint32_t second) {
    if (!direct_count) return;
    const uint32_t roots[2] = {first, second};
    for (unsigned i = 0; i < 2; ++i) {
        title_marker *marker = root_markers[direct_bucket(roots[i])];
        while (marker) {
            title_marker *next = marker->root_next;
            if (marker->root == roots[i]) {
                unlink_direct(marker);
                --direct_count;
                release_marker(marker);
            }
            marker = next;
        }
    }
}

int gex_dreamcast_title_has_direct(uint32_t root, uint32_t anchor) {
    if (!direct_count) return 0;
    for (title_marker *marker = direct_markers[direct_bucket(anchor)]; marker;
         marker = marker->next)
        if (marker->root == root && marker->address == anchor) return 1;
    return 0;
}

void gex_dreamcast_title_forget(uint32_t begin, uint32_t end) {
    if (begin >= end) return;
    forget_direct(begin, end);
    forget_palettes(begin, end);
    uint32_t start = begin >= TITLE_MARKER_SIZE - 1 ? begin - (TITLE_MARKER_SIZE - 1) : 0;
    start &= ~3u;
    for (uint64_t at = start; at < end; at += 4) {
        unsigned bucket = ((uint32_t)at >> 2) * 2654435761u >> 20;
        title_marker **link = &markers[bucket];
        while (*link) {
            title_marker *marker = *link;
            if (marker->address != at || marker->address >= end ||
                (uint64_t)marker->address + TITLE_MARKER_SIZE <= begin) {
                link = &marker->next;
                continue;
            }
            *link = marker->next;
            release_marker(marker);
        }
    }
}

void gex_dreamcast_title_reset(void) {
    gex_dreamcast_palette_reset();
    for (unsigned i = 0; i < TITLE_DIRECT_BUCKETS; ++i)
        while (direct_markers[i]) {
            title_marker *marker = direct_markers[i];
            unlink_direct(marker);
            --direct_count;
            release_marker(marker);
        }
    memset(direct_tails, 0, sizeof direct_tails);
    memset(root_tails, 0, sizeof root_tails);
    for (unsigned i = 0; i < TITLE_BUCKETS; ++i)
        while (markers[i]) {
            title_marker *marker = markers[i];
            markers[i] = marker->next;
            release_marker(marker);
        }
}

static int record(const gex_gpu_guest_io *io, uint32_t object, uint32_t cel, int backdrop,
                  int map, uint32_t scene, const uint32_t *roots, char *error, size_t size) {
    if (!io || !io->read || !io->write || !io->allocated) return 0;
    uint32_t camera_x, camera_y, frame, fade, colors[32], begin, end, cursor, tails[2];
    uint16_t screen_x, screen_y;
    uint8_t fade_rgb[3], color_bytes[32 * 4];
    if (read32(io, 0x4a2974, &camera_x) || read32(io, 0x4a2988, &camera_y) ||
        read32(io, 0x4a2ac8, &frame) || read16(io, 0x4a2a96, &screen_x) ||
        read16(io, 0x4a2a94, &screen_y) || read32(io, 0x4a2afc, &fade) ||
        read_bytes(io, 0x4a2af8, fade_rgb, sizeof fade_rgb) ||
        read32(io, 0x4a2ae0, &begin) || read32(io, 0x4a2adc, &end) ||
        read32(io, 0x4a2ae4, &cursor) || read32(io, 0x4a2b18, &tails[0]) ||
        read32(io, 0x4a2b14, &tails[1]) ||
        read_bytes(io, 0x460048, color_bytes, sizeof color_bytes))
        return 0;
    uint8_t swap = fade_rgb[0]; fade_rgb[0] = fade_rgb[2]; fade_rgb[2] = swap;
    for (unsigned i = 0; i < 32; ++i)
        colors[i] = (uint32_t)color_bytes[i * 4] |
                    (uint32_t)color_bytes[i * 4 + 1] << 8 |
                    (uint32_t)color_bytes[i * 4 + 2] << 16 |
                    (uint32_t)color_bytes[i * 4 + 3] << 24;
    if (!begin || (begin & 3) || (cursor & 3) ||
        end <= begin || cursor < begin || cursor > end ||
        !tails[0] || !tails[1] ||
        (roots && (tails[0] == tails[1] || ((tails[0] | tails[1]) & 3)))) return 0;

    gex_title_part local_parts[MAP_LOCAL_PARTS];
    gex_title_part *parts = map ? local_parts : malloc(TITLE_PART_LIMIT * sizeof *parts);
    if (!parts) return fail(error, size, "cannot allocate title draw plan");
    size_t count = 0;
    int status;
    if (backdrop) {
        uint32_t palette;
        uint8_t header[28];
        if (scene != 49 ||
            read32(io, 0x46a534, &palette) || !palette ||
            read_bytes(io, 0x460258, header, sizeof header) ||
            (header[16] & 0x7f) || header[20] != 28 || header[21] ||
            header[22] != 64 || header[23] != 32 ||
            header[24] || header[25] || header[26] || header[27]) {
            free(parts);
            return 0;
        }
        for (int x = 0; x < 320; x += 64)
            for (int y = 0; y < 240; y += 32)
                parts[count++] = (gex_title_part){0x460258, palette, 0, 0, x, y, 0, 0};
        status = GEX_TITLE_PLAN_OK;
    } else {
        status = gex_title_plan(io->read, io->context, object, cel,
                                camera_x, camera_y, frame, screen_x, screen_y, 320, 240,
                                parts, map ? MAP_LOCAL_PARTS : TITLE_PART_LIMIT, &count);
        if (map && status == GEX_TITLE_PLAN_CAPACITY) {
            parts = malloc(TITLE_PART_LIMIT * sizeof *parts);
            if (!parts) return fail(error, size, "cannot allocate title draw plan");
            status = gex_title_plan(io->read, io->context, object, cel,
                                    camera_x, camera_y, frame, screen_x, screen_y, 320, 240,
                                    parts, TITLE_PART_LIMIT, &count);
        }
    }
    if (status != GEX_TITLE_PLAN_OK || (!roots && count * TITLE_PAIR_SIZE > end - begin)) {
        if (parts != local_parts) free(parts);
        return 0;
    }
    title_draw *pending = NULL, **tail = &pending;
    title_asset *backdrop_asset = NULL;
    uint32_t position = cursor, first = 0;
    int wrapped = 0;
    for (size_t i = 0; i < count; ++i) {
        if (parts[i].flags & 0xc0000000u) { status = 0; goto rollback; }
        if (!roots && end - position < TITLE_PAIR_SIZE) {
            position = begin;
            wrapped = 1;
            if (position + TITLE_PAIR_SIZE == cursor) { status = 0; goto rollback; }
        }
        if (!roots && i && wrapped && position + TITLE_PAIR_SIZE > first) {
            status = 0; goto rollback;
        }
        if (!i) { first = position; wrapped = 0; }
        for (unsigned j = 0; !roots && j < 2; ++j)
            if (tails[j] >= position && tails[j] < position + TITLE_PAIR_SIZE) {
                status = 0; goto rollback;
            }
        title_asset *asset = backdrop_asset;
        if (!asset)
            for (asset = asset_index[asset_bucket(parts[i].image)]; asset;
                 asset = asset->indexed_next)
                if (asset->owner == asset && asset->backdrop == (unsigned)backdrop &&
                    asset->image.image_address == parts[i].image &&
                    (backdrop ? matches_backdrop(io, &asset->image) :
                     gex_title_cel_matches_image(io->read, io->matches, io->context, parts[i].image,
                                                 &asset->image))) break;
        if (asset && !backdrop_asset) {
            title_asset *owner = asset->owner;
            uint8_t palette[512];
            size_t palette_size = owner->image.palette_size;
            if (palette_size && (!parts[i].palette ||
                read_bytes(io, parts[i].palette, palette, palette_size))) {
                status = 0; goto rollback;
            }
            for (asset = asset_index[asset_bucket(owner->image.image_address)]; asset;
                 asset = asset->indexed_next)
                if (asset->owner == owner &&
                    !memcmp(asset->image.palette, palette, palette_size)) break;
            if (!asset) {
                if (!make_room(sizeof *asset)) { status = 0; goto rollback; }
                asset = malloc(sizeof *asset);
                if (!asset) { status = -1; goto rollback; }
                *asset = (title_asset){.image = owner->image, .bytes = sizeof *asset,
                                       .backdrop = owner->backdrop,
                                       .owner = owner};
                if (palette_size) memcpy(asset->image.palette, palette, palette_size);
                asset->image.palette_address = parts[i].palette;
                ++owner->variants;
                insert_asset(asset);
                storage += sizeof *asset;
            }
        }
        if (!asset) {
            gex_title_cel_snapshot snapshot;
            int captured = backdrop ? capture_backdrop(io, parts[i].palette, &snapshot) :
                map ? gex_title_cel_capture_map(io->read, io->context, parts[i].image,
                                                parts[i].palette, &snapshot) :
                      gex_title_cel_capture(io->read, io->context, parts[i].image,
                                            parts[i].palette, &snapshot);
            if (captured != GEX_TITLE_CEL_OK) {
                status = captured == GEX_TITLE_CEL_NO_MEMORY ? -1 : 0;
                goto rollback;
            }
            for (asset = assets; asset; asset = asset->next)
                if (asset->backdrop == (unsigned)backdrop &&
                    same_asset(&asset->image, &snapshot)) break;
            if (asset) {
                gex_title_cel_release(&snapshot);
            } else {
                size_t bytes = sizeof *asset + snapshot.source_size +
                               (size_t)snapshot.cel.width * snapshot.cel.height * sizeof *snapshot.pixels;
                if (!make_room(bytes)) {
                    gex_title_cel_release(&snapshot);
                    status = 0; goto rollback;
                }
                asset = malloc(sizeof *asset);
                if (!asset) {
                    gex_title_cel_release(&snapshot);
                    status = -1; goto rollback;
                }
                *asset = (title_asset){.image = snapshot, .bytes = bytes,
                                       .backdrop = (unsigned)backdrop};
                asset->owner = asset;
                insert_asset(asset);
                storage += bytes;
            }
        }
        if (backdrop) backdrop_asset = asset;
        ++asset->references;
        if (!make_room(sizeof(title_draw))) {
            release_asset(asset);
            status = 0; goto rollback;
        }
        title_draw *draw = malloc(sizeof *draw);
        if (!draw) {
            release_asset(asset);
            status = -1; goto rollback;
        }
        storage += sizeof *draw;
        *draw = (title_draw){.marker = {{.address = roots ? tails[0] : position,
                                         .root = roots ? roots[0] : 0, .draw = draw},
                                        {.address = roots ? tails[1] : position + TITLE_MARKER_SIZE,
                                         .root = roots ? roots[1] : 0, .draw = draw}},
                             .x = parts[i].x, .y = parts[i].y,
                             .wrap_x = parts[i].wrap_x, .wrap_y = parts[i].wrap_y,
                             .rgb = backdrop ? (uint32_t)fade_rgb[0] |
                                               (uint32_t)fade_rgb[1] << 8 |
                                               (uint32_t)fade_rgb[2] << 16 :
                                    gex_title_packet_rgb(parts[i].tint, fade, fade_rgb, colors),
                             .semitransparent = !backdrop && !!(parts[i].tint & 0x8080u),
                             .world = roots && scene == 3,
                             .backdrop = roots && backdrop,
                             .asset = asset};
        *tail = draw;
        tail = &draw->next;
        if (!roots) position += TITLE_PAIR_SIZE;
    }
    if (parts != local_parts) free(parts);
    if (roots) {
        while (pending) {
            title_draw *draw = pending;
            pending = pending->next;
            for (unsigned i = 0; i < 2; ++i) {
                append_direct(&draw->marker[i]);
                ++direct_count;
            }
        }
        return 1;
    }
    while (pending) {
        title_draw *end = pending;
        /* The allocation callback detects a wrap only when next < cursor. */
        int wrapped = end->marker[0].address + TITLE_PAIR_SIZE < cursor;
        while (end->next && end->next->marker[0].address ==
                            end->marker[0].address + TITLE_PAIR_SIZE &&
               (!wrapped || end->next->marker[0].address + TITLE_PAIR_SIZE < cursor))
            end = end->next;
        title_draw *after = end->next;
        io->allocated(io->context, end->marker[0].address + TITLE_PAIR_SIZE);
        do {
            title_draw *draw = pending;
            pending = pending->next;
            uint32_t next = draw->marker[0].address + TITLE_PAIR_SIZE;
            write32(io, 0x4a2ae4, next);
            cursor = next;
            for (unsigned i = 0; i < 2; ++i) {
                title_marker *marker = &draw->marker[i];
                write32(io, marker->address, 0x00ffffffu);
                write32(io, marker->address + 4, (draw->rgb & 0xffffffu) | TITLE_OPCODE << 24);
                write32(io, tails[i], marker->address);
                tails[i] = marker->address;
                write32(io, i ? 0x4a2b14 : 0x4a2b18, tails[i]);
                unsigned bucket = (marker->address >> 2) * 2654435761u >> 20;
                marker->next = markers[bucket];
                markers[bucket] = marker;
            }
        } while (pending != after);
    }
    return 1;
rollback:
    if (parts != local_parts) free(parts);
    release_pending(pending);
    return status < 0 ? fail(error, size, "cannot allocate title image snapshot") : 0;
}

int gex_dreamcast_title_record(const gex_gpu_guest_io *io, uint32_t object, uint32_t cel,
                               char *error, size_t size) {
    return record(io, object, cel, 0, 0, 63, NULL, error, size);
}

int gex_dreamcast_title_record_direct(const gex_gpu_guest_io *io, uint32_t object,
                                      uint32_t cel, const uint32_t roots[2],
                                      char *error, size_t size) {
    if (!roots || !roots[0] || !roots[1] || roots[0] == roots[1]) return 0;
    return record(io, object, cel, 0, 0, 63, roots, error, size);
}

int gex_dreamcast_backdrop_record(const gex_gpu_guest_io *io, uint32_t scene,
                                  char *error, size_t size) {
    return record(io, 0, 0, 1, 0, scene, NULL, error, size);
}

int gex_dreamcast_backdrop_record_direct(const gex_gpu_guest_io *io, uint32_t scene,
                                         const uint32_t roots[2], char *error, size_t size) {
    if (scene != 49 || !roots || !roots[0] || !roots[1] || roots[0] == roots[1]) return 0;
    return record(io, 0, 0, 1, 0, scene, roots, error, size);
}

int gex_dreamcast_map_cel_record(const gex_gpu_guest_io *io, uint32_t scene,
                                 uint32_t object, uint32_t cel, char *error, size_t size) {
    return scene == 49 ? record(io, object, cel, 0, 1, scene, NULL, error, size) : 0;
}

int gex_dreamcast_scene_cel_record_direct(const gex_gpu_guest_io *io, uint32_t scene,
                                          uint32_t object, uint32_t cel,
                                          const uint32_t roots[2], char *error, size_t size) {
    if ((scene != 49 && scene != 3) || !roots || !roots[0] || !roots[1] ||
        roots[0] == roots[1]) return 0;
    return record(io, object, cel, 0, 1, scene, roots, error, size);
}

static int signed16(int value) {
    unsigned v = (unsigned)value & 0xffffu;
    return v & 0x8000u ? (int)v - 65536 : (int)v;
}

static int submit_draw(void *target, title_draw *draw, unsigned copy, unsigned rgb,
                       const gex_raster_state *state, gex_title_image_submit submit,
                       char *error, size_t size) {
    if (!state || !submit) return fail(error, size, "invalid deferred title draw");
    const gex_title_cel_snapshot *snapshot = &draw->asset->image;
    int x = signed16(draw->x + snapshot->cel.left - (copy ? draw->wrap_x : 0));
    int y = signed16(draw->y + snapshot->cel.top - (copy ? draw->wrap_y : 0));
    int left = state->left > 0 ? state->left : 0;
    int right = state->right < 320 ? state->right : 320;
    int top = state->top - state->origin_y;
    int bottom = state->bottom - state->origin_y;
    if (top < 8) top = 8;
    if (draw->world && !state->layout) {
        if (bottom == 232 || bottom == 240) bottom = 248;
        if (bottom > 248) bottom = 248;
        if (state->world_clip) {
            if (left < state->world_left) left = state->world_left;
            if (right > state->world_right) right = state->world_right;
            if (top < state->world_top) top = state->world_top;
            if (bottom > state->world_bottom) bottom = state->world_bottom;
        }
    } else if (bottom > 232) bottom = 232;
    int fitted_y = state->layout == 1 ? 8 : 0;
    int repeats = state->layout == 2 ? 2 : 1;
    int visible = 0;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        int offset_y = fitted_y + repeat * 224;
        int py = y - state->origin_y - 8 + offset_y;
        int clip_top = top - 8 + offset_y, clip_bottom = bottom - 8 + offset_y;
        if (clip_top < 0) clip_top = 0;
        if (clip_bottom > 240) clip_bottom = 240;
        if (left < right && clip_top < clip_bottom &&
            x < right && x + (int)snapshot->cel.width > left &&
            py < clip_bottom && py + (int)snapshot->cel.height > clip_top) {
            visible = 1;
            break;
        }
    }
    if (!visible) return 1;
    size_t count = (size_t)snapshot->cel.width * snapshot->cel.height;
    size_t color_bytes = sizeof(title_colors) + count * 2 * sizeof(uint16_t);
    title_colors *colors = draw->asset->colors;
    int temporary = 0;
    if (!colors) {
        colors = malloc(color_bytes);
        if (!colors) return fail(error, size, "cannot allocate title color planes");
        colors->valid = 0;
        if (color_bytes <= TITLE_STORAGE_LIMIT - storage) {
            draw->asset->colors = colors;
            draw->asset->bytes += color_bytes;
            storage += color_bytes;
        } else {
            temporary = 1;
        }
    }
    uint16_t *planes = colors->planes;
    if (!colors->valid || colors->rgb != rgb || colors->semitransparent != draw->semitransparent ||
        !state->modulation || memcmp(colors->modulation, state->modulation, sizeof colors->modulation)) {
        uint32_t visibility[8];
        if (gex_title_cel_visibility(&snapshot->cel, snapshot->palette,
                                     snapshot->palette_size, state->modulation,
                                     rgb, visibility)) {
            if (temporary) free(colors);
            return fail(error, size, "cannot convert deferred title colors");
        }
        if (!colors->valid || colors->semitransparent != draw->semitransparent ||
            memcmp(colors->visibility, visibility, sizeof visibility)) {
            colors->valid = 0;
            if (gex_title_cel_colors(&snapshot->cel, snapshot->pixels, count,
                                     snapshot->palette, snapshot->palette_size, state->modulation,
                                     rgb, draw->semitransparent, planes, planes + count)) {
                if (temporary) free(colors);
                return fail(error, size, "cannot convert deferred title colors");
            }
            colors->blended = 0;
            for (size_t i = 0; i < count; ++i) colors->blended |= planes[count + i] != 0;
            colors->image_id = !temporary && next_image_id != UINT64_MAX ? ++next_image_id : 0;
            memcpy(colors->visibility, visibility, sizeof visibility);
        }
        colors->rgb = rgb;
        colors->semitransparent = draw->semitransparent;
        if (state->modulation) {
            memcpy(colors->modulation, state->modulation, sizeof colors->modulation);
            colors->valid = 1;
        }
    }
    gex_gpu_image image = {planes, colors->blended ? planes + count : NULL,
                           snapshot->cel.width, snapshot->cel.height};
    int vertex_tint = snapshot->cel.format < 2 ||
                      (!draw->semitransparent && (rgb & 255) != 128 &&
                       ((rgb >> 8) & 255) != 128 && ((rgb >> 16) & 255) != 128);
    int result = 1;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        int offset_y = fitted_y + repeat * 224;
        float py = (float)(y - state->origin_y - 8 + offset_y);
        gex_gpu_vertex q[] = {{(float)x, py, 0, 0},
            {(float)(x + image.width), py, (float)image.width, 0},
            {(float)x, py + image.height, 0, (float)image.height},
            {(float)(x + image.width), py + image.height,
             (float)image.width, (float)image.height}};
        int clip_top = top - 8 + offset_y, clip_bottom = bottom - 8 + offset_y;
        if (clip_top < 0) clip_top = 0;
        if (clip_bottom > 240) clip_bottom = 240;
        if (left < right && clip_top < clip_bottom &&
            submit(target, q, left, clip_top, right, clip_bottom, &image,
                   colors->image_id, rgb, vertex_tint, error, size)) {
            result = -1;
            break;
        }
    }
    if (temporary) free(colors);
    return result;
}

int gex_dreamcast_title_packet(void *target, uint32_t address, const uint8_t *packet,
                               const gex_raster_state *state, gex_title_image_submit submit,
                               char *error, size_t size) {
    unsigned bucket = (address >> 2) * 2654435761u >> 20;
    title_marker *marker;
    for (marker = markers[bucket]; marker; marker = marker->next)
        if (marker->address == address) break;
    if (!marker) return 0;
    if (!packet || packet[7] != TITLE_OPCODE)
        return fail(error, size, "invalid deferred title packet");
    unsigned rgb = packet[4] | (unsigned)packet[5] << 8 | (unsigned)packet[6] << 16;
    return submit_draw(target, marker->draw, marker == &marker->draw->marker[1],
                       rgb, state, submit, error, size);
}

static int submit_tile(void *target, const gex_dreamcast_tile_draw *tile, unsigned copy,
                       const gex_raster_state *state, gex_title_polygon_submit submit,
                       gex_gpu_direct_state *result, char *error, size_t size) {
    if (!state || !submit || !result) return fail(error, size, "invalid direct tile draw");
    unsigned page = result->page_valid ? result->page & 0x19f : state->page;
    if (tile->is_quad || tile->page_change) {
        result->page = tile->page;
        result->page_valid = 1;
        page = tile->is_quad ? tile->page : tile->page & 0x19f;
    }
    if (tile->page_change) {
        result->page_command = tile->page;
        result->page_command_valid = 1;
    }
    int left = state->left > 0 ? state->left : 0;
    int right = state->right < 320 ? state->right : 320;
    int top = state->top - state->origin_y;
    int bottom = state->bottom - state->origin_y;
    if (top < 8) top = 8;
    if (!state->layout) {
        if (bottom == 232 || bottom == 240) bottom = 248;
        if (bottom > 248) bottom = 248;
        if (state->world_clip) {
            if (left < state->world_left) left = state->world_left;
            if (right > state->world_right) right = state->world_right;
            if (top < state->world_top) top = state->world_top;
            if (bottom > state->world_bottom) bottom = state->world_bottom;
        }
    } else if (bottom > 232) bottom = 232;
    int offset = state->layout == 1 ? 8 : 0;
    int clip_top = top - 8 + offset, clip_bottom = bottom - 8 + offset;
    if (clip_top < 0) clip_top = 0;
    if (clip_bottom > 240) clip_bottom = 240;
    if (left >= right || clip_top >= clip_bottom) return 1;
    gex_gpu_vertex q[4];
    int x0 = signed16(tile->x[0] + (copy ? tile->wrap_x : 0));
    int y0 = signed16(tile->y[0] + (copy ? tile->wrap_y : 0));
    for (unsigned i = 0; i < 4; ++i) {
        int x = tile->is_quad ? signed16(tile->x[i] + (copy ? tile->wrap_x : 0)) :
                x0 + ((i & 1) ? tile->width : 0);
        int y = tile->is_quad ? signed16(tile->y[i] + (copy ? tile->wrap_y : 0)) :
                y0 + ((i & 2) ? tile->height : 0);
        unsigned u = tile->is_quad ? tile->u[i] : tile->u[0] + ((i & 1) ? tile->width : 0);
        unsigned v = tile->is_quad ? tile->v[i] : tile->v[0] + ((i & 2) ? tile->height : 0);
        if (tile->is_quad && (i == 1 || i == 3) && !u) u = 256;
        if (tile->is_quad && (i == 2 || i == 3) && !v) v = 256;
        q[i] = (gex_gpu_vertex){(float)x, (float)(y - state->origin_y - 8 + offset),
                                (float)u, (float)v};
    }
    gex_gpu_material material = {page, tile->clut,
                                 (tile->is_quad ? 0x2cu : 0x64u) | (tile->rgb >> 24 & 2u),
                                 tile->rgb & 0xffffffu, state->atlas, state->modulation};
    return submit(target, q, left, clip_top, right, clip_bottom, &material, error, size) ? -1 : 1;
}

int gex_dreamcast_title_draw_direct_tiles(void *target, uint32_t root, uint32_t anchor,
                                           const gex_raster_state *state,
                                           gex_title_image_submit image,
                                           gex_title_polygon_submit polygon,
                                           gex_gpu_direct_state *result,
                                           char *error, size_t size) {
    int found = 0;
    for (title_marker *marker = direct_markers[direct_bucket(anchor)]; marker;
         marker = marker->next) {
        if (marker->root != root || marker->address != anchor) continue;
        title_draw *draw = marker->draw;
        if (draw->is_tile) {
            if (submit_tile(target, &draw->tile, marker == &draw->marker[1], state,
                            polygon, result, error, size) < 0) return -1;
            found = 1;
            continue;
        }
        gex_raster_state backdrop_state;
        const gex_raster_state *draw_state = state;
        if (draw->backdrop && state && state->layout == 1 && state->repeat_backdrop) {
            backdrop_state = *state;
            backdrop_state.layout = 2;
            draw_state = &backdrop_state;
        }
        if (submit_draw(target, draw, marker == &draw->marker[1], draw->rgb,
                        draw_state, image, error, size) < 0) return -1;
        found = 1;
    }
    return found;
}

int gex_dreamcast_title_draw_direct(void *target, uint32_t root, uint32_t anchor,
                                     const gex_raster_state *state, gex_title_image_submit submit,
                                     char *error, size_t size) {
    gex_gpu_direct_state result = {0};
    return gex_dreamcast_title_draw_direct_tiles(target, root, anchor, state, submit, NULL,
                                                  &result, error, size);
}
