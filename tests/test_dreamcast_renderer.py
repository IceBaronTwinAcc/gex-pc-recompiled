from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DreamcastRendererTests(unittest.TestCase):
    def test_pvr_palette_writes_retire_pending_presentation(self):
        source = (ROOT / "src/platform/dreamcast/packet_gpu.c").read_text()
        start = source.rfind("static ", 0, source.index("program_palette("))
        helper = source[start:source.index("static int native_polygon_draw(", start)]
        invoke = "program_palette(0, colors, 2, error, sizeof error)"
        harness = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define WORK(field, value) ((void)0)
static uint32_t programmed_palettes[32], palettes[1024];
static int pending, fence_failure, fences, writes, unsafe_writes, failures;
char error[64];
int complete_presentation(char *error, size_t size) {
    if (!pending) return 0;
    ++fences;
    if (fence_failure) {
        snprintf(error, size, "completion failed");
        return -1;
    }
    pending = 0;
    return 0;
}
void pvr_set_pal_entry(unsigned at, uint32_t color) {
    (void)at; (void)color;
    ++writes;
    unsafe_writes += pending;
}
#define EXPECT(e) do { if (!(e)) { \
    fprintf(stderr, "palette fence failed: %s\n", #e); ++failures; \
} } while (0)
"""
        harness += helper
        harness += f"""
int main(void) {{
    const uint32_t colors[] = {{0xff123456, 0xff789abc}};
    pending = 1;
    EXPECT(!{invoke});
    EXPECT(fences == 1 && !pending && writes == 2 && !unsafe_writes);
    pending = 1;
    EXPECT(!{invoke});
    EXPECT(fences == 1 && pending && writes == 2);
    programmed_palettes[0] = 0;
    fence_failure = 1;
    EXPECT({invoke} != 0);
    EXPECT(fences == 2 && pending && writes == 2 && !unsafe_writes);
    EXPECT(!programmed_palettes[0] && !strcmp(error, "completion failed"));
    return failures != 0;
}}
"""
        with tempfile.TemporaryDirectory(prefix="gex-pvr-palette-fence-") as work:
            work = Path(work)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", str(work / "test.c"),
                            "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True, timeout=30)

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
