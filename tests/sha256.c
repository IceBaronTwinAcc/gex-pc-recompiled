#include <stdio.h>
#include <string.h>
#include "sha256.h"

static int check(const char *text, size_t size, unsigned repeats, const char *expected) {
    gex_sha256 hash;
    gex_sha256_init(&hash);
    for (unsigned i = 0; i < repeats; ++i)
        gex_sha256_update(&hash, text, size);
    uint8_t digest[32];
    gex_sha256_final(&hash, digest);
    char actual[65];
    for (unsigned i = 0; i < 32; ++i)
        snprintf(actual + i * 2, 3, "%02x", digest[i]);
    if (!strcmp(actual, expected)) return 0;
    fprintf(stderr, "SHA-256 mismatch: %s != %s\n", actual, expected);
    return 1;
}

int main(void) {
    int failures = 0;
    failures += check("", 0, 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    failures += check("abc", 3, 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    failures += check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, 1,
                      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    failures += check("a", 1, 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    return failures ? 1 : 0;
}
