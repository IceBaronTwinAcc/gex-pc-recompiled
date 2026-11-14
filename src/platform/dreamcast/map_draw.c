#include "map_draw.h"
#include "map_plan.h"
#include "title_draw.h"
#include <string.h>

typedef struct {
    uint32_t sprite, page_packet, rgb, link, page_padding;
    uint16_t page, uv, x, y, width, height;
    uint8_t quad[40];
    unsigned layer, flipped, bytes;
} map_command;

typedef struct {
    uint32_t begin[2], end[2];
    unsigned count;
} map_reservations;

typedef struct {
    uint32_t begin, end;
} map_allocation;

static int read_bytes(const gex_gpu_guest_io *io, uint32_t address, void *out, size_t size) {
    if (!address || size > UINT32_MAX - address) return -1;
    return io->read(io->context, address, out, size);
}

static int read16(const gex_gpu_guest_io *io, uint32_t address, uint16_t *value) {
    uint8_t bytes[2];
    if (read_bytes(io, address, bytes, sizeof bytes)) return -1;
    *value = bytes[0] | (uint16_t)bytes[1] << 8;
    return 0;
}

static int read32(const gex_gpu_guest_io *io, uint32_t address, uint32_t *value) {
    uint8_t bytes[4];
    if (read_bytes(io, address, bytes, sizeof bytes)) return -1;
    *value = bytes[0] | (uint32_t)bytes[1] << 8 |
             (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return 0;
}

static void put16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static uint16_t word(const uint8_t *bytes) {
    return bytes[0] | (uint16_t)bytes[1] << 8;
}

static void put32(uint8_t *bytes, uint32_t value) {
    put16(bytes, (uint16_t)value);
    put16(bytes + 2, (uint16_t)(value >> 16));
}

static void write32(const gex_gpu_guest_io *io, uint32_t address, uint32_t value) {
    uint8_t bytes[4];
    put32(bytes, value);
    io->write(io->context, address, bytes, sizeof bytes);
}

static void write16(const gex_gpu_guest_io *io, uint32_t address, uint16_t value) {
    uint8_t bytes[2];
    put16(bytes, value);
    io->write(io->context, address, bytes, sizeof bytes);
}

static int reserve(uint32_t begin, uint32_t end, uint32_t *cursor, uint32_t bytes,
                   const uint32_t tails[4], map_reservations *reservations,
                   uint32_t *address) {
    uint32_t position = *cursor;
    if (end - position < bytes) {
        position = begin;
        /* The callback cannot identify a wrap if next reaches the old cursor. */
        if (position + bytes >= *cursor) return -1;
    }
    for (unsigned i = 0; i < 4; ++i)
        if (tails[i] >= position && tails[i] < position + bytes) return -1;
    for (unsigned i = 0; i < reservations->count; ++i)
        if (position < reservations->end[i] && reservations->begin[i] < position + bytes)
            return -1;
    /* Sequential reservations can form only one run on each side of a wrap. */
    if (reservations->count && reservations->end[reservations->count - 1] == position)
        reservations->end[reservations->count - 1] = position + bytes;
    else {
        if (reservations->count == 2) return -1;
        unsigned i = reservations->count++;
        reservations->begin[i] = position;
        reservations->end[i] = position + bytes;
    }
    *address = position;
    *cursor = position + bytes;
    return 0;
}

static void retire_chunk_allocation(const gex_gpu_guest_io *io,
                                    const map_allocation *allocations, size_t count,
                                    size_t index, size_t *batch_end, uint32_t *cursor) {
    if (index == *batch_end) {
        size_t end = index + 1;
        /* A wrapped callback is recognized only while next remains below cursor. */
        int wrapped = allocations[index].end < *cursor;
        while (end < count && allocations[end].begin == allocations[end - 1].end &&
               (!wrapped || allocations[end].end < *cursor))
            ++end;
        io->allocated(io->context, allocations[end - 1].end);
        *batch_end = end;
    }
    *cursor = allocations[index].end;
}

int gex_dreamcast_backdrop_tiles_direct(const gex_gpu_guest_io *io, uint32_t location,
                                        uint16_t palette, const uint32_t roots[2]) {
    uint32_t tails[2];
    uint8_t uv[3], fade[3];
    if (!io || !io->read || !location || location > UINT32_MAX - 15 || !roots ||
        read_bytes(io, location + 12, uv, sizeof uv) ||
        read_bytes(io, 0x4a2af8, fade, sizeof fade) ||
        read32(io, 0x4a2b18, &tails[0]) || read32(io, 0x4a2b14, &tails[1]))
        return 0;
    gex_dreamcast_tile_draw tiles[40] = {0};
    for (unsigned i = 0; i < 40; ++i) {
        gex_dreamcast_tile_draw *tile = &tiles[i];
        tile->x[0] = (uint16_t)((i / 8) * 64);
        tile->y[0] = (uint16_t)((i % 8) * 32);
        tile->u[0] = (uint8_t)(uv[0] << 2);
        tile->v[0] = uv[2];
        tile->width = 64;
        tile->height = 32;
        tile->clut = palette;
        tile->rgb = UINT32_C(0x64000000) | (uint32_t)fade[0] << 16 |
                    (uint32_t)fade[1] << 8 | fade[2];
    }
    return gex_dreamcast_title_record_cel_tiles(tiles, 40, roots, tails);
}

int gex_dreamcast_map_chunk_direct(const gex_gpu_guest_io *io, uint32_t chunk,
                                   uint32_t definitions, int32_t x, int32_t y,
                                   int width, int height) {
    if (!io || !io->read || width != 320 || (height != 240 && height != 256) ||
        x < -0x40000000 || x > 0x40000000 ||
        y < -0x40000000 || y > 0x40000000) return 0;
    uint32_t table, pages, tails[4];
    uint16_t screen_x, screen_y, last_page[2] = {0xffff, 0xffff};
    uint8_t fade[3];
    if (read32(io, 0x4a2ae8, &table) || read32(io, 0x460e08, &pages) ||
        read16(io, 0x4a2a96, &screen_x) || read16(io, 0x4a2a94, &screen_y) ||
        read_bytes(io, 0x4a2af8, fade, 3) ||
        read32(io, 0x46bc74, &tails[0]) || read32(io, 0x46bc70, &tails[1]) ||
        read32(io, 0x46bc6c, &tails[2]) || read32(io, 0x46bc68, &tails[3]) ||
        !pages || (int16_t)screen_x < -256 || (int16_t)screen_x > 256 ||
        (int16_t)screen_y < -256 || (int16_t)screen_y > 256) return 0;
    gex_map_tile tiles[GEX_MAP_TILE_LIMIT];
    gex_dreamcast_tile_draw draws[GEX_MAP_TILE_LIMIT];
    size_t count;
    if (gex_dreamcast_map_plan(io->read, io->context, chunk, table, definitions,
                                x, y, (int16_t)screen_x, (int16_t)screen_y,
                                width, height, tiles, &count)) return 0;
    for (size_t i = 0; i < count; ++i) {
        const gex_map_tile *tile = &tiles[i];
        uint8_t header[10], image_flags, palette_header[4];
        uint16_t page_index, page, uv;
        uint32_t tint, palette = 0;
        if (read_bytes(io, tile->image + 20, header, sizeof header) ||
            read_bytes(io, tile->image + 16, &image_flags, 1) ||
            (image_flags & 3) == 3 || read16(io, tile->image + 18, &page_index) ||
            read32(io, tile->descriptor + 8, &tint)) return 0;
        if ((image_flags & 3) != 2) {
            if (!io->prepare_palette || read32(io, tile->descriptor + 4, &palette) ||
                palette < 4 || read_bytes(io, palette - 4, palette_header, sizeof palette_header))
                return 0;
            int16_t slot = (int16_t)word(palette_header + 2);
            if (slot < -1 || slot >= (palette_header[0] ? 64 : 384)) return 0;
        }
        int64_t address = (int64_t)pages + (int16_t)page_index * 8;
        if (address <= 0 || address > UINT32_MAX - 3 ||
            read16(io, (uint32_t)address, &page) ||
            read16(io, (uint32_t)address + 2, &uv)) return 0;
        gex_dreamcast_tile_draw *draw = &draws[i];
        memset(draw, 0, sizeof *draw);
        draw->layer = !!(tile->spec & 0x2000);
        draw->is_quad = !!(tile->spec & 0xc000);
        draw->page_change = !draw->is_quad && last_page[draw->layer] != page;
        draw->page = draw->is_quad ? page : page & 0x1ff;
        draw->palette_address = palette;
        last_page[draw->layer] = page;
        draw->rgb = (uint32_t)fade[2] | (uint32_t)fade[1] << 8 |
                    (uint32_t)fade[0] << 16 | (tint & 0x8080u ? 0x66000000u : 0x64000000u);
        draw->width = header[2];
        draw->height = header[3];
        draw->wrap_x = screen_x;
        draw->wrap_y = screen_y;
        int offset_x = (int16_t)word(header + 4), offset_y = (int16_t)word(header + 6);
        int flip_x = !!(tile->spec & 0x4000), flip_y = !!(tile->spec & 0x8000);
        int x0 = (tile->x >> 16) + (flip_x ? 32 - offset_x : offset_x);
        int y0 = (tile->y >> 16) + (flip_y ? 32 - offset_y : offset_y);
        int x1 = x0 + (flip_x ? -(int)draw->width : draw->width);
        int y1 = y0 + (flip_y ? -(int)draw->height : draw->height);
        uint8_t u = (uint8_t)uv, v = (uint8_t)(uv >> 8);
        for (unsigned vertex = 0; vertex < 4; ++vertex) {
            draw->x[vertex] = (uint16_t)((vertex & 1) ? x1 : x0);
            draw->y[vertex] = (uint16_t)((vertex & 2) ? y1 : y0);
            draw->u[vertex] = (uint8_t)(u + ((vertex & 1) ? draw->width - 1 : 0));
            draw->v[vertex] = (uint8_t)(v + ((vertex & 2) ? draw->height - 1 : 0));
        }
    }
    return gex_dreamcast_title_record_tiles(io, draws, count, tails);
}

int gex_dreamcast_map_chunk(const gex_gpu_guest_io *io, uint32_t chunk,
                            uint32_t definitions, int32_t x, int32_t y,
                            int width, int height) {
    if (!io || !io->read || !io->write || !io->allocated ||
        width != 320 || height != 240 ||
        x < -0x40000000 || x > 0x40000000 ||
        y < -0x40000000 || y > 0x40000000) return 0;
    uint32_t table, pages, begin, end, cursor, tails[4];
    uint16_t screen_x, screen_y, last_page[2] = {0xffff, 0xffff};
    uint8_t fade[3];
    if (read32(io, 0x4a2ae8, &table) || read32(io, 0x460e08, &pages) ||
        read32(io, 0x4a2ae0, &begin) || read32(io, 0x4a2adc, &end) ||
        read32(io, 0x4a2ae4, &cursor) || read16(io, 0x4a2a96, &screen_x) ||
        read16(io, 0x4a2a94, &screen_y) || read_bytes(io, 0x4a2af8, fade, 3) ||
        read32(io, 0x46bc74, &tails[0]) || read32(io, 0x46bc70, &tails[1]) ||
        read32(io, 0x46bc6c, &tails[2]) || read32(io, 0x46bc68, &tails[3]))
        return 0;
    if (!pages || (int16_t)screen_x < -256 || (int16_t)screen_x > 256 ||
        (int16_t)screen_y < -256 || (int16_t)screen_y > 256 ||
        !begin || (begin & 3) || (cursor & 3) || end <= begin ||
        cursor < begin || cursor > end || end - begin < 64 ||
        !tails[0] || !tails[1] || !tails[2] || !tails[3]) return 0;

    gex_map_tile tiles[GEX_MAP_TILE_LIMIT];
    map_command commands[GEX_MAP_TILE_LIMIT];
    map_reservations reservations = {0};
    map_allocation allocations[GEX_MAP_TILE_LIMIT * 2];
    size_t allocation_count = 0;
    uint32_t committed_cursor = cursor;
    size_t count;
    if (gex_dreamcast_map_plan(io->read, io->context, chunk, table, definitions,
                                x, y, (int16_t)screen_x, (int16_t)screen_y,
                                width, height, tiles, &count)) return 0;
    for (size_t i = 0; i < count; ++i) {
        const gex_map_tile *tile = &tiles[i];
        uint8_t header[10], image_flags;
        uint16_t page_index;
        uint32_t tint;
        if (read_bytes(io, tile->image + 20, header, sizeof header) ||
            read_bytes(io, tile->image + 16, &image_flags, 1) ||
            (image_flags & 3) != 2 ||
            read16(io, tile->image + 18, &page_index) ||
            read32(io, tile->descriptor + 8, &tint)) return 0;
        int64_t page_address = (int64_t)pages + (int16_t)page_index * 8;
        if (page_address <= 0 || page_address > UINT32_MAX - 3) return 0;
        map_command *cmd = &commands[i];
        if (read16(io, (uint32_t)page_address, &cmd->page) ||
            read16(io, (uint32_t)page_address + 2, &cmd->uv)) return 0;
        cmd->layer = !!(tile->spec & 0x2000);
        cmd->rgb = (uint32_t)fade[2] | (uint32_t)fade[1] << 8 |
                   (uint32_t)fade[0] << 16 | (tint & 0x8080u ? 0x66000000u : 0x64000000u);
        cmd->x = (uint16_t)((tile->x >> 16) + (int16_t)(header[4] | (uint16_t)header[5] << 8));
        cmd->y = (uint16_t)((tile->y >> 16) + (int16_t)(header[6] | (uint16_t)header[7] << 8));
        cmd->width = header[2];
        cmd->height = header[3];
        cmd->flipped = !!(tile->spec & 0xc000u);
        cmd->bytes = cmd->flipped ? 80 : 40;
        cmd->page_packet = 0;
        if (end - begin < cmd->bytes ||
            reserve(begin, end, &cursor, cmd->bytes, tails, &reservations, &cmd->sprite) ||
            read32(io, cmd->sprite, &cmd->link)) return 0;
        allocations[allocation_count++] =
            (map_allocation){cmd->sprite, cmd->sprite + cmd->bytes};
        if (cmd->flipped) {
            if (read_bytes(io, cmd->sprite, cmd->quad, sizeof cmd->quad)) return 0;
            int offset_x = (int16_t)word(header + 4), offset_y = (int16_t)word(header + 6);
            int flip_x = !!(tile->spec & 0x4000u), flip_y = !!(tile->spec & 0x8000u);
            int x0 = (tile->x >> 16) + (flip_x ? 32 - offset_x : offset_x);
            int y0 = (tile->y >> 16) + (flip_y ? 32 - offset_y : offset_y);
            int x1 = x0 + (flip_x ? -(int)cmd->width : cmd->width);
            int y1 = y0 + (flip_y ? -(int)cmd->height : cmd->height);
            uint8_t u = (uint8_t)cmd->uv, v = (uint8_t)(cmd->uv >> 8);
            put32(cmd->quad + 4, (cmd->rgb & 0xffffffu) |
                  (tint & 0x8080u ? 0x2e000000u : 0x2c000000u));
            put16(cmd->quad + 8, (uint16_t)x0); put16(cmd->quad + 10, (uint16_t)y0);
            put16(cmd->quad + 16, (uint16_t)x1); put16(cmd->quad + 18, (uint16_t)y0);
            put16(cmd->quad + 24, (uint16_t)x0); put16(cmd->quad + 26, (uint16_t)y1);
            put16(cmd->quad + 32, (uint16_t)x1); put16(cmd->quad + 34, (uint16_t)y1);
            cmd->quad[12] = u; cmd->quad[13] = v;
            cmd->quad[20] = (uint8_t)(u + cmd->width - 1); cmd->quad[21] = v;
            cmd->quad[28] = u; cmd->quad[29] = (uint8_t)(v + cmd->height - 1);
            cmd->quad[36] = cmd->quad[20]; cmd->quad[37] = cmd->quad[29];
            put16(cmd->quad + 22, cmd->page);
            last_page[cmd->layer] = cmd->page;
        } else if (last_page[cmd->layer] != cmd->page) {
            if (reserve(begin, end, &cursor, 24, tails, &reservations, &cmd->page_packet) ||
                read32(io, cmd->page_packet + 8, &cmd->page_padding))
                return 0;
            if (cmd->page_packet < cmd->sprite + 40 && cmd->sprite < cmd->page_packet + 24)
                return 0;
            allocations[allocation_count++] =
                (map_allocation){cmd->page_packet, cmd->page_packet + 24};
            last_page[cmd->layer] = cmd->page;
        }
    }
    size_t allocation_index = 0, batch_end = 0;
    for (size_t i = 0; i < count; ++i) {
        map_command *cmd = &commands[i];
        unsigned list = cmd->layer * 2;
        if (cmd->flipped) {
            retire_chunk_allocation(io, allocations, allocation_count, allocation_index++,
                                    &batch_end, &committed_cursor);
            write32(io, 0x4a2ae4, cmd->sprite + 80);
            io->write(io->context, cmd->sprite + 4, cmd->quad + 4, 36);
            io->write(io->context, cmd->sprite + 40, cmd->quad, 40);
            for (unsigned vertex = 0; vertex < 4; ++vertex) {
                unsigned offset = 8 + vertex * 8;
                write16(io, cmd->sprite + 40 + offset,
                        word(cmd->quad + offset) + screen_x);
                write16(io, cmd->sprite + 42 + offset,
                        word(cmd->quad + offset + 2) + screen_y);
            }
            for (unsigned copy = 0; copy < 2; ++copy) {
                uint32_t node = cmd->sprite + copy * 40;
                write32(io, tails[list + copy], node);
                tails[list + copy] = node;
                write32(io, (cmd->layer ? 0x46bc6c : 0x46bc74) - copy * 4, node);
            }
            continue;
        }
        uint8_t sprite[20] = {0};
        put32(sprite, cmd->link);
        put32(sprite + 4, cmd->rgb);
        put16(sprite + 8, cmd->x);
        put16(sprite + 10, cmd->y);
        put16(sprite + 12, cmd->uv);
        put16(sprite + 16, cmd->width);
        put16(sprite + 18, cmd->height);
        retire_chunk_allocation(io, allocations, allocation_count, allocation_index++,
                                &batch_end, &committed_cursor);
        write32(io, 0x4a2ae4, cmd->sprite + 40);
        io->write(io->context, cmd->sprite + 4, sprite + 4, 16);
        io->write(io->context, cmd->sprite + 20, sprite, sizeof sprite);
        put16(sprite + 8, cmd->x + screen_x);
        put16(sprite + 10, cmd->y + screen_y);
        io->write(io->context, cmd->sprite + 28, sprite + 8, 4);
        if (cmd->page_packet) {
            uint32_t page_word = 0xe1000200u + (cmd->page & 0x1ffu);
            retire_chunk_allocation(io, allocations, allocation_count, allocation_index++,
                                    &batch_end, &committed_cursor);
            write32(io, 0x4a2ae4, cmd->page_packet + 24);
            write32(io, cmd->page_packet + 20, cmd->page_padding);
            for (unsigned copy = 0; copy < 2; ++copy) {
                uint32_t page = cmd->page_packet + copy * 12;
                write32(io, page, cmd->sprite + copy * 20);
                write32(io, page + 4, page_word);
                write32(io, tails[list + copy], page);
            }
        } else {
            for (unsigned copy = 0; copy < 2; ++copy)
                write32(io, tails[list + copy], cmd->sprite + copy * 20);
        }
        for (unsigned copy = 0; copy < 2; ++copy) {
            tails[list + copy] = cmd->sprite + copy * 20;
            write32(io, (cmd->layer ? 0x46bc6c : 0x46bc74) - copy * 4,
                    tails[list + copy]);
        }
    }
    return 1;
}

typedef struct {
    uint32_t address;
    uint8_t packet[40];
} flipped_command;

static void flipped_quad(const gex_gpu_cel_draw *draw, unsigned format,
                         const uint8_t record[8], const uint8_t location[4],
                         gex_dreamcast_tile_draw *quad) {
    memset(quad, 0, sizeof *quad);
    uint16_t page = word(location);
    if (!(draw->tint & 1)) page |= 0x20;
    unsigned u = location[2], v = location[3];
    unsigned uv_width = record[2], uv_height = record[3];
    int offset_x = (int16_t)word(record + 4), offset_y = (int16_t)word(record + 6);
    int flip_x = !!(draw->flags & 0x80000000u);
    int flip_y = !!(draw->flags & 0x40000000u);
    uint16_t x0 = (uint16_t)(draw->x + (flip_x ? -offset_x : offset_x));
    uint16_t y0 = (uint16_t)(draw->y + (flip_y ? -offset_y : offset_y));
    uint16_t x1 = (uint16_t)(x0 + (flip_x ? -(int)record[2] : record[2]));
    uint16_t y1 = (uint16_t)(y0 + (flip_y ? -(int)record[3] : record[3]));
    if (flip_x) {
        if (u) --u;
        else if (format == 1) { --page; u = 127; }
        else --uv_width;
    } else if (u + uv_width == 256) {
        --uv_width;
    }
    if (flip_y) {
        if (v) --v;
        else --uv_height;
    } else if (v + uv_height == 256) {
        --uv_height;
    }
    quad->page = page;
    quad->clut = draw->palette;
    quad->wrap_x = (uint16_t)-draw->wrap_x;
    quad->wrap_y = (uint16_t)-draw->wrap_y;
    quad->is_quad = 1;
    quad->rgb = (draw->rgb & 0xffffffu) |
                (draw->tint & 0x8080u ? 0x2e000000u : 0x2c000000u);
    for (unsigned vertex = 0; vertex < 4; ++vertex) {
        quad->x[vertex] = (vertex & 1) ? x1 : x0;
        quad->y[vertex] = (vertex & 2) ? y1 : y0;
        quad->u[vertex] = (uint8_t)(u + ((vertex & 1) ? uv_width : 0));
        quad->v[vertex] = (uint8_t)(v + ((vertex & 2) ? uv_height : 0));
    }
}

int gex_dreamcast_map_cel_direct(const gex_gpu_guest_io *io,
                                 const gex_gpu_cel_draw *draw, const uint32_t roots[2]) {
    enum { LIMIT = 128 };
    if (!io || !io->read || !io->write || !draw || !roots ||
        !draw->image || !draw->atlas ||
        draw->image > UINT32_MAX - 20 - (LIMIT + 1) * 8) return 0;
    uint32_t tails[2];
    uint16_t last_page;
    uint8_t image_flags;
    if (read32(io, 0x4a2b18, &tails[0]) || read32(io, 0x4a2b14, &tails[1]) ||
        read16(io, 0x4a2b20, &last_page) ||
        read_bytes(io, draw->image + 16, &image_flags, 1) || (image_flags & 3) == 3)
        return 0;
    int flipped = !!(draw->flags & 0xc0000000u);
    gex_dreamcast_tile_draw quads[LIMIT];
    uint32_t atlas = draw->atlas;
    size_t count = 0;
    for (; count <= LIMIT; ++count) {
        uint8_t record[8], location[4];
        if (read_bytes(io, draw->image + 20 + (uint32_t)count * 8, record, sizeof record))
            return 0;
        if (!(record[0] | record[1])) break;
        if (count == LIMIT || !record[2] || !record[3] || !atlas) return 0;
        uint32_t next;
        if (image_flags & 0x40) {
            if (atlas > UINT32_MAX - 8 || read_bytes(io, atlas, location, sizeof location))
                return 0;
            next = atlas + 8;
        } else {
            if (atlas > UINT32_MAX - 20 || read_bytes(io, atlas + 16, location, sizeof location) ||
                read32(io, atlas + 4, &next))
                return 0;
        }
        if (flipped) {
            flipped_quad(draw, image_flags & 3, record, location, &quads[count]);
            last_page = quads[count].page;
        } else {
            uint16_t page = word(location);
            if (!(draw->tint & 1)) page |= 0x20;
            gex_dreamcast_tile_draw *tile = &quads[count];
            memset(tile, 0, sizeof *tile);
            tile->x[0] = (uint16_t)(draw->x + (int16_t)word(record + 4));
            tile->y[0] = (uint16_t)(draw->y + (int16_t)word(record + 6));
            tile->u[0] = location[2]; tile->v[0] = location[3];
            tile->width = record[2]; tile->height = record[3];
            tile->wrap_x = (uint16_t)-draw->wrap_x;
            tile->wrap_y = (uint16_t)-draw->wrap_y;
            tile->page_change = page != last_page;
            tile->page = page & 0x1ff;
            tile->clut = draw->palette;
            tile->rgb = (draw->rgb & 0xffffffu) |
                        (draw->tint & 0x8080u ? 0x66000000u : 0x64000000u);
            last_page = page;
        }
        atlas = next;
    }
    if (!count) return 0;
    int result = gex_dreamcast_title_record_cel_tiles(quads, count, roots, tails);
    if (result <= 0) return result;
    write16(io, 0x4a2b20, last_page);
    return 1;
}

int gex_dreamcast_map_flipped_cel(const gex_gpu_guest_io *io,
                                  const gex_gpu_cel_draw *draw, uint32_t *previous_tail) {
    enum { LIMIT = 128, PAIR_SIZE = 80 };
    if (!io || !io->read || !io->write || !io->allocated || !draw || !previous_tail ||
        !(draw->flags & 0xc0000000u) || !draw->image || !draw->atlas ||
        draw->image > UINT32_MAX - 20 - (LIMIT + 1) * 8)
        return 0;
    uint32_t begin, end, cursor, tails[2];
    uint8_t image_flags;
    if (read32(io, 0x4a2ae0, &begin) || read32(io, 0x4a2adc, &end) ||
        read32(io, 0x4a2ae4, &cursor) || read32(io, 0x4a2b18, &tails[0]) ||
        read32(io, 0x4a2b14, &tails[1]) ||
        read_bytes(io, draw->image + 16, &image_flags, 1))
        return 0;
    unsigned format = image_flags & 3;
    if (format == 3 || !begin || (begin & 3) || (cursor & 3) ||
        end <= begin || cursor < begin || cursor > end || end - begin < PAIR_SIZE ||
        !tails[0] || !tails[1])
        return 0;

    flipped_command commands[LIMIT];
    uint32_t atlas = draw->atlas;
    size_t count = 0;
    for (; count <= LIMIT; ++count) {
        uint8_t record[8], location[4];
        if (read_bytes(io, draw->image + 20 + (uint32_t)count * 8, record, sizeof record))
            return 0;
        if (!(record[0] | record[1])) break;
        if (count == LIMIT || !record[2] || !record[3]) return 0;
        uint32_t next;
        if (image_flags & 0x40) {
            if (atlas > UINT32_MAX - 8 || read_bytes(io, atlas, location, sizeof location))
                return 0;
            next = atlas + 8;
        } else {
            if (atlas > UINT32_MAX - 20 || read_bytes(io, atlas + 16, location, sizeof location) ||
                read32(io, atlas + 4, &next))
                return 0;
        }
        gex_dreamcast_tile_draw quad;
        flipped_quad(draw, format, record, location, &quad);
        if (end - cursor < PAIR_SIZE) cursor = begin;
        for (unsigned i = 0; i < 2; ++i)
            if (tails[i] >= cursor && tails[i] < cursor + PAIR_SIZE) return 0;
        for (size_t i = 0; i < count; ++i)
            if (cursor < commands[i].address + PAIR_SIZE &&
                commands[i].address < cursor + PAIR_SIZE) return 0;
        flipped_command *cmd = &commands[count];
        cmd->address = cursor;
        if (read_bytes(io, cursor, cmd->packet, sizeof cmd->packet)) return 0;
        put32(cmd->packet + 4, quad.rgb);
        for (unsigned vertex = 0; vertex < 4; ++vertex) {
            unsigned at = 8 + vertex * 8;
            put16(cmd->packet + at, quad.x[vertex]);
            put16(cmd->packet + at + 2, quad.y[vertex]);
            cmd->packet[at + 4] = quad.u[vertex];
            cmd->packet[at + 5] = quad.v[vertex];
        }
        put16(cmd->packet + 14, quad.clut);
        put16(cmd->packet + 22, quad.page);
        cursor += PAIR_SIZE;
        atlas = next;
        if (!atlas && count + 1 < LIMIT) {
            uint16_t following;
            if (read16(io, draw->image + 28 + (uint32_t)count * 8, &following) || following)
                return 0;
        }
    }

    if (!count) return 0;
    for (size_t i = 0; i < count; ++i) {
        flipped_command *cmd = &commands[i];
        io->allocated(io->context, cmd->address + PAIR_SIZE);
        write32(io, 0x4a2ae4, cmd->address + PAIR_SIZE);
        io->write(io->context, cmd->address + 4, cmd->packet + 4, 26);
        io->write(io->context, cmd->address + 32, cmd->packet + 32, 6);
        for (unsigned vertex = 0; vertex < 4; ++vertex) {
            unsigned at = 8 + vertex * 8;
            uint16_t vx = cmd->packet[at] | (uint16_t)cmd->packet[at + 1] << 8;
            uint16_t vy = cmd->packet[at + 2] | (uint16_t)cmd->packet[at + 3] << 8;
            put16(cmd->packet + at, (uint16_t)(vx - draw->wrap_x));
            put16(cmd->packet + at + 2, (uint16_t)(vy - draw->wrap_y));
        }
        io->write(io->context, cmd->address + 40, cmd->packet, sizeof cmd->packet);
        *previous_tail = tails[1];
        for (unsigned copy = 0; copy < 2; ++copy) {
            write32(io, tails[copy], cmd->address + copy * 40);
            tails[copy] = cmd->address + copy * 40;
            write32(io, copy ? 0x4a2b14 : 0x4a2b18, tails[copy]);
        }
        write16(io, 0x4a2b20, (uint16_t)(cmd->packet[22] |
                                           (uint16_t)cmd->packet[23] << 8));
    }
    return 1;
}

int gex_dreamcast_map_unflipped_cel(const gex_gpu_guest_io *io,
                                    const gex_gpu_cel_draw *draw, gex_gpu_cel_result *result) {
    enum { LIMIT = 128 };
    if (!io || !io->read || !io->write || !io->allocated || !draw || !result ||
        (draw->flags & 0xc0000000u) || !draw->image || !draw->atlas ||
        draw->image > UINT32_MAX - 20 - (LIMIT + 1) * 8) return 0;
    uint32_t begin, end, cursor, tails[4] = {0, 0, 0, 0};
    uint16_t last_page;
    uint8_t flags;
    if (read32(io, 0x4a2ae0, &begin) || read32(io, 0x4a2adc, &end) ||
        read32(io, 0x4a2ae4, &cursor) || read32(io, 0x4a2b18, &tails[0]) ||
        read32(io, 0x4a2b14, &tails[1]) || read16(io, 0x4a2b20, &last_page) ||
        read_bytes(io, draw->image + 16, &flags, 1)) return 0;
    if ((flags & 3) == 3 || !begin || (begin & 3) ||
        (cursor & 3) || end <= begin || cursor < begin || cursor > end ||
        end - begin < 40 || !tails[0] || !tails[1]) return 0;

    map_command commands[LIMIT];
    map_reservations reservations = {0};
    uint8_t sprites[LIMIT][20], pages[LIMIT][12];
    uint32_t atlas = draw->atlas;
    size_t count = 0;
    for (; count <= LIMIT; ++count) {
        uint8_t record[8], location[4];
        if (read_bytes(io, draw->image + 20 + (uint32_t)count * 8, record, sizeof record))
            return 0;
        if (!(record[0] | record[1])) break;
        if (count == LIMIT || !record[2] || !record[3] || !atlas) return 0;
        uint32_t next;
        if (flags & 0x40) {
            if (atlas > UINT32_MAX - 8 || read_bytes(io, atlas, location, sizeof location))
                return 0;
            next = atlas + 8;
        } else {
            if (atlas > UINT32_MAX - 20 || read_bytes(io, atlas + 16, location, sizeof location) ||
                read32(io, atlas + 4, &next))
                return 0;
        }
        map_command *cmd = &commands[count];
        cmd->page = (uint16_t)(location[0] | (uint16_t)location[1] << 8);
        if (!(draw->tint & 1)) cmd->page |= 0x20;
        cmd->page_packet = 0;
        cmd->bytes = 40;
        if (reserve(begin, end, &cursor, 40, tails, &reservations, &cmd->sprite) ||
            read_bytes(io, cmd->sprite, sprites[count], 20)) return 0;
        if (last_page != cmd->page) {
            if (reserve(begin, end, &cursor, 24, tails, &reservations, &cmd->page_packet) ||
                (cmd->page_packet < cmd->sprite + 40 &&
                 cmd->sprite < cmd->page_packet + 24) ||
                read_bytes(io, cmd->page_packet, pages[count], 12)) return 0;
            last_page = cmd->page;
        }
        put32(sprites[count] + 4, (draw->rgb & 0xffffffu) |
              (draw->tint & 0x8080u ? 0x66000000u : 0x64000000u));
        put16(sprites[count] + 8, (uint16_t)(draw->x + (int16_t)word(record + 4)));
        put16(sprites[count] + 10, (uint16_t)(draw->y + (int16_t)word(record + 6)));
        put16(sprites[count] + 12, (uint16_t)(location[2] | (uint16_t)location[3] << 8));
        put16(sprites[count] + 14, draw->palette);
        put16(sprites[count] + 16, record[2]);
        put16(sprites[count] + 18, record[3]);
        atlas = next;
    }
    if (!count) return 0;
    for (size_t i = 0; i < count; ++i) {
        map_command *cmd = &commands[i];
        uint8_t *sprite = sprites[i];
        io->allocated(io->context, cmd->sprite + 40);
        write32(io, 0x4a2ae4, cmd->sprite + 40);
        io->write(io->context, cmd->sprite + 4, sprite + 4, 16);
        put16(sprite + 8, (uint16_t)(word(sprite + 8) - draw->wrap_x));
        put16(sprite + 10, (uint16_t)(word(sprite + 10) - draw->wrap_y));
        io->write(io->context, cmd->sprite + 20, sprite, 20);
        if (cmd->page_packet) {
            uint8_t *page = pages[i];
            put32(page, cmd->sprite);
            put32(page + 4, 0xe1000200u + (cmd->page & 0x1ffu));
            io->allocated(io->context, cmd->page_packet + 24);
            write32(io, 0x4a2ae4, cmd->page_packet + 24);
            io->write(io->context, cmd->page_packet, page, 12);
            put32(page, cmd->sprite + 20);
            io->write(io->context, cmd->page_packet + 12, page, 12);
            write32(io, tails[0], cmd->page_packet);
            write32(io, tails[1], cmd->page_packet + 12);
            result->eax = cmd->sprite + 20;
            result->ecx = cmd->page_packet + 12;
            result->edx = tails[1];
            write16(io, 0x4a2b20, cmd->page);
        } else {
            write32(io, tails[0], cmd->sprite);
            write32(io, tails[1], cmd->sprite + 20);
            result->eax = tails[0];
            result->ecx = tails[1];
            result->edx = cmd->sprite + 20;
        }
        tails[0] = cmd->sprite;
        tails[1] = cmd->sprite + 20;
        write32(io, 0x4a2b18, tails[0]);
        write32(io, 0x4a2b14, tails[1]);
    }
    return 1;
}

static int scaled_coordinate(int offset, int32_t span, int32_t divisor,
                             uint16_t origin, uint16_t *value) {
    int32_t product = (int32_t)((uint32_t)offset * (uint32_t)span);
    if (!divisor || (product == INT32_MIN && divisor == -1)) return -1;
    *value = (uint16_t)(origin + (uint32_t)(product / divisor));
    return 0;
}

static int scaled_quad(const gex_gpu_scaled_cel_draw *draw, const uint8_t record[8],
                       const uint8_t location[4], gex_dreamcast_tile_draw *quad) {
    uint16_t x0, x1, y0, y1;
    int ox = (int16_t)word(record + 4), oy = (int16_t)word(record + 6);
    if (scaled_coordinate(ox, draw->span_x, draw->divisor_x, draw->x, &x0) ||
        scaled_coordinate(ox + record[2], draw->span_x, draw->divisor_x, draw->x, &x1) ||
        scaled_coordinate(oy, draw->span_y, draw->divisor_y, draw->y, &y0) ||
        scaled_coordinate(oy + record[3], draw->span_y, draw->divisor_y, draw->y, &y1))
        return -1;
    memset(quad, 0, sizeof *quad);
    quad->page = word(location);
    if (!(draw->tint & 1)) quad->page |= 0x20;
    quad->clut = draw->palette;
    quad->wrap_x = (uint16_t)-draw->wrap_x;
    quad->wrap_y = (uint16_t)-draw->wrap_y;
    quad->is_quad = 1;
    quad->rgb = draw->rgb;
    for (unsigned vertex = 0; vertex < 4; ++vertex) {
        quad->x[vertex] = (vertex & 1) ? x1 : x0;
        quad->y[vertex] = (vertex & 2) ? y1 : y0;
        quad->u[vertex] = (uint8_t)(location[2] + ((vertex & 1) ? record[2] - 1 : 0));
        quad->v[vertex] = (uint8_t)(location[3] + ((vertex & 2) ? record[3] - 1 : 0));
    }
    return 0;
}

int gex_dreamcast_map_scaled_cel_direct(const gex_gpu_guest_io *io,
                                        const gex_gpu_scaled_cel_draw *draw,
                                        const uint32_t roots[2], uint32_t *last_atlas) {
    enum { LIMIT = 128 };
    if (!io || !io->read || !io->write || !draw || !roots || !last_atlas ||
        ((draw->rgb >> 24) & ~2u) != 0x2c || !draw->image ||
        draw->image > UINT32_MAX - 16 || !draw->tiles || !draw->atlas ||
        draw->tiles > UINT32_MAX - (LIMIT + 1) * 8) return 0;
    uint32_t tails[2];
    uint8_t flags;
    if (read32(io, 0x4a2b18, &tails[0]) || read32(io, 0x4a2b14, &tails[1]) ||
        read_bytes(io, draw->image + 16, &flags, 1) || (flags & 3) == 3) return 0;
    gex_dreamcast_tile_draw quads[LIMIT];
    uint32_t atlas = draw->atlas, final_atlas = 0;
    size_t count = 0;
    for (; count <= LIMIT; ++count) {
        uint8_t record[8], location[4];
        if (read_bytes(io, draw->tiles + (uint32_t)count * 8, record, sizeof record)) return 0;
        if (!(record[0] | record[1])) break;
        if (count == LIMIT || !record[2] || !record[3] || !atlas) return 0;
        uint32_t next;
        if (flags & 0x40) {
            if (atlas > UINT32_MAX - 8 || read_bytes(io, atlas, location, sizeof location))
                return 0;
            next = final_atlas = atlas + 8;
        } else {
            if (atlas > UINT32_MAX - 20 || read_bytes(io, atlas + 16, location, sizeof location) ||
                read32(io, atlas + 4, &next))
                return 0;
            final_atlas = atlas;
        }
        if (scaled_quad(draw, record, location, &quads[count])) return 0;
        atlas = next;
    }
    if (!count) return 0;
    int result = gex_dreamcast_title_record_cel_tiles(quads, count, roots, tails);
    if (result <= 0) return result;
    write16(io, 0x4a2b20, quads[count - 1].page);
    *last_atlas = final_atlas;
    return 1;
}

int gex_dreamcast_map_scaled_cel(const gex_gpu_guest_io *io,
                                 const gex_gpu_scaled_cel_draw *draw, uint32_t *last_atlas) {
    enum { LIMIT = 128, PAIR_SIZE = 80 };
    if (!io || !io->read || !io->write || !io->allocated || !draw || !last_atlas ||
        !draw->image || draw->image > UINT32_MAX - 16 || !draw->tiles || !draw->atlas ||
        draw->tiles > UINT32_MAX - (LIMIT + 1) * 8) return 0;
    uint32_t begin, end, cursor, tails[2];
    uint8_t flags;
    if (read32(io, 0x4a2ae0, &begin) || read32(io, 0x4a2adc, &end) ||
        read32(io, 0x4a2ae4, &cursor) || read32(io, 0x4a2b18, &tails[0]) ||
        read32(io, 0x4a2b14, &tails[1]) ||
        read_bytes(io, draw->image + 16, &flags, 1)) return 0;
    if ((flags & 3) == 3 || !begin || (begin & 3) || (cursor & 3) ||
        end <= begin || cursor < begin || cursor > end || end - begin < PAIR_SIZE ||
        !tails[0] || !tails[1]) return 0;

    flipped_command commands[LIMIT];
    uint32_t atlas = draw->atlas, final_atlas = 0;
    size_t count = 0;
    for (; count <= LIMIT; ++count) {
        uint8_t record[8], location[4];
        if (read_bytes(io, draw->tiles + (uint32_t)count * 8, record, sizeof record))
            return 0;
        if (!(record[0] | record[1])) break;
        if (count == LIMIT || !record[2] || !record[3] || !atlas) return 0;
        uint32_t next;
        if (flags & 0x40) {
            if (atlas > UINT32_MAX - 8 || read_bytes(io, atlas, location, sizeof location))
                return 0;
            next = atlas + 8;
            final_atlas = next;
        } else {
            if (atlas > UINT32_MAX - 20 || read_bytes(io, atlas + 16, location, sizeof location) ||
                read32(io, atlas + 4, &next))
                return 0;
            final_atlas = atlas;
        }
        gex_dreamcast_tile_draw quad;
        if (scaled_quad(draw, record, location, &quad)) return 0;
        if (end - cursor < PAIR_SIZE) cursor = begin;
        for (unsigned i = 0; i < 2; ++i)
            if (tails[i] >= cursor && tails[i] < cursor + PAIR_SIZE) return 0;
        for (size_t i = 0; i < count; ++i)
            if (cursor < commands[i].address + PAIR_SIZE &&
                commands[i].address < cursor + PAIR_SIZE) return 0;
        flipped_command *cmd = &commands[count];
        cmd->address = cursor;
        if (read_bytes(io, cursor, cmd->packet, sizeof cmd->packet)) return 0;
        put32(cmd->packet + 4, quad.rgb);
        for (unsigned vertex = 0; vertex < 4; ++vertex) {
            unsigned at = 8 + vertex * 8;
            put16(cmd->packet + at, quad.x[vertex]);
            put16(cmd->packet + at + 2, quad.y[vertex]);
            cmd->packet[at + 4] = quad.u[vertex];
            cmd->packet[at + 5] = quad.v[vertex];
        }
        put16(cmd->packet + 14, quad.clut);
        put16(cmd->packet + 22, quad.page);
        cursor += PAIR_SIZE;
        atlas = next;
    }
    if (!count) return 0;
    for (size_t i = 0; i < count; ++i) {
        flipped_command *cmd = &commands[i];
        io->allocated(io->context, cmd->address + PAIR_SIZE);
        write32(io, 0x4a2ae4, cmd->address + PAIR_SIZE);
        io->write(io->context, cmd->address + 4, cmd->packet + 4, 26);
        io->write(io->context, cmd->address + 32, cmd->packet + 32, 6);
        for (unsigned vertex = 0; vertex < 4; ++vertex) {
            unsigned at = 8 + vertex * 8;
            put16(cmd->packet + at, (uint16_t)(word(cmd->packet + at) - draw->wrap_x));
            put16(cmd->packet + at + 2, (uint16_t)(word(cmd->packet + at + 2) - draw->wrap_y));
        }
        io->write(io->context, cmd->address + 40, cmd->packet, sizeof cmd->packet);
        for (unsigned copy = 0; copy < 2; ++copy) {
            write32(io, tails[copy], cmd->address + copy * 40);
            tails[copy] = cmd->address + copy * 40;
            write32(io, copy ? 0x4a2b14 : 0x4a2b18, tails[copy]);
        }
        write16(io, 0x4a2b20, word(cmd->packet + 22));
    }
    *last_atlas = final_atlas;
    return 1;
}
