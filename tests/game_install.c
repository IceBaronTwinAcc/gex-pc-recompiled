#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "game_install.h"

static int picks;
void hle_fatal(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    abort();
}
int gex_pick_game_folder(char *path, size_t size, char *error, size_t error_size) {
    (void)error; (void)error_size;
    ++picks;
    const char *selected = getenv("GEX_TEST_PICK");
    if (picks > 1 || !selected) return 1;
    if (snprintf(path, size, "%s", selected) >= (int)size) return -1;
    return 0;
}
void gex_setup_error(const char *error) { fprintf(stderr, "dialog: %s\n", error); }

int main(int argc, char **argv) {
    char error[1024] = "";
    int result;
    if (argc == 4 && !strcmp(argv[1], "install"))
        result = gex_install_game(argv[2], argv[3], error, sizeof error);
    else if (argc == 4 && !strcmp(argv[1], "prepare"))
        result = gex_prepare_game(argv[2], atoi(argv[3]));
    else return 64;
    if (result < 0) fprintf(stderr, "%s\n", error);
    fprintf(stderr, "picker-calls=%d\n", picks);
    return result < 0 ? 2 : result;
}
