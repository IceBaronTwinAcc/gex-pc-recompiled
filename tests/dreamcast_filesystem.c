#include <kos.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/filesystem.h"

void hle_fatal(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    abort();
}

char *gex_string_duplicate(const char *text) { return strdup(text); }
void hle_block_begin(void) {}
void hle_block_end(void) {}

static void compare(int cached, int reference, size_t bytes) {
    uint8_t actual[8193], expected[8193];
    assert(bytes <= sizeof actual);
    int64_t position = gex_file_seek(cached, 0, SEEK_CUR);
    assert(position >= 0 && lseek(reference, (off_t)position, SEEK_SET) == position);
    ssize_t count = read(reference, expected, bytes);
    assert(count >= 0 && gex_file_read(cached, actual, bytes) == count);
    assert(!memcmp(actual, expected, (size_t)count));
    assert(gex_file_seek(cached, 0, SEEK_CUR) == position + count);
}

int main(void) {
    const char *paths[] = {"/cd/Gex/GEX.exe", "/cd/Gex/Readme.txt"};
    for (unsigned pass = 0; pass < 3; ++pass) {
        int cached = gex_file_open(paths[pass % 2], GEX_FILE_READ);
        int reference = open(paths[pass % 2], O_RDONLY);
        assert(cached >= 0 && reference >= 0);
        int64_t end = gex_file_seek(cached, 0, SEEK_END);
        assert(end > 0 && gex_file_seek(cached, 0, SEEK_SET) == 0);
        for (unsigned i = 0; i < 10; ++i) compare(cached, reference, 8193);
        int64_t offsets[] = {0, 1, 2039, 65529, end - 23, end, end + 17};
        for (unsigned i = 0; i < sizeof offsets / sizeof *offsets; ++i) {
            int64_t expected = lseek(reference, (off_t)offsets[i], SEEK_SET);
            assert(expected >= 0 && gex_file_seek(cached, offsets[i], SEEK_SET) == expected);
            compare(cached, reference, 257);
        }
        assert(gex_file_seek(cached, 2039, SEEK_SET) == 2039);
        compare(cached, reference, 0);
        int duplicate = gex_file_duplicate(cached);
        assert(duplicate >= 0);
        compare(duplicate, reference, 257);
        compare(cached, reference, 257);
        assert(!gex_file_close(duplicate));
        assert(!gex_file_close(cached) && !close(reference));
    }
    int ram = gex_file_open("/ram/gex-fs-test", GEX_FILE_READ | GEX_FILE_WRITE | GEX_FILE_CREATE);
    assert(ram >= 0);
    uint8_t actual[4], first[] = {1, 2, 3, 4}, second[] = {5, 6, 7, 8};
    assert(gex_file_write(ram, first, 4) == 4 && gex_file_seek(ram, 0, SEEK_SET) == 0);
    assert(gex_file_read(ram, actual, 4) == 4 && !memcmp(first, actual, 4));
    assert(gex_file_seek(ram, 0, SEEK_SET) == 0 && gex_file_write(ram, second, 4) == 4);
    assert(gex_file_seek(ram, 0, SEEK_SET) == 0);
    assert(gex_file_read(ram, actual, 4) == 4 && !memcmp(second, actual, 4));
    assert(!gex_file_close(ram) && !gex_file_remove("/ram/gex-fs-test"));
    assert(gex_file_read(-1, actual, 4) == -1);
    puts("Dreamcast filesystem regression: PASS");
    return 0;
}
