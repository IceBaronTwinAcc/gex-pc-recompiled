#include "title_cel.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static unsigned word(const uint8_t *p) {
    return p[0] | (unsigned)p[1] << 8;
}

int gex_title_cel_decompress(const uint8_t *source, size_t source_size,
                             uint8_t *output, size_t output_size) {
    if (!source || !output || source_size < 4 || (output_size & 3))
        return GEX_TITLE_CEL_INVALID;
    size_t data = (size_t)source[0] | (size_t)source[1] << 8 |
                  (size_t)source[2] << 16 | (size_t)source[3] << 24;
    if (data < 4 || data > source_size) return GEX_TITLE_CEL_INVALID;
    size_t control = 4, control_end = data, written = 0;
    while (written < output_size) {
        if (control >= control_end) return GEX_TITLE_CEL_INVALID;
        unsigned command = source[control++];
        size_t words = command & 0x7f;
        if (!words) words = 128;
        if (words > (output_size - written) / 4) return GEX_TITLE_CEL_INVALID;
        size_t bytes = words * 4;
        size_t read_size = command & 0x80 ? 4 : bytes;
        if (read_size > source_size - data) return GEX_TITLE_CEL_INVALID;
        if (command & 0x80) {
            for (size_t i = 0; i < bytes; i += 4)
                memcpy(output + written + i, source + data, 4);
        } else {
            memcpy(output + written, source + data, bytes);
        }
        data += read_size;
        written += bytes;
    }
    return GEX_TITLE_CEL_OK;
}

int gex_title_cel_bounds(const uint8_t *image, size_t size, gex_title_cel *out) {
    if (!image || !out || size < 28) return GEX_TITLE_CEL_INVALID;
    unsigned format = image[16] & 3;
    if (format == 3 || (image[16] & 0x44)) return GEX_TITLE_CEL_UNSUPPORTED;
    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
    unsigned count = 0;
    size_t at = 20;
    for (;;) {
        if (at > size || size - at < 8) return GEX_TITLE_CEL_INVALID;
        int source = (int16_t)word(image + at);
        if (!source) break;
        unsigned width = image[at + 2], height = image[at + 3];
        int x = (int16_t)word(image + at + 4), y = (int16_t)word(image + at + 6);
        unsigned padded = (width + 7) & ~7u;
        size_t pitch = format == 0 ? padded / 2 : format == 1 ? padded : padded * 2;
        if (source < 0 || !width || !height || (size_t)source > size ||
            pitch * height > size - (size_t)source) return GEX_TITLE_CEL_INVALID;
        if (left > x) left = x;
        if (top > y) top = y;
        if (right < x + (int)width) right = x + (int)width;
        if (bottom < y + (int)height) bottom = y + (int)height;
        if (++count > 512) return GEX_TITLE_CEL_UNSUPPORTED;
        at += 8;
    }
    if (!count) return GEX_TITLE_CEL_INVALID;
    if (right - left > 256 || bottom - top > 256) return GEX_TITLE_CEL_UNSUPPORTED;
    *out = (gex_title_cel){left, top, (unsigned)(right - left),
                           (unsigned)(bottom - top), format, count};
    return GEX_TITLE_CEL_OK;
}

int gex_title_cel_decode(const uint8_t *image, size_t size,
                         const gex_title_cel *cel, uint32_t *pixels, size_t count) {
    gex_title_cel actual;
    int result = gex_title_cel_bounds(image, size, &actual);
    if (result != GEX_TITLE_CEL_OK) return result;
    if (!cel || !pixels || cel->left != actual.left || cel->top != actual.top ||
        cel->width != actual.width || cel->height != actual.height ||
        cel->format != actual.format || cel->tiles != actual.tiles ||
        count < (size_t)actual.width * actual.height) return GEX_TITLE_CEL_INVALID;
    memset(pixels, 0, (size_t)actual.width * actual.height * sizeof *pixels);
    for (unsigned tile = 0; tile < actual.tiles; ++tile) {
        size_t at = 20 + 8 * tile;
        unsigned source = word(image + at), width = image[at + 2], height = image[at + 3];
        int x = (int16_t)word(image + at + 4) - actual.left;
        int y = (int16_t)word(image + at + 6) - actual.top;
        unsigned padded = (width + 7) & ~7u;
        size_t pitch = actual.format == 0 ? padded / 2 : actual.format == 1 ? padded : padded * 2;
        for (unsigned row = 0; row < height; ++row) {
            const uint8_t *src = image + source + pitch * row;
            uint32_t *dst = pixels + (y + row) * actual.width + x;
            for (unsigned col = 0; col < width; ++col) {
                if (dst[col]) return GEX_TITLE_CEL_OVERLAP;
                unsigned value = actual.format == 0 ? (src[col / 2] >> ((col & 1) * 4)) & 15 :
                                 actual.format == 1 ? src[col] : word(src + col * 2);
                dst[col] = 0x10000u | value;
            }
        }
    }
    return GEX_TITLE_CEL_OK;
}

static int capture_read(gex_title_cel_read read, void *context, uint32_t base,
                        size_t offset, void *out, size_t size) {
    if (offset > UINT32_MAX - base || size > UINT32_MAX - (base + offset))
        return -1;
    return read(context, base + (uint32_t)offset, out, size);
}

static int capture_compressed(gex_title_cel_read read, void *context, uint32_t image,
                              const uint8_t *records, unsigned tiles, unsigned format,
                              uint8_t **raw, size_t *raw_size, uint8_t **decoded,
                              size_t *decoded_size) {
    enum { MAX_EXPANDED = 512 * 1024, MAX_CONTROLS = 128 * 1024 };
    size_t records_size = 20 + ((size_t)tiles + 1) * 8;
    size_t expanded = 0, previous = 0;
    for (unsigned i = 0; i < tiles; ++i) {
        const uint8_t *record = records + 20 + i * 8;
        size_t source = word(record), width = record[2], height = record[3];
        size_t padded = (width + 7) & ~(size_t)7;
        size_t pitch = format == 0 ? padded / 2 : format == 1 ? padded : padded * 2;
        size_t bytes = pitch * height;
        if (source < previous || (source > previous && source != records_size + expanded))
            return GEX_TITLE_CEL_UNSUPPORTED;
        if (source > previous) {
            if (bytes > MAX_EXPANDED - expanded) return GEX_TITLE_CEL_UNSUPPORTED;
            expanded += bytes;
            previous = source;
        }
    }
    if (!expanded || (expanded & 3)) return GEX_TITLE_CEL_INVALID;
    uint8_t header[4];
    if (capture_read(read, context, image, records_size, header, sizeof header))
        return GEX_TITLE_CEL_INVALID;
    size_t data_offset = (size_t)header[0] | (size_t)header[1] << 8 |
                         (size_t)header[2] << 16 | (size_t)header[3] << 24;
    if (data_offset < 4 || data_offset > MAX_CONTROLS) return GEX_TITLE_CEL_INVALID;
    size_t control = 4, used = 0, produced = 0;
    while (produced < expanded) {
        uint8_t command;
        if (control >= data_offset ||
            capture_read(read, context, image, records_size + control, &command, 1))
            return GEX_TITLE_CEL_INVALID;
        ++control;
        size_t words = command & 0x7f;
        if (!words) words = 128;
        if (words > (expanded - produced) / 4) return GEX_TITLE_CEL_INVALID;
        produced += words * 4;
        used += command & 0x80 ? 4 : words * 4;
    }
    size_t compressed_size = records_size + data_offset + used;
    size_t expanded_size = records_size + expanded;
    uint8_t *snapshot = malloc(compressed_size);
    uint8_t *plain = malloc(expanded_size);
    if (!snapshot || !plain) {
        free(snapshot);
        free(plain);
        return GEX_TITLE_CEL_NO_MEMORY;
    }
    int status = GEX_TITLE_CEL_INVALID;
    if (capture_read(read, context, image, 0, snapshot, compressed_size)) goto done;
    memcpy(plain, snapshot, records_size);
    plain[16] &= (uint8_t)~4u;
    status = gex_title_cel_decompress(snapshot + records_size, compressed_size - records_size,
                                      plain + records_size, expanded);
    if (status != GEX_TITLE_CEL_OK) goto done;
    *raw = snapshot;
    *raw_size = compressed_size;
    *decoded = plain;
    *decoded_size = expanded_size;
    return GEX_TITLE_CEL_OK;
done:
    free(snapshot);
    free(plain);
    return status;
}

static int capture(gex_title_cel_read read, void *context,
                   uint32_t image, uint32_t palette, int compressed_allowed,
                   gex_title_cel_snapshot *out) {
    if (!out) return GEX_TITLE_CEL_INVALID;
    memset(out, 0, sizeof *out);
    if (!read || !image) return GEX_TITLE_CEL_INVALID;
    uint8_t records[20 + 513 * 8];
    if (capture_read(read, context, image, 0, records, 20))
        return GEX_TITLE_CEL_INVALID;
    unsigned format = records[16] & 3;
    if (format == 3 || (records[16] & 0x40) ||
        ((records[16] & 4) && !compressed_allowed)) return GEX_TITLE_CEL_UNSUPPORTED;
    size_t end = 20;
    unsigned tiles;
    for (tiles = 0; tiles <= 512; ++tiles) {
        size_t at = 20 + tiles * 8;
        if (capture_read(read, context, image, at, records + at, 8))
            return GEX_TITLE_CEL_INVALID;
        if (end < at + 8) end = at + 8;
        int source = (int16_t)word(records + at);
        if (!source) break;
        if (tiles == 512) return GEX_TITLE_CEL_UNSUPPORTED;
        unsigned width = records[at + 2], height = records[at + 3];
        if (source < 0 || !width || !height) return GEX_TITLE_CEL_INVALID;
        unsigned padded = (width + 7) & ~7u;
        size_t pitch = format == 0 ? padded / 2 : format == 1 ? padded : padded * 2;
        size_t limit = (size_t)source + pitch * height;
        if (end < limit) end = limit;
    }
    if (!tiles) return GEX_TITLE_CEL_INVALID;
    int result = GEX_TITLE_CEL_OK;
    uint8_t *buffer = NULL, *compressed = NULL;
    size_t compressed_size = 0;
    if (records[16] & 4) {
        result = capture_compressed(read, context, image, records, tiles, format,
                                    &compressed, &compressed_size, &buffer, &end);
        if (result != GEX_TITLE_CEL_OK) return result;
    } else {
        buffer = calloc(1, end);
        if (!buffer) return GEX_TITLE_CEL_NO_MEMORY;
        memcpy(buffer, records, 20 + (tiles + 1) * 8);
        for (unsigned i = 0; i < tiles; ++i) {
            const uint8_t *record = records + 20 + i * 8;
            unsigned source = word(record), width = record[2], height = record[3];
            unsigned padded = (width + 7) & ~7u;
            size_t pitch = format == 0 ? padded / 2 : format == 1 ? padded : padded * 2;
            if (capture_read(read, context, image, source, buffer + source, pitch * height)) {
                result = GEX_TITLE_CEL_INVALID;
                goto done;
            }
        }
    }
    result = gex_title_cel_bounds(buffer, end, &out->cel);
    if (result != GEX_TITLE_CEL_OK) goto done;
    size_t count = (size_t)out->cel.width * out->cel.height;
    out->pixels = malloc(count * sizeof *out->pixels);
    if (!out->pixels) {
        result = GEX_TITLE_CEL_NO_MEMORY;
        goto done;
    }
    result = gex_title_cel_decode(buffer, end, &out->cel, out->pixels, count);
    if (result != GEX_TITLE_CEL_OK) goto done;
    out->palette_size = format == 0 ? 32 : format == 1 ? 512 : 0;
    if (out->palette_size && (!palette ||
        capture_read(read, context, palette, 0, out->palette, out->palette_size)))
        result = GEX_TITLE_CEL_INVALID;
    if (result == GEX_TITLE_CEL_OK) {
        out->source = compressed ? compressed : buffer;
        out->source_size = compressed ? compressed_size : end;
        out->image_address = image;
        out->palette_address = palette;
        if (compressed) compressed = NULL;
        else buffer = NULL;
    }
done:
    free(compressed);
    free(buffer);
    if (result != GEX_TITLE_CEL_OK) gex_title_cel_release(out);
    return result;
}

int gex_title_cel_capture(gex_title_cel_read read, void *context,
                          uint32_t image, uint32_t palette,
                          gex_title_cel_snapshot *out) {
    return capture(read, context, image, palette, 0, out);
}

int gex_title_cel_capture_map(gex_title_cel_read read, void *context,
                              uint32_t image, uint32_t palette,
                              gex_title_cel_snapshot *out) {
    return capture(read, context, image, palette, 1, out);
}

void gex_title_cel_release(gex_title_cel_snapshot *snapshot) {
    if (!snapshot) return;
    free(snapshot->pixels);
    free(snapshot->source);
    memset(snapshot, 0, sizeof *snapshot);
}

static int matches_bytes(gex_title_cel_read read, gex_title_cel_match match,
                         void *context, uint32_t base,
                         size_t offset, const uint8_t *expected, size_t size) {
    if (match) {
        if (offset > UINT32_MAX - base || size > UINT32_MAX - (base + offset))
            return 0;
        return match(context, base + (uint32_t)offset, expected, size) == 1;
    }
    uint8_t bytes[512];
    while (size) {
        size_t chunk = size < sizeof bytes ? size : sizeof bytes;
        if (capture_read(read, context, base, offset, bytes, chunk) ||
            memcmp(bytes, expected, chunk))
            return 0;
        offset += chunk;
        expected += chunk;
        size -= chunk;
    }
    return 1;
}

int gex_title_cel_matches_image(gex_title_cel_read read, gex_title_cel_match match, void *context,
                                uint32_t image, const gex_title_cel_snapshot *snapshot) {
    if (!read || !snapshot || !snapshot->source || image != snapshot->image_address) return 0;
    if (snapshot->source_size > 16 && (snapshot->source[16] & 4))
        return matches_bytes(read, match, context, image, 0, snapshot->source,
                             snapshot->source_size);
    size_t records = 20 + ((size_t)snapshot->cel.tiles + 1) * 8;
    if (records > snapshot->source_size ||
        !matches_bytes(read, match, context, image, 0, snapshot->source, records))
        return 0;
    for (unsigned i = 0; i < snapshot->cel.tiles; ++i) {
        const uint8_t *record = snapshot->source + 20 + i * 8;
        size_t source = word(record), width = record[2], height = record[3];
        size_t padded = (width + 7) & ~(size_t)7;
        size_t pitch = snapshot->cel.format == 0 ? padded / 2 :
                       snapshot->cel.format == 1 ? padded : padded * 2;
        size_t bytes = pitch * height;
        if (source > snapshot->source_size || bytes > snapshot->source_size - source ||
            !matches_bytes(read, match, context, image, source, snapshot->source + source, bytes))
            return 0;
    }
    return 1;
}

int gex_title_cel_matches(gex_title_cel_read read, gex_title_cel_match match, void *context,
                          uint32_t image, uint32_t palette,
                          const gex_title_cel_snapshot *snapshot) {
    return snapshot && palette == snapshot->palette_address &&
           gex_title_cel_matches_image(read, match, context, image, snapshot) &&
           (!snapshot->palette_size ||
            matches_bytes(read, match, context, palette, 0, snapshot->palette, snapshot->palette_size));
}

int gex_title_cel_visibility(const gex_title_cel *cel, const uint8_t *palette,
                             size_t palette_size, const uint8_t *modulation,
                             unsigned rgb, uint32_t visible[8]) {
    if (!cel || !visible || cel->format > 2 ||
        (cel->format < 2 && (!palette || !modulation ||
                             palette_size < (cel->format ? 512u : 32u))))
        return GEX_TITLE_CEL_INVALID;
    memset(visible, 0, 8 * sizeof *visible);
    if (cel->format == 2) return GEX_TITLE_CEL_OK;
    unsigned count = cel->format ? 256 : 16;
    for (unsigned index = 0; index < count; ++index) {
        unsigned source = word(palette + index * 2);
        unsigned modulated = source & 0x8000;
        for (unsigned component = 0; component < 3; ++component) {
            unsigned tint = (rgb >> (component * 8 + 3)) & 31;
            unsigned channel = (source >> (component * 5)) & 31;
            modulated |= (unsigned)modulation[(tint << 5) | channel] << (component * 5);
        }
        if (source && modulated) visible[index / 32] |= UINT32_C(1) << (index & 31);
    }
    return GEX_TITLE_CEL_OK;
}

int gex_title_cel_colors(const gex_title_cel *cel, const uint32_t *pixels,
                         size_t count, const uint8_t *palette, size_t palette_size,
                         const uint8_t *modulation, unsigned rgb, int semitransparent,
                         uint16_t *opaque, uint16_t *blended) {
    uint32_t mask[8];
    if (!cel || !pixels || !opaque || !blended || !cel->width || !cel->height ||
        cel->width > 256 || cel->height > 256 ||
        count < (size_t)cel->width * cel->height ||
        gex_title_cel_visibility(cel, palette, palette_size, modulation, rgb, mask))
        return GEX_TITLE_CEL_INVALID;
    for (size_t i = 0, n = (size_t)cel->width * cel->height; i < n; ++i) {
        unsigned value = pixels[i], color = 0, visible = 0;
        if (value & 0x10000u) {
            if (cel->format < 2) {
                unsigned index = value & (cel->format ? 255u : 15u);
                color = word(palette + index * 2);
                visible = (mask[index / 32] >> (index & 31)) & 1;
            } else {
                color = value & 0xffffu;
                visible = color != 0;
            }
        }
        uint16_t native = visible ? (uint16_t)(0x8000u | (color & 31u) << 10 |
                                              (color & 0x3e0u) | (color >> 10 & 31u)) : 0;
        int blend = semitransparent && (color & 0x8000u);
        opaque[i] = blend ? 0 : native;
        blended[i] = blend ? native : 0;
    }
    return GEX_TITLE_CEL_OK;
}

static unsigned spread(unsigned value) {
    value = (value | value << 4) & 0x0f0f;
    value = (value | value << 2) & 0x3333;
    return (value | value << 1) & 0x5555;
}

int gex_title_cel_twiddle(const gex_title_cel *cel, const uint16_t *pixels,
                          size_t count, unsigned width, unsigned height,
                          uint32_t *words, size_t word_count) {
    if (!cel || !pixels || !words || !cel->width || !cel->height ||
        width < 8 || width > 256 || (width & (width - 1)) ||
        height < 8 || height > 256 || (height & (height - 1)) ||
        cel->width > width || cel->height > height ||
        count < (size_t)cel->width * cel->height ||
        word_count < (size_t)width * height / 2)
        return GEX_TITLE_CEL_INVALID;
    unsigned side = width < height ? width : height;
    unsigned mask = side - 1;
    unsigned x_offsets[128];
    for (unsigned x = 0; x < width; x += 2)
        x_offsets[x / 2] = (spread(x & mask) << 1) / 2 +
                           (x / side) * side * side / 2;
    memset(words, 0, (size_t)width * height * sizeof(uint16_t));
    for (unsigned y = 0; y < cel->height; y += 2) {
        unsigned y_offset = spread(y & mask) / 2 + (y / side) * side * side / 2;
        const uint16_t *row = pixels + (size_t)y * cel->width;
        const uint16_t *next = y + 1 < cel->height ? row + cel->width : NULL;
        unsigned x = 0;
        for (; x + 1 < cel->width; x += 2) {
            unsigned at = y_offset + x_offsets[x / 2];
            words[at] = row[x] | (uint32_t)(next ? next[x] : 0) << 16;
            words[at + 1] = row[x + 1] | (uint32_t)(next ? next[x + 1] : 0) << 16;
        }
        if (x < cel->width)
            words[y_offset + x_offsets[x / 2]] =
                row[x] | (uint32_t)(next ? next[x] : 0) << 16;
    }
    return GEX_TITLE_CEL_OK;
}
