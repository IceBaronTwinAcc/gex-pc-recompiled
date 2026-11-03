#ifndef GEX_SAVE_STORE_H
#define GEX_SAVE_STORE_H

#include <stddef.h>

enum { GEX_SAVE_SLOTS = 3, GEX_PASSWORD_SIZE = 9 };

typedef struct {
    char password[GEX_SAVE_SLOTS][GEX_PASSWORD_SIZE];
    char path[4096];
    char error[256];
    int available;
} gex_save_store;

/* Passwords must be exactly eight retail alphabet characters plus a NUL. */
int gex_password_valid(const char password[GEX_PASSWORD_SIZE]);
unsigned gex_password_levels(const char password[GEX_PASSWORD_SIZE]);
int gex_save_store_open(gex_save_store *store, const char *path);
int gex_save_store_write(gex_save_store *store, unsigned slot,
                         const char password[GEX_PASSWORD_SIZE]);
int gex_save_store_copy(gex_save_store *store, unsigned source, unsigned destination);
int gex_save_store_erase(gex_save_store *store, unsigned slot);

#endif
