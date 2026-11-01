from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ExecutableIOTests(unittest.TestCase):
    def test_hash_and_pe_reads_preserve_errors_and_short_reads(self):
        toolkit = Path(os.environ.get("GEX_PCRECOMP_DIR", ROOT / "pcrecomp"))
        with tempfile.TemporaryDirectory(prefix="gex-executable-io-") as work:
            work = Path(work)
            source = work / "test.c"
            source.write_text(r"""
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "identity.h"
static int allocation_fail;
static void *pe_alloc(size_t bytes);
#define malloc pe_alloc
#include "pe_loader.c"
#undef malloc
static void *pe_alloc(size_t bytes) { return allocation_fail ? NULL : malloc(bytes); }

static uint8_t input[1024 * 1024];
static size_t input_size, visible_size, position, limit, fail_after;
static int opens, closes, reads, interrupts, fail_open, fail_close, fail_seek;

int gex_file_open(const char *path, unsigned flags) {
    assert(!strcmp(path, "GEX.exe") && flags == GEX_FILE_READ);
    ++opens;
    position = 0;
    return fail_open ? -1 : 7;
}
int gex_file_close(int file) { assert(file == 7); ++closes; return fail_close ? -1 : 0; }
int64_t gex_file_read(int file, void *data, size_t bytes) {
    assert(file == 7);
    ++reads;
    if (interrupts) { --interrupts; errno = EINTR; return -1; }
    if (position >= fail_after) { errno = EIO; return -1; }
    if (position >= visible_size) return 0;
    if (bytes > limit) bytes = limit;
    if (bytes > visible_size - position) bytes = visible_size - position;
    memcpy(data, input + position, bytes);
    position += bytes;
    return (int64_t)bytes;
}
int64_t gex_file_seek(int file, int64_t offset, int origin) {
    assert(file == 7 && offset == 0);
    if (fail_seek == origin + 1) return -1;
    assert(origin == SEEK_END || origin == SEEK_SET);
    position = origin == SEEK_END ? input_size : 0;
    return (int64_t)position;
}
enum gex_io_error gex_fs_error(void) {
    return errno == EINTR ? GEX_IO_INTERRUPTED : GEX_IO_OTHER;
}
static void reset(void) {
    visible_size = input_size;
    position = opens = closes = reads = interrupts = 0;
    limit = fail_after = SIZE_MAX;
    fail_open = fail_close = fail_seek = allocation_fail = 0;
}
static void check_slurp(int success) {
    size_t size = SIZE_MAX;
    uint8_t *bytes = slurp("GEX.exe", &size);
    assert(!!bytes == success);
    assert(opens == 1 && closes == !fail_open);
    if (success) {
        assert(size == input_size);
        assert(!memcmp(bytes, input, size));
    } else assert(size == SIZE_MAX);
    free(bytes);
}
static void check_hash(int success) {
    assert(gex_verify_executable("GEX.exe") == !success);
    assert(opens == 1 && closes == !fail_open);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    input_size = fread(input, 1, sizeof input, file);
    assert(input_size && input_size < sizeof input && !ferror(file));
    assert(fclose(file) == 0);
    reset(); check_hash(1);
    reset(); limit = 137; interrupts = 3; check_hash(1);
    reset(); fail_open = 1; check_hash(0);
    reset(); fail_after = 0; check_hash(0);
    reset(); limit = 8192; fail_after = 8192; check_hash(0);
    reset(); fail_close = 1; check_hash(0);
    reset(); visible_size = 3; check_hash(0);
    reset(); input[0] ^= 1; check_hash(0); input[0] ^= 1;
    reset(); check_slurp(1);
    reset(); limit = 137; interrupts = 3; check_slurp(1);
    reset(); fail_open = 1; check_slurp(0);
    reset(); allocation_fail = 1; check_slurp(0); assert(!reads);
    reset(); fail_seek = SEEK_END + 1; check_slurp(0); assert(!reads);
    reset(); fail_seek = SEEK_SET + 1; check_slurp(0); assert(!reads);
    reset(); fail_after = 0; check_slurp(0);
    reset(); limit = 8192; fail_after = 8192; check_slurp(0);
    reset(); fail_close = 1; check_slurp(0);
    reset(); visible_size = 3; check_slurp(0);
    input_size = 0;
    reset(); check_slurp(1); assert(!reads);
    return 0;
}
""")
            program = work / "executable-io"
            subprocess.run(
                [os.environ.get("GEX_TEST_CC", "cc"), "-std=c11", "-O2",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-I", str(ROOT / "src"),
                 "-I", str(toolkit / "runtime/win32hle"),
                 "-I", str(toolkit / "runtime/recomp32"),
                 str(source), str(ROOT / "src/identity.c"), str(ROOT / "src/sha256.c"),
                 "-o", str(program)],
                check=True, capture_output=True, text=True, timeout=30,
            )
            result = subprocess.run(
                [str(program), str(ROOT / "Gex/GEX.exe")],
                capture_output=True, text=True, timeout=15,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
