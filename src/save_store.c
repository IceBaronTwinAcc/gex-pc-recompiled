#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "save_store.h"
#include "platform/filesystem.h"

static const char alphabet[] = "BCDFGHKLPRSTVXYZ";

static unsigned field(const uint8_t bytes[3], unsigned offset, unsigned count) {
    unsigned value = 0, used = 0;
    while (count) {
        unsigned available = 8 - offset % 8;
        unsigned take = count < available ? count : available;
        value |= ((bytes[offset / 8] >> (available - take)) & ((1u << take) - 1)) << used;
        offset += take;
        count -= take;
        used += take;
    }
    return value;
}

static int payload(const char password[GEX_PASSWORD_SIZE], uint8_t bytes[3]) {
    unsigned sum = 0, nibbles[8];
    if (password[8]) return 0;
    for (unsigned i = 0; i < 8; ++i) {
        const char *letter = password[i] ? strchr(alphabet, password[i]) : NULL;
        if (!letter) return 0;
        nibbles[i] = (unsigned)(letter - alphabet);
        if (i >= 2) sum += (unsigned char)password[i];
    }
    if (password[0] != alphabet[sum & 15] || password[1] != alphabet[(sum >> 4) & 15])
        return 0;
    /* Retail XOR table at 0x45ABA8; low nibble is written first. */
    const uint8_t masks[] = {0x3C, 0x56, 0x84};
    for (unsigned i = 0; i < 3; ++i)
        bytes[i] = (uint8_t)((nibbles[2 + i * 2] | nibbles[3 + i * 2] << 4) ^ masks[i]);
    /* Bound every retail table lookup before invoking its permissive decoder. */
    return field(bytes, 0, 3) < 7 && field(bytes, 3, 5) <= 19 &&
        field(bytes, 8, 4) <= 8 && field(bytes, 21, 2) <= 2 && !(bytes[2] & 1);
}

int gex_password_valid(const char password[GEX_PASSWORD_SIZE]) {
    uint8_t bytes[3];
    return payload(password, bytes);
}

unsigned gex_password_levels(const char password[GEX_PASSWORD_SIZE]) {
    uint8_t bytes[3];
    return payload(password, bytes) ? field(bytes, 3, 5) : 0;
}

static uint32_t checksum(const uint8_t *bytes, size_t count) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static int failure(gex_save_store *store, const char *message) {
    snprintf(store->error, sizeof store->error, "%s", message);
    fprintf(stderr, "[saves] %s: %s\n", store->path, store->error);
    return -1;
}

static int io_failure(gex_save_store *store, const char *operation) {
    char message[256];
    snprintf(message, sizeof message, "%s: %s", operation, strerror(errno));
    return failure(store, message);
}

int gex_save_store_open(gex_save_store *store, const char *path) {
    memset(store, 0, sizeof *store);
    if (strlen(path) >= sizeof store->path) return failure(store, "save path is too long");
    strcpy(store->path, path);
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (errno != ENOENT) return io_failure(store, "cannot read saves");
        store->available = 1;
        return 0;
    }
    uint8_t data[39];
    size_t count = fread(data, 1, sizeof data, file);
    int extra = fgetc(file), bad = ferror(file);
    if (fclose(file)) bad = 1;
    if (bad) return io_failure(store, "cannot read saves");
    if (count != sizeof data || extra != EOF || memcmp(data, "GEXDECK1", 8))
        return failure(store, "invalid save file; original file left untouched");
    uint32_t expected = 0;
    for (unsigned i = 0; i < 4; ++i) expected |= (uint32_t)data[35 + i] << (8 * i);
    if (checksum(data, 35) != expected)
        return failure(store, "save checksum mismatch; original file left untouched");
    for (unsigned i = 0; i < GEX_SAVE_SLOTS; ++i) {
        const char *password = (const char *)data + 8 + i * GEX_PASSWORD_SIZE;
        if (password[0]) {
            if (!gex_password_valid(password)) return failure(store, "invalid saved password");
        } else {
            for (unsigned j = 1; j < GEX_PASSWORD_SIZE; ++j)
                if (password[j]) return failure(store, "invalid empty save slot");
        }
    }
    memcpy(store->password, data + 8, sizeof store->password);
    store->available = 1;
    return 0;
}

static int commit(gex_save_store *store, const void *passwords) {
    if (!store->available) return failure(store, "save store unavailable; original file left untouched");
    uint8_t data[39] = {0};
    memcpy(data, "GEXDECK1", 8);
    memcpy(data + 8, passwords, sizeof store->password);
    uint32_t crc = checksum(data, 35);
    for (unsigned i = 0; i < 4; ++i) data[35 + i] = (uint8_t)(crc >> (8 * i));
    char *temporary;
    FILE *file = gex_fs_replace_begin(store->path, &temporary);
    if (!file) return io_failure(store, "cannot create save");
    int bad = fwrite(data, 1, sizeof data, file) != sizeof data;
    int committed = gex_fs_replace_commit(file, temporary, store->path);
    free(temporary);
    if (bad || committed) return io_failure(store, "cannot commit save");
    memcpy(store->password, data + 8, sizeof store->password);
    store->error[0] = 0;
    return 0;
}

int gex_save_store_write(gex_save_store *store, unsigned slot,
                         const char password[GEX_PASSWORD_SIZE]) {
    if (slot >= GEX_SAVE_SLOTS) return failure(store, "invalid save slot");
    if (!gex_password_valid(password)) return failure(store, "invalid checkpoint password");
    char passwords[GEX_SAVE_SLOTS][GEX_PASSWORD_SIZE];
    memcpy(passwords, store->password, sizeof passwords);
    memcpy(passwords[slot], password, GEX_PASSWORD_SIZE);
    return commit(store, passwords);
}

int gex_save_store_copy(gex_save_store *store, unsigned source, unsigned destination) {
    if (source >= GEX_SAVE_SLOTS || destination >= GEX_SAVE_SLOTS || source == destination)
        return failure(store, "invalid copy destination");
    if (!store->password[source][0]) return failure(store, "cannot copy an empty deck");
    return gex_save_store_write(store, destination, store->password[source]);
}

int gex_save_store_erase(gex_save_store *store, unsigned slot) {
    if (slot >= GEX_SAVE_SLOTS) return failure(store, "invalid save slot");
    char passwords[GEX_SAVE_SLOTS][GEX_PASSWORD_SIZE];
    memcpy(passwords, store->password, sizeof passwords);
    memset(passwords[slot], 0, GEX_PASSWORD_SIZE);
    return commit(store, passwords);
}
