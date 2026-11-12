#include <stdio.h>
#include "platform/host.h"
#include "platform/filesystem.h"

int gex_host_settings_path(char *path, size_t size) {
    const char *directory = "/save";
    if (gex_directory_create(directory) && gex_fs_error() != GEX_IO_EXISTS) {
        fprintf(stderr, "[gex] cannot create browser save directory: %s\n",
                gex_fs_error_text());
        return -1;
    }
    int length = snprintf(path, size, "%s/settings.reg", directory);
    if (length < 0 || (size_t)length >= size) {
        fprintf(stderr, "[gex] browser settings path is too long\n");
        return -1;
    }
    return 0;
}
