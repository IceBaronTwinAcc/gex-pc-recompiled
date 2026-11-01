#ifndef GEX_SHA256_H
#define GEX_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
} gex_sha256;

void gex_sha256_init(gex_sha256 *hash);
void gex_sha256_update(gex_sha256 *hash, const void *data, size_t size);
void gex_sha256_final(gex_sha256 *hash, uint8_t digest[32]);

#endif
