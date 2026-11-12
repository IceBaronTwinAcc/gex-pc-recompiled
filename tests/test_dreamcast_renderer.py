from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DreamcastRendererTests(unittest.TestCase):
    def test_pvr_fractional_rectangle_fit(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        start = source.index("static int fit_rectangle_axis(")
        helper = source[start:source.index("static int native_polygon_prepare_impl(", start)]
        harness = r"""
#include <math.h>
#include <stdio.h>
#include "src/platform/dreamcast/sampling.h"
static int texel_floor(float value) {
    int truncated = (int)value;
    return truncated - (value < truncated);
}
"""
        harness += helper
        harness += r"""
int main(void) {
    const int origins[] = {0, 32, 220, 255};
    for (int width = 2; width <= 64; ++width)
        for (int span = -40; span <= 40; ++span)
            for (int flip = 0; flip < 2; ++flip)
                for (int clip = 0; clip < 3; ++clip)
                    for (unsigned o = 0; o < sizeof origins / sizeof *origins; ++o) {
                        int origin = origins[o], position = flip ? 100 + width : 100;
                        int end = flip ? 100 : 100 + width;
                        int low = 100 + (clip == 1 ? width / 4 : 0);
                        int high = 100 + width - (clip == 2 ? width / 4 : 0);
                        int first = flip ? position - high : low - position;
                        int count = high - low;
                        int64_t fixed = (int64_t)span * 65536 / width;
                        int mismatch = 0;
                        for (int i = first; i < first + count; ++i) {
                            int expected = origin + flip +
                                (int)floor((double)i * fixed / 65536);
                            float old = origin + flip + i * ((float)span / width) + 1.0f / 1024;
                            if (span % width && texel_floor(old) != expected) mismatch = 1;
                        }
                        float a, b;
                        int fitted = fit_rectangle_axis(position, end, origin, origin + span,
                                                        low, high, &a, &b);
                        if (fitted != mismatch) {
                            fprintf(stderr, "fit skipped: w=%d span=%d flip=%d clip=%d origin=%d\n",
                                    width, span, flip, clip, origin);
                            return 1;
                        }
                        if (fitted)
                            for (int i = first; i < first + count; ++i) {
                                int expected = origin + flip +
                                    (int)floor((double)i * fixed / 65536);
                                float sample = a + (i - first + 0.5f) * ((b - a) / count);
                                if (!(sample > expected && sample < expected + 1)) return 1;
                            }
                    }
    const int widths[] = {96, 128, 255, 256, 319, 320, 500, 1024, 32767};
    const int spans[] = {-256, -31, -2, 2, 15, 16, 22, 31, 255, 256};
    for (unsigned w = 0; w < sizeof widths / sizeof *widths; ++w)
        for (unsigned s = 0; s < sizeof spans / sizeof *spans; ++s)
            for (int flip = 0; flip < 2; ++flip)
                for (int clip = 0; clip < 3; ++clip)
                    for (unsigned o = 0; o < sizeof origins / sizeof *origins; ++o) {
                        int width = widths[w], span = spans[s], origin = origins[o];
                        int count = width < 320 ? width : 320;
                        int first = clip == 0 ? 0 : clip == 1 ? (width - count) / 2 : width - count;
                        int position = flip ? width : 0, end = flip ? 0 : width;
                        int low = flip ? width - first - count : first;
                        int high = low + count;
                        int64_t fixed = (int64_t)span * 65536 / width;
                        int mismatch = 0;
                        for (int i = first; i < first + count; ++i) {
                            int expected = origin + flip +
                                (int)floor((double)i * fixed / 65536);
                            float old = origin + flip + i * ((float)span / width) + 1.0f / 1024;
                            if (span % width && texel_floor(old) != expected) mismatch = 1;
                        }
                        float a, b;
                        int fitted = fit_rectangle_axis(position, end, origin, origin + span,
                                                        low, high, &a, &b);
                        if (fitted != mismatch) {
                            fprintf(stderr, "wide fit skipped: w=%d span=%d flip=%d clip=%d origin=%d\n",
                                    width, span, flip, clip, origin);
                            return 1;
                        }
                    }
    unsigned random = 1;
    for (int test = 0; test < 120000; ++test) {
        random = random * 1664525u + 1013904223u;
        int width = 1 + (int)(random % 32767);
        random = random * 1664525u + 1013904223u;
        int span = (int)(random % 513) - 256;
        random = random * 1664525u + 1013904223u;
        int count = 1 + (int)(random % (width < 320 ? width : 320));
        random = random * 1664525u + 1013904223u;
        int first = (int)(random % (width - count + 1));
        random = random * 1664525u + 1013904223u;
        int origin = (int)(random % 256), flip = random >> 31;
        int position = flip ? width : 0, end = flip ? 0 : width;
        int low = flip ? width - first - count : first, high = low + count;
        int64_t fixed = (int64_t)span * 65536 / width;
        int mismatch = 0;
        for (int i = first; i < first + count; ++i) {
            int expected = origin + flip + (int)floor((double)i * fixed / 65536);
            float old = origin + flip + i * ((float)span / width) + 1.0f / 1024;
            if (span % width && texel_floor(old) != expected) mismatch = 1;
        }
        float a, b;
        int fitted = fit_rectangle_axis(position, end, origin, origin + span,
                                        low, high, &a, &b);
        if (fitted != mismatch) {
            fprintf(stderr, "random fit skipped: w=%d span=%d first=%d count=%d origin=%d\n",
                    width, span, first, count, origin);
            return 1;
        }
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="gex-pvr-rectangle-fit-") as work:
            work = Path(work)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", "-I", str(ROOT),
                            str(work / "test.c"), "-lm", "-o", str(work / "test")],
                           check=True)
            subprocess.run([str(work / "test")], check=True, timeout=30)

    def test_pvr_fractional_axis_fit(self):
        harness = r"""
#include <math.h>
#include <stdio.h>
#include "src/platform/dreamcast/sampling.h"

static int check(int width, int span, int first, int count) {
    gex_pvr_axis_fit fit;
    if (!gex_pvr_fit_axis(width, span, first, count, &fit)) {
        fprintf(stderr, "fit failed: %d %d %d %d\n", width, span, first, count);
        return 1;
    }
    int step = (int)((long long)span * 65536 / width);
    for (int i = 0; i < count; ++i) {
        int expected = (int)floor((double)(first + i) * step / 65536);
        float actual = fit.first + i * fit.step;
        if (!(actual > expected && actual < expected + 1)) {
            fprintf(stderr, "fit mismatch: %d %d %d %d i=%d actual=%.9f expected=%d\n",
                    width, span, first, count, i, actual, expected);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    for (int width = 1; width <= 96; ++width)
        for (int span = -40; span <= 40; ++span)
            if (check(width, span, 0, width) ||
                check(width, span, width / 4, width - width / 4))
                return 1;
    if (check(320, 256, 0, 320) ||
        check(320, 256, 127, 97) ||
        check(320, -256, 0, 320))
        return 1;
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="gex-pvr-axis-fit-") as work:
            work = Path(work)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", "-I", str(ROOT),
                            str(work / "test.c"), "-lm", "-o", str(work / "test")],
                           check=True)
            subprocess.run([str(work / "test")], check=True, timeout=30)

    def test_pvr_texture_clear_uses_word_stores(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        start = source.index("static void zero_texture_memory(pvr_ptr_t memory, size_t bytes) {")
        helper = source[start:source.index("static texture *texture_region_base(", start)]
        create = source[source.index("static void *create("):source.index("static void destroy(")]
        self.assertIn("zero_texture_memory(t->image[i], TW * TH * 2)", create)
        harness = r"""
#include <stdint.h>
#include <stddef.h>
typedef void *pvr_ptr_t;
"""
        harness += helper
        harness += r"""
int main(void) {
    uint32_t data[130];
    for (size_t words = 0; words <= 128; ++words) {
        for (size_t i = 0; i < 130; ++i) data[i] = 0x5a5a5a5a;
        zero_texture_memory(data + 1, words * sizeof *data);
        if (data[0] != 0x5a5a5a5a) return 1;
        for (size_t i = 1; i <= words; ++i)
            if (data[i] != 0) return 1;
        for (size_t i = words + 1; i < 130; ++i)
            if (data[i] != 0x5a5a5a5a) return 1;
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="gex-pvr-clear-") as work:
            work = Path(work)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", str(work / "test.c"),
                            "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True, timeout=30)

    def test_pvr_vram_copy_handles_unaligned_sources(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        start = source.index("static void copy_vram_words(")
        helper = source[start:source.index("static void update_row(", start)]
        harness = r"""
#include <stdint.h>
#include <stddef.h>
#include <string.h>
"""
        harness += helper
        harness += r"""
int main(void) {
    union { uint32_t align; uint8_t bytes[2080]; } input, output;
    for (unsigned i = 0; i < sizeof input.bytes; ++i)
        input.bytes[i] = (uint8_t)(i * 73 + 19);
    for (unsigned src = 0; src < 8; ++src)
        for (unsigned dst = 0; dst < 8; dst += 2)
            for (unsigned words = 0; words <= 1024; ++words) {
                memset(output.bytes, 0x5a, sizeof output.bytes);
                copy_vram_words((uint16_t *)(output.bytes + dst + 8),
                                input.bytes + src, words);
                if (memcmp(output.bytes + dst + 8, input.bytes + src, words * 2))
                    return 1;
                for (unsigned i = 0; i < dst + 8; ++i)
                    if (output.bytes[i] != 0x5a) return 1;
                for (unsigned i = dst + 8 + words * 2; i < sizeof output.bytes; ++i)
                    if (output.bytes[i] != 0x5a) return 1;
            }
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="gex-pvr-vram-copy-") as work:
            work = Path(work)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", str(work / "test.c"),
                            "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True, timeout=30)

    def test_pvr_integral_rectangle_samples_texel_centers(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        start = source.index("static float integral_sample_bias(")
        helper = source[start:source.index("static int fit_rectangle_axis(", start)]
        harness = r"""
#include <math.h>
#include <stdio.h>
"""
        harness += helper
        harness += r"""
int main(void) {
    const int origins[] = {0, 1, 32, 128, 220, 255};
    for (int width = 1; width <= 320; ++width)
        for (int step = -2; step <= 2; ++step)
            for (unsigned o = 0; o < sizeof origins / sizeof *origins; ++o)
                for (int flip = 0; flip < 2; ++flip) {
                    float x0 = flip ? 400 : 80;
                    float origin = origins[o];
                    float bias = integral_sample_bias(x0, width, origin, step);
                    if (bias != 0.5f) return 1;
                    float u0 = origin + flip - step * 0.5f + bias;
                    for (int x = 0; x < width; ++x) {
                        int column = flip ? width - 1 - x : x;
                        int expected = origins[o] + flip + column * step;
                        float sample = u0 +
                            ((flip ? 400 - width + x : 80 + x) + 0.5f - x0) *
                            (flip ? -step : step);
                        if (sample != expected + 0.5f) {
                            fprintf(stderr, "width=%d step=%d flip=%d x=%d: %f != %d.5\n",
                                    width, step, flip, x, sample, expected);
                            return 1;
                        }
                    }
                }
    if (integral_sample_bias(0, 6, 32, 2.0f / 6) != 1.0f / 1024 ||
        integral_sample_bias(0.25f, 6, 32, 1) != 1.0f / 1024 ||
        integral_sample_bias(0, 6.5f, 32, 1) != 1.0f / 1024 ||
        integral_sample_bias(0, 6, 32.5f, 1) != 1.0f / 1024 ||
        integral_sample_bias(NAN, 6, 32, 1) != 1.0f / 1024 ||
        integral_sample_bias(0, 6, 32, INFINITY) != 1.0f / 1024)
        return 1;
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="gex-pvr-integral-sampling-") as work:
            work = Path(work)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", str(work / "test.c"),
                            "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True, timeout=30)

    def test_pvr_palette_writes_skip_unchanged_entries(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        start = source.rfind("static ", 0, source.index("program_palette("))
        helper = source[start:source.index("static int native_polygon_draw(", start)]
        harness = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define WORK(field, value) ((void)0)
static uint32_t programmed_palettes[32], palettes[1024];
static int writes, failures;
void pvr_set_pal_entry(unsigned at, uint32_t color) {
    (void)at; (void)color;
    ++writes;
}
#define EXPECT(e) do { if (!(e)) { \
    fprintf(stderr, "palette update failed: %s\n", #e); ++failures; \
} } while (0)
"""
        harness += helper
        harness += r"""
int main(void) {
    uint32_t colors[] = {0xff123456, 0xff789abc};
    program_palette(0, colors, 2);
    EXPECT(writes == 2 && palettes[0] == colors[0] && palettes[1] == colors[1]);
    program_palette(0, colors, 2);
    EXPECT(writes == 2);
    colors[1] = 0xffabcdef;
    program_palette(0, colors, 2);
    EXPECT(writes == 3 && palettes[1] == colors[1]);
    return failures != 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="gex-pvr-palette-fence-") as work:
            work = Path(work)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", str(work / "test.c"),
                            "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True, timeout=30)

    def test_pvr_presentation_renders_to_framebuffer(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        flush = source[source.index("static int flush_commands_impl("):
                       source.index("static int flush_commands(", source.index("static int flush_commands_impl("))]
        self.assertIn("if (wait_for_render) {", flush)
        self.assertIn("pvr_scene_begin_txr(", flush)
        self.assertIn("} else {\n            pvr_scene_begin();", flush)
        self.assertNotIn("pvr_scene_begin_rtt(", source)

    def test_pvr_preservation_uses_one_to_one_nearest_texture(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        flush = source[source.index("static int flush_commands_impl("):
                       source.index("static int flush_commands(", source.index("static int flush_commands_impl("))]
        context = source[source.index("static void context("):
                         source.index("static void vertices(")]
        quad = source[source.index("static void picture_quad("):
                      source.index("static void clear_texture_reads(")]
        self.assertIn("PVR_FILTER_NONE", context)
        self.assertIn("if (!wait_for_render) {", flush)
        self.assertIn("source + y * (320 / 2), 320 * 2 / 32", flush)
        self.assertIn("PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, TW, TH", quad)
        self.assertIn("vertices(v, 0, 0, 320, 240, 0, 0, 320.0f / TW, 240.0f / TH,", quad)
        self.assertIn("picture_quad(&header, v, current_image(active), 255)", flush)
        self.assertNotIn("finish_feedback(", source)
        self.assertNotIn("save_replay(", source)

    def test_pvr_rect_invalidation_matches_row_writes(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        helpers = source[source.index("static uint32_t row_mask("):
                         source.index("static void native_atlas_write_rect_impl(")]
        harness = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    unsigned page;
    size_t bytes;
    uint32_t dirty[256];
    unsigned region_begin, region_end, region_top, region_bottom;
} texture;
"""
        harness += helpers
        harness += r"""
static uint32_t seed = 0x6de897a1;
static unsigned random_value(void) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
int main(void) {
    for (unsigned i = 0; i < 60000; ++i) {
        unsigned format = i % 3, row = 128u << format;
        texture expected = {.page = (format << 7) | (i & 31), .bytes = row * 256};
        expected.region_begin = random_value() % row;
        expected.region_end = expected.region_begin + 1 +
                              random_value() % (row - expected.region_begin);
        expected.region_top = random_value() & 255;
        expected.region_bottom = expected.region_top + 1 +
                                 random_value() % (256 - expected.region_top);
        texture actual = expected;
        unsigned offset = random_value() & 0xfffff;
        unsigned bytes = 1 + random_value() % 2048;
        unsigned rows = 1 + random_value() % 1025;
        for (unsigned y = 0; y < rows; ++y) {
            unsigned at = (offset + y * 2048) & 0xfffff;
            unsigned count = bytes < 0x100000 - at ? bytes : 0x100000 - at;
            invalidate_rows(&expected, at, count);
            invalidate_rows(&expected, at + 0x100000, count);
            if (bytes > count) {
                invalidate_rows(&expected, 0, bytes - count);
                invalidate_rows(&expected, 0x100000, bytes - count);
            }
        }
        invalidate_rect(&actual, offset, bytes, rows);
        if (memcmp(actual.dirty, expected.dirty, sizeof actual.dirty) ||
            actual.region_end != expected.region_end) {
            fprintf(stderr, "invalidation mismatch: page=%x offset=%x bytes=%u rows=%u\n",
                    expected.page, offset, bytes, rows);
            return 1;
        }
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="gex-pvr-invalidation-") as work:
            work = Path(work)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", str(work / "test.c"),
                            "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True, timeout=30)

    def test_pvr_strided_overlap_matches_legacy(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        helpers = source[source.index("static int interval_overlap("):
                         source.index("static int remap_write_overlap(")]
        harness = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
"""
        harness += helpers
        harness += r"""
static int legacy(unsigned begin, size_t bytes, unsigned offset, size_t pitch,
                  size_t row_bytes, unsigned rows) {
    offset &= 0xfffff;
    return interval_overlap(begin, bytes, offset, pitch, row_bytes, rows) ||
           interval_overlap((uint64_t)begin + 0x100000, bytes, offset, pitch, row_bytes, rows) ||
           interval_overlap(begin, bytes, (uint64_t)offset + 0x100000, pitch, row_bytes, rows);
}
static uint32_t seed = 0x673b98a5;
static unsigned random_value(void) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
static int check(unsigned begin, size_t bytes, unsigned offset, size_t pitch,
                 size_t width, unsigned rows) {
    int actual = write_overlap(begin, bytes, offset, pitch, width, rows);
    int expected = legacy(begin, bytes, offset, pitch, width, rows);
    if (actual == expected) return 0;
    fprintf(stderr, "overlap mismatch: %x %zu %x %zu %zu %u: %d != %d\n",
            begin, bytes, offset, pitch, width, rows, actual, expected);
    return 1;
}
int main(void) {
    const unsigned points[] = {0, 1, 31, 32, 2047, 2048, 0xff800, 0xffff0,
                               0xfffff, 0x100000};
    const unsigned widths[] = {0, 1, 2, 31, 32, 511, 512, 2047, 2048, 2049};
    const unsigned heights[] = {0, 1, 2, 255, 256, 511, 512, 513, 1024};
    for (unsigned a = 0; a < sizeof points / sizeof *points; ++a)
        for (unsigned b = 0; b < sizeof widths / sizeof *widths; ++b)
            for (unsigned c = 0; c < sizeof points / sizeof *points; ++c)
                for (unsigned d = 0; d < sizeof widths / sizeof *widths; ++d)
                    for (unsigned e = 0; e < sizeof heights / sizeof *heights; ++e)
                        if (check(points[a], widths[b], points[c], 2048,
                                  widths[d], heights[e])) return 1;
    for (unsigned i = 0; i < 1000000; ++i) {
        unsigned begin = random_value() & 0xfffff;
        size_t bytes = random_value() % 2050;
        unsigned offset = random_value();
        size_t width = random_value() % 2050;
        unsigned rows = random_value() % 1025;
        size_t pitch = i % 8 ? 2048 : random_value() % 8193;
        if (check(begin, bytes, offset, pitch, width, rows)) return 1;
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="gex-pvr-overlap-") as work:
            work = Path(work)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", str(work / "test.c"),
                            "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True, timeout=30)

    def test_pvr_remap_invalidation_never_misses_written_texels(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        helpers = source[source.index("static int interval_overlap("):
                         source.index("static int snapshot_segment_overlap(")]
        harness = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef struct {
    unsigned page, origin_u, origin_v, dimension;
} remap_texture;
"""
        harness += helpers
        harness += r"""
static uint32_t seed = 0x7d1b3f09;
static unsigned random_value(void) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
static int expected_overlap(const remap_texture *t, unsigned offset,
                            size_t pitch, unsigned bytes, unsigned rows) {
    unsigned source_x = (t->page & 15) * 128 + t->origin_u / 2;
    unsigned source_y = (t->page & 16 ? 256 : 0) + t->origin_v;
    unsigned source_end = source_x + (t->dimension + (t->origin_u & 1) + 1) / 2;
    for (unsigned row = 0; row < rows; ++row)
        for (unsigned byte = 0; byte < bytes; ++byte) {
            unsigned at = (offset + row * pitch + byte) & 0xfffff;
            unsigned y = at / 2048, x = at % 2048;
            if (y >= source_y && y < source_y + t->dimension &&
                x >= source_x && x < source_end) return 1;
        }
    return 0;
}
int main(void) {
    remap_texture odd = {.page = 15, .origin_u = 1, .origin_v = 16, .dimension = 8};
    unsigned last = (odd.page & 15) * 128 + odd.origin_v * 2048 + 4;
    if (!expected_overlap(&odd, last, 2048, 1, 1) ||
        !remap_write_overlap(&odd, last, 2048, 1, 1) ||
        !remap_write_overlap(&odd, last, 0, 1, 1)) {
        fprintf(stderr, "missed final byte of odd-origin remap\n");
        return 1;
    }
    for (unsigned i = 0; i < 100000; ++i) {
        remap_texture t = {.page = random_value() & 31,
            .dimension = 8u << (random_value() % 6)};
        t.origin_u = random_value() % (257 - t.dimension);
        t.origin_v = random_value() % (257 - t.dimension);
        unsigned begin = (t.page & 15) * 128 + (t.page & 16 ? 0x80000 : 0) +
                         t.origin_v * 2048 + t.origin_u / 2;
        unsigned offset = i & 1 ? random_value() & 0xfffff :
            (begin + (random_value() % (t.dimension + 8)) * 2048 +
             random_value() % 300 - 150) & 0xfffff;
        unsigned bytes = 1 + random_value() % 257;
        unsigned rows = 1 + random_value() % 16;
        size_t pitch = i % 4 ? 2048 : random_value() % 4097;
        int actual = remap_write_overlap(&t, offset, pitch, bytes, rows);
        int expected = expected_overlap(&t, offset, pitch, bytes, rows);
        if (expected && !actual) {
            fprintf(stderr, "missed remap: page=%x u=%u v=%u dimension=%u "
                    "offset=%x pitch=%zu bytes=%u rows=%u\n",
                    t.page, t.origin_u, t.origin_v, t.dimension,
                    offset, pitch, bytes, rows);
            return 1;
        }
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="gex-pvr-remap-invalidation-") as work:
            work = Path(work)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", str(work / "test.c"),
                            "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True, timeout=30)

    def test_pvr_snapshot_invalidation_matches_byte_dependencies(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        helpers = source[source.index("static int interval_overlap("):
                         source.index("static int remap_write_overlap(")]
        helpers += source[source.index("static int snapshot_segment_overlap("):
                          source.index("static void dependency_range(")]
        harness = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef struct {
    unsigned page, clut, origin_u, origin_v, dimension;
} transient_texture;
"""
        harness += helpers
        harness += r"""
static uint32_t seed = 0x9f87d2ab;
static unsigned random_value(void) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
static int circular_overlap(unsigned a, unsigned alen, unsigned b, unsigned blen) {
    if (!alen || !blen) return 0;
    if (alen >= 0x100000 || blen >= 0x100000) return 1;
    a &= 0xfffff; b &= 0xfffff;
    unsigned a_first = alen < 0x100000 - a ? alen : 0x100000 - a;
    unsigned b_first = blen < 0x100000 - b ? blen : 0x100000 - b;
    return (a < b + b_first && b < a + a_first) ||
           (alen > a_first && b < alen - a_first) ||
           (blen > b_first && a < blen - b_first) ||
           (alen > a_first && blen > b_first);
}
static int expected(const transient_texture *t, unsigned offset, size_t pitch,
                    unsigned bytes, unsigned rows) {
    unsigned format = t->page >> 7 & 3;
    unsigned base = (t->page & 15) * 128 + (t->page & 16 ? 0x80000 : 0);
    unsigned column = format == 0 ? t->origin_u / 2 :
                      format == 1 ? t->origin_u : t->origin_u * 2;
    unsigned width = format == 0 ? (t->dimension + (t->origin_u & 1) + 1) / 2 :
                     format == 1 ? t->dimension : t->dimension * 2;
    unsigned clut_bytes = format == 0 ? 32 : format == 1 ? 512 : 0;
    for (unsigned y = 0; y < rows; ++y) {
        unsigned at = (offset + (uint64_t)y * pitch) & 0xfffff;
        if (circular_overlap(t->clut * 32, clut_bytes, at, bytes)) return 1;
        for (unsigned sy = 0; sy < t->dimension; ++sy)
            if (circular_overlap(base + (t->origin_v + sy) * 2048 + column,
                                 width, at, bytes)) return 1;
    }
    return 0;
}
static int check(const transient_texture *t, unsigned offset, size_t pitch,
                 unsigned bytes, unsigned rows) {
    int actual = snapshot_write_overlap(t, offset, pitch, bytes, rows);
    int reference = expected(t, offset, pitch, bytes, rows);
    if (actual == reference) return 0;
    fprintf(stderr, "snapshot mismatch: page=%x u=%u v=%u dim=%u clut=%x "
            "offset=%x pitch=%zu bytes=%u rows=%u: %d != %d\n",
            t->page, t->origin_u, t->origin_v, t->dimension, t->clut,
            offset, pitch, bytes, rows, actual, reference);
    return 1;
}
int main(void) {
    transient_texture edge = {.page = 31 | 256, .origin_u = 248,
                              .origin_v = 248, .dimension = 8};
    if (check(&edge, 0x170, 2048, 8, 1) ||
        !snapshot_write_overlap(&edge, 0x170, 2048, 8, 1))
        return 1;
    const unsigned widths[] = {0, 1, 2, 31, 32, 127, 511, 512, 2048, 2049, 0x100000};
    for (unsigned i = 0; i < 25000; ++i) {
        unsigned format = i % 3;
        transient_texture t = {.page = (random_value() & 31) | format << 7,
            .clut = random_value() & 0x7fff, .dimension = 8u << (random_value() % 6)};
        t.origin_u = random_value() % (257 - t.dimension);
        t.origin_v = random_value() % (257 - t.dimension);
        unsigned column = format == 0 ? t.origin_u / 2 :
                          format == 1 ? t.origin_u : t.origin_u * 2;
        unsigned base = (t.page & 15) * 128 + (t.page & 16 ? 0x80000 : 0) +
                        t.origin_v * 2048 + column;
        unsigned offset = i % 3 == 0 ? random_value() :
                          i % 3 == 1 ? base + (random_value() % (t.dimension + 2)) * 2048 +
                                       random_value() % 600 - 300 :
                                       t.clut * 32 + random_value() % 700 - 100;
        size_t pitch = i % 3 ? 2048 : random_value() % 70000;
        unsigned bytes = widths[random_value() % (sizeof widths / sizeof *widths)];
        unsigned rows = random_value() % 17;
        if (check(&t, offset, pitch, bytes, rows)) return 1;
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="gex-pvr-snapshot-invalidation-") as work:
            work = Path(work)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", str(work / "test.c"),
                            "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True, timeout=30)

    def test_native_video_and_store_queue_pixels(self):
        with tempfile.TemporaryDirectory(prefix="gex-dc-renderer-") as work:
            work = Path(work)
            (work / "dc").mkdir()
            (work / "kos.h").write_text("""
#include <stdint.h>
#define DM_320x240 1
#define DM_640x480 2
#define PM_RGB565 1
extern uint16_t *vram_s;
void vid_set_mode(int mode, int format);
void vid_clear(int r, int g, int b);
""")
            (work / "dc/sq.h").write_text("""
#include <stddef.h>
#include <stdint.h>
uint32_t *sq_lock(void *destination);
void *sq_fast_cpy(void *destination, const void *source, size_t blocks);
void sq_wait(void);
void sq_unlock(void);
""")
            (work / "test.c").write_text(r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kos.h"
#include "renderer.h"

static uint16_t video[640 * 480] __attribute__((aligned(32)));
uint16_t *vram_s = video;
static int width, height, locked, copies, waited;
static uint8_t *gdi_pixels;
void vid_set_mode(int mode, int format) {
    assert(format == PM_RGB565);
    width = mode == DM_320x240 ? 320 : 640;
    height = width == 320 ? 240 : 480;
}
void vid_clear(int r, int g, int b) {
    assert(!r && !g && !b);
    memset(video, 0, sizeof video);
}
uint32_t *sq_lock(void *destination) {
    assert(!locked && destination == video);
    locked = 1;
    waited = 0;
    return destination;
}
void *sq_fast_cpy(void *destination, const void *source, size_t blocks) {
    assert(locked);
    assert(!((uintptr_t)destination & 31));
    assert(!((uintptr_t)source & 7));
    assert(blocks > 0);
    assert((uint8_t *)destination >= (uint8_t *)video);
    assert((uint8_t *)destination + blocks * 32 <=
           (uint8_t *)video + width * height * 2);
    ++copies;
    return memcpy(destination, source, blocks * 32);
}
void sq_wait(void) { assert(locked); waited = 1; }
void sq_unlock(void) { assert(locked && waited); locked = 0; }
void hle_gdi_window_surface(uint8_t *pixels, int w, int h, int pitch, int bpp) {
    gdi_pixels = pixels;
    (void)w; (void)h; (void)pitch; (void)bpp;
}
void hle_set_screen_size(int w, int h) { (void)w; (void)h; }

static void check(int w, int h, int padding, int misalign) {
    int pitch = w * 2 + padding;
    uint8_t *allocation = aligned_alloc(32, ((size_t)pitch * h + 63) & ~(size_t)31);
    assert(allocation);
    uint8_t *pixels = allocation + misalign;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint16_t value = (uint16_t)(1 + (y * w + x) % 65535);
            memcpy(pixels + y * pitch + x * 2, &value, 2);
        }
    const gex_renderer_backend *backend = gex_renderer_backend_get();
    gex_render_frame frame = {.pixels = pixels, .width = w, .height = h, .pitch = pitch};
    char error[128];
    assert(!backend->configure(&frame, error, sizeof error));
    assert(width == (w <= 320 && h <= 240 ? 320 : 640));
    copies = 0;
    assert(!backend->present(&frame, error, sizeof error));
    assert(!locked);
    int direct = w == width && h == height && !padding && !misalign;
    assert(copies == (direct ? 1 : gex_renderer_fit(width, height, w, h).height));
    gex_render_rect fit = gex_renderer_fit(width, height, w, h);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            uint16_t expected = 0;
            if (x >= fit.x && x < fit.x + fit.width &&
                y >= fit.y && y < fit.y + fit.height) {
                int sx = (x - fit.x) * w / fit.width;
                int sy = (y - fit.y) * h / fit.height;
                memcpy(&expected, pixels + sy * pitch + sx * 2, 2);
            }
            assert(video[y * width + x] == expected);
        }
    memset(pixels, 0, (size_t)pitch * h);
    assert(!backend->present(&frame, error, sizeof error));
    for (int i = 0; i < width * height; ++i) assert(!video[i]);
    free(allocation);
}
static void check_upload(int w, int h, int sw, int sh, int offset) {
    int pitch = sw * 2 + offset;
    uint8_t *storage = malloc((size_t)pitch * sh + offset);
    assert(storage);
    uint8_t *pixels = storage + offset;
    for (int y=0;y<sh;++y)
        for (int x=0;x<sw;++x) {
            unsigned value=(unsigned)(y*sw+x);
            pixels[y*pitch+x*2]=(uint8_t)value;
            pixels[y*pitch+x*2+1]=(uint8_t)(value>>8);
        }
    assert(!gex_renderer_resize(w,h));
    assert(!gex_renderer_upload_bgr555(pixels,sw,sh,pitch));
    gex_render_frame frame;
    assert(!gex_renderer_frame(&frame));
    assert(frame.pixels!=(uint8_t *)video);
    assert(gdi_pixels==frame.pixels);
    gex_render_rect fit=gex_renderer_fit(width,height,w,h);
    for(int y=0;y<height;++y)
        for(int x=0;x<width;++x) {
            unsigned expected=0;
            if(x>=fit.x && x<fit.x+fit.width && y>=fit.y && y<fit.y+fit.height) {
                int fx=(x-fit.x)*w/fit.width, fy=(y-fit.y)*h/fit.height;
                int sx=fx*sw/w,sy=fy*sh/h;
                const uint8_t *p=pixels+sy*pitch+sx*2;
                unsigned value=p[0]|(unsigned)p[1]<<8;
                expected=(value&31)<<11|(value&0x3E0)<<1|(value>>10&31);
                const uint8_t *q=frame.pixels+fy*frame.pitch+fx*2;
                assert((q[0]|(unsigned)q[1]<<8)==expected);
            }
            assert(video[y*width+x]==expected);
        }
    memset(frame.pixels,0,(size_t)frame.pitch*frame.height);
    assert(!gex_renderer_commit());
    for(int i=0;i<width*height;++i)assert(!video[i]);
    free(storage);
}
int main(void) {
    char error[128];
    const gex_renderer_backend *backend = gex_renderer_backend_get();
    assert(!backend->open(1, error, sizeof error));
    check(320, 240, 0, 0);
    check(640, 480, 0, 0);
    check(320, 240, 4, 0);
    check(320, 240, 0, 2);
    check(427, 240, 2, 0);
    check(200, 240, 0, 0);
    check(200, 100, 0, 0);
    check(320, 224, 0, 0);
    gex_render_frame too_big = {.width = 641, .height = 480};
    assert(backend->configure(&too_big, error, sizeof error) == -1);
    backend->shutdown();
    assert(!gex_renderer_init(backend,1));
    check_upload(320,240,320,240,0);
    check_upload(320,240,320,224,1);
    check_upload(640,480,320,224,0);
    check_upload(320,240,319,223,1);
    check_upload(427,240,320,224,0);
    check_upload(200,100,320,224,0);
    gex_renderer_shutdown();
    return 0;
}
""")
            toolchain = Path(os.environ.get("GEX_PCRECOMP_DIR", ROOT / "pcrecomp"))
            command = [
                os.environ.get("GEX_TEST_CC", "cc"), "-std=c11", "-O2",
                "-I", str(work), "-I", str(ROOT / "src"),
                "-I", str(toolchain / "runtime/win32hle"),
                "-I", str(toolchain / "runtime/recomp32"),
                str(work / "test.c"), str(ROOT / "src/platform/dreamcast/renderer.c"),
                str(ROOT / "src/renderer.c"), "-o", str(work / "renderer-test"),
            ]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(work / "renderer-test")], check=True,
                           capture_output=True, timeout=10)
