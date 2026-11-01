from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DreamcastFilesystemTests(unittest.TestCase):
    def test_aligned_transfers_preserve_read_semantics(self):
        with tempfile.TemporaryDirectory(prefix="gex-dc-filesystem-") as work:
            work = Path(work)
            (work / "kos.h").write_text("""
#include <stddef.h>
#include <stdint.h>
typedef int mutex_t;
typedef int file_t;
typedef struct { char name[256]; int size; } dirent_t;
typedef struct { struct { char pathname[8]; } nmmgr; } vfs_handler_t;
vfs_handler_t *fs_get_handler(int file);
void *fs_get_handle(int file);
#define MUTEX_INITIALIZER 0
#define O_DIR 0x10000
int mutex_lock(mutex_t *mutex);
int mutex_unlock(mutex_t *mutex);
int64_t fs_seek64(int file, int64_t offset, int origin);
int fs_open(const char *path, int flags);
int fs_close(int file);
const dirent_t *fs_readdir(int file);
""")
            (work / "test.c").write_text(r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kos.h"
#include "platform/filesystem.h"

static int locked, calls, dma_calls, seekable, fail_call, short_call;
static int is_cd, seek_calls, fail_seek, blocked, blocks, fail_dup, pins, unpins;
static uint8_t *protected_output;
static size_t position, length;
static uint8_t input[200000];

vfs_handler_t *fs_get_handler(int file) {
    (void)file;
    static vfs_handler_t cd = {.nmmgr = {.pathname = "/cd"}};
    static vfs_handler_t ram = {.nmmgr = {.pathname = "/ram"}};
    return is_cd ? &cd : &ram;
}
void *fs_get_handle(int file) { (void)file; return input; }
int mock_dup(int file) {
    if (fail_dup) { errno = EMFILE; return -1; }
    ++pins;
    return file + 1;
}
void hle_block_begin(void) { assert(!blocked && !locked); blocked = 1; ++blocks; }
void hle_block_end(void) {
    assert(blocked && !locked);
    if (protected_output) {
        for (unsigned i = 0; i < 8192; ++i) assert(protected_output[i] == 0xCC);
        protected_output = NULL;
    }
    blocked = 0;
}
int mock_close(int file) {
    if (file == 5) { assert(blocked && !locked); ++unpins; }
    return 0;
}
int mutex_lock(mutex_t *mutex) {
    assert(!locked && !*mutex);
    *mutex = locked = 1;
    return 0;
}
int mutex_unlock(mutex_t *mutex) {
    assert(locked && *mutex);
    *mutex = locked = 0;
    errno = EBUSY;
    return 0;
}
void hle_fatal(const char *format, ...) {
    (void)format;
    abort();
}
int64_t fs_seek64(int file, int64_t offset, int origin) {
    (void)file;
    if (++seek_calls == fail_seek) { errno = EIO; return -1; }
    if (!seekable) { errno = ESPIPE; return -1; }
    int64_t next = origin == SEEK_SET ? offset :
                   origin == SEEK_CUR ? (int64_t)position + offset : (int64_t)length + offset;
    assert(next >= 0);
    position = (size_t)next;
    return (int64_t)position;
}
ssize_t mock_read(int file, void *data, size_t bytes) {
    (void)file;
    ++calls;
    if (is_cd) assert(blocked);
    if (calls == fail_call) { errno = EIO; return -1; }
    if (locked && !((uintptr_t)data & 31) && !(position & 2047) && bytes >= 2048) {
        assert(bytes <= 65536 && !(bytes & 2047));
        ++dma_calls;
    }
    size_t count = position < length ? length - position : 0;
    if (count > bytes) count = bytes;
    if (calls == short_call && count > 7) count = 7;
    memcpy(data, input + position, count);
    position += count;
    return (ssize_t)count;
}
static void reset(size_t start, size_t size) {
    assert(!locked);
    assert(!blocked);
    assert(!gex_file_close(4));
    position = start;
    length = size;
    calls = dma_calls = fail_call = short_call = 0;
    seekable = 1;
    is_cd = seek_calls = fail_seek = blocks = fail_dup = pins = unpins = 0;
    errno = 0;
}
static void check(size_t offset, size_t bytes, unsigned alignment) {
    uint8_t *storage = aligned_alloc(32, (bytes + 95) & ~(size_t)31);
    assert(storage);
    memset(storage, 0xCC, (bytes + 95) & ~(size_t)31);
    uint8_t *destination = storage + 32 + alignment;
    reset(offset, sizeof input);
    size_t expected = bytes < length - offset ? bytes : length - offset;
    assert(gex_file_read(4, destination, bytes) == (int64_t)expected);
    assert(position == offset + expected);
    assert(!memcmp(destination, input + offset, expected));
    assert(destination[-1] == 0xCC && destination[expected] == 0xCC);
    if (alignment && bytes >= 8192) assert(dma_calls > 0);
    if (!alignment || bytes < 2048) assert(calls == 1 && !dma_calls);
    assert(!locked && !errno);
    free(storage);
}
int main(void) {
    for (size_t i = 0; i < sizeof input; ++i) input[i] = (uint8_t)(i * 13 + i / 2048);
    check(0, 0, 1);
    check(0, 17, 1);
    check(0, 2047, 1);
    check(0, 2048, 1);
    check(0, 8192, 8);
    check(0, 150123, 16);
    check(1, 150123, 24);
    check(2047, 8192, 3);
    check(1234, 8192, 0);
    check(0, 8192, 0);
    check(170003, 50000, 1);

    uint8_t output[100000] __attribute__((aligned(32)));
    reset(0, sizeof input);
    fail_call = 1;
    assert(gex_file_read(4, output + 1, 8192) == -1 && errno == EIO);
    assert(!position && !locked);

    reset(1, sizeof input);
    is_cd = 1;
    memset(output + 1, 0xCC, 8192);
    protected_output = output + 1;
    assert(gex_file_read(4, output + 1, 8192) == 8192);
    assert(pins == 1 && unpins == 1 && blocks == 2 && !blocked && !protected_output);
    assert(calls == 1 && dma_calls == 1 && position == 8193 && !errno);
    assert(!memcmp(output + 1, input + 1, 8192));
    assert(gex_file_read(4, output + 1, 8192) == 8192 && calls == 1 && position == 16385);
    assert(!memcmp(output + 1, input + 8193, 8192));
    assert(fs_seek64(4, 65529, SEEK_SET) == 65529);
    assert(gex_file_read(4, output + 1, 8192) == 8192 && calls == 2 && position == 73721);
    assert(!memcmp(output + 1, input + 65529, 8192));
    assert(!gex_file_read(4, output, 0) && position == 73721 && calls == 2);

    reset(1, sizeof input);
    is_cd = fail_dup = 1;
    assert(gex_file_read(4, output + 1, 8192) == -1 && errno == EMFILE);
    assert(position == 1 && !calls && !blocks && !pins && !unpins);

    reset(0, sizeof input);
    is_cd = 1;
    fail_call = 1;
    assert(gex_file_read(4, output + 1, 8192) == -1 && errno == EIO);
    assert(!position && !locked);
    reset(0, sizeof input);
    is_cd = 1;
    fail_call = 2;
    assert(gex_file_read(4, output + 1, 90000) == 65536 && errno == EIO);
    assert(position == 65536 && !locked && !memcmp(output + 1, input, 65536));
    reset(1, sizeof input);
    is_cd = 1;
    short_call = 1;
    assert(gex_file_read(4, output + 1, 8192) == 6 && calls == 1 && position == 7);
    assert(!locked && !memcmp(output + 1, input + 1, 6));
    reset(sizeof input - 5, sizeof input);
    is_cd = 1;
    assert(gex_file_read(4, output + 1, 8192) == 5 && position == sizeof input);
    assert(!memcmp(output + 1, input + sizeof input - 5, 5));
    assert(!gex_file_read(4, output + 1, 8192) && position == sizeof input && !locked);

    reset(0, sizeof input);
    is_cd = 1;
    fail_seek = 3;
    assert(gex_file_read(4, output + 1, 8192) == -1 && errno == EIO && !locked);
    fail_seek = 0;
    assert(fs_seek64(4, 0, SEEK_SET) == 0);
    assert(gex_file_read(4, output + 1, 8192) == 8192 && calls == 2 && position == 8192);
    assert(!memcmp(output + 1, input, 8192));
    reset(0, sizeof input);
    fail_call = 2;
    assert(gex_file_read(4, output + 1, 90000) == 65536 && errno == EIO);
    assert(position == 65536 && !locked);
    assert(!memcmp(output + 1, input, 65536));
    reset(0, sizeof input);
    short_call = 1;
    assert(gex_file_read(4, output + 1, 8192) == 7 && calls == 1);
    assert(position == 7 && !locked && !memcmp(output + 1, input, 7));
    reset(sizeof input, sizeof input);
    assert(!gex_file_read(4, output + 1, 8192) && !locked);
    reset(0, 0);
    assert(!gex_file_read(4, output + 1, 8192) && !locked);
    reset(0, sizeof input);
    seekable = 0;
    errno = EAGAIN;
    assert(gex_file_read(4, output + 1, 8192) == 8192);
    assert(calls == 1 && !dma_calls && errno == EAGAIN && !locked);
    assert(!memcmp(output + 1, input, 8192));
    reset(0, sizeof input);
    seekable = 0;
    fail_call = 1;
    assert(gex_file_read(4, output + 1, 8192) == -1 && errno == EIO);
    return 0;
}
""")
            toolchain = Path(os.environ.get("GEX_PCRECOMP_DIR", ROOT / "pcrecomp"))
            command = [
                os.environ.get("GEX_TEST_CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE",
                "-O2", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Dread=mock_read", "-Dclose=mock_close", "-Ddup=mock_dup",
                "-I", str(work), "-I", str(ROOT / "src"),
                "-I", str(toolchain / "runtime/win32hle"),
                "-I", str(toolchain / "runtime/recomp32"),
                str(work / "test.c"), str(ROOT / "src/platform/dreamcast/filesystem.c"),
                "-o", str(work / "filesystem-test"),
            ]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(work / "filesystem-test")],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
