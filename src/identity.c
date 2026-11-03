#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "identity.h"
#include "sha256.h"
#include "platform/filesystem.h"

int gex_verify_executable(const char *path) {
    int file = gex_file_open(path, GEX_FILE_READ);
    if (file < 0) {
        fprintf(stderr, "[gex] cannot read executable %s\n", path);
        return 1;
    }
    gex_sha256 hash;
    gex_sha256_init(&hash);
    unsigned char buffer[8192];
    int failed = 0;
    for (;;) {
        int64_t count = gex_file_read(file, buffer, sizeof buffer);
        if (count < 0) {
            if (gex_fs_error() == GEX_IO_INTERRUPTED) continue;
            failed = 1;
            break;
        }
        if (!count) break;
        gex_sha256_update(&hash, buffer, (size_t)count);
    }
    if (gex_file_close(file)) failed = 1;
    if (failed) {
        fprintf(stderr, "[gex] cannot hash executable %s\n", path);
        return 1;
    }
    unsigned char digest[32];
    gex_sha256_final(&hash, digest);
    return gex_verify_executable_digest(digest);
}

int gex_verify_executable_digest(const unsigned char digest[32]) {
    static const char expected[] = "e1fd63ecb09fca29d63286e22447752dcb120d7e682f8759d1021776f7698b86";
    char actual[65];
    for (unsigned i = 0; i < 32; ++i)
        snprintf(actual + 2 * i, 3, "%02x", digest[i]);
    if (strcmp(actual, expected)) {
        fprintf(stderr, "[gex] unsupported executable SHA-256 %s; expected %s\n", actual, expected);
        return 1;
    }
    return 0;
}
