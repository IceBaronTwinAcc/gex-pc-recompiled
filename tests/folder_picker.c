#include <stdio.h>
#include "game_install.h"

int main(void) {
    char path[4096], error[1024];
    int result = gex_pick_game_folder(path, sizeof path, error, sizeof error);
    if (!result) printf("%s\n", path);
    else if (result < 0) fprintf(stderr, "%s\n", error);
    return result < 0 ? 2 : result;
}
