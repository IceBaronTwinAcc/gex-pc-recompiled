#ifndef GEX_GAME_INSTALL_H
#define GEX_GAME_INSTALL_H

#include <stddef.h>

/* Returns 0 on success, 1 on cancellation, -1 on failure. */
int gex_install_game(const char *source, const char *destination, char *error, size_t size);
int gex_prepare_game(const char *destination, int interactive);
int gex_default_game_folder(char *path, size_t size);
int gex_pick_game_folder(char *path, size_t size, char *error, size_t error_size);
void gex_setup_error(const char *message);

#endif
