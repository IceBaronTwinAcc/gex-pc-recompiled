#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "platform/host.h"
#include "platform/filesystem.h"

int gex_host_prepare_playback(void) { return 0; }

int gex_host_settings_path(char *path, size_t size) {
#if defined(__APPLE__) || defined(__ANDROID__)
    const char *root = getenv("XDG_DATA_HOME");
    if (root && *root) {
        int length = snprintf(path, size, "%s/gex-recomp/Gex/settings.reg", root);
        if (length < 0 || (size_t)length >= size) {
            fprintf(stderr, "[gex] settings path is too long\n");
            return -1;
        }
        for (char *separator = path + 1; *separator; ++separator) {
            if (*separator != '/') continue;
            *separator = '\0';
            int failed = gex_directory_create(path) && gex_fs_error() != GEX_IO_EXISTS;
            if (failed)
                fprintf(stderr, "[gex] cannot create settings directory %s: %s\n",
                        path, gex_fs_error_text());
            *separator = '/';
            if (failed) return -1;
        }
        return 0;
    }
#endif
    char *preferences = SDL_GetPrefPath("gex-recomp", "Gex");
    if (!preferences) {
        fprintf(stderr, "[gex] cannot locate writable settings directory: %s\n", SDL_GetError());
        return -1;
    }
    int length = snprintf(path, size, "%ssettings.reg", preferences);
    SDL_free(preferences);
    if (length < 0 || (size_t)length >= size) {
        fprintf(stderr, "[gex] settings path is too long\n");
        return -1;
    }
    return 0;
}
