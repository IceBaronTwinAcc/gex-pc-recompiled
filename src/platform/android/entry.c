#include <android/log.h>
#include <stdio.h>
#include <unistd.h>
#include <SDL2/SDL.h>

int gex_android_main(int argc, char **argv);

int SDL_main(int argc, char **argv) {
    const char *directory = SDL_AndroidGetInternalStoragePath();
    char path[4096];
    int length = directory ? snprintf(path, sizeof path, "%s/last-run.log", directory) : -1;
    if (length < 0 || (size_t)length >= sizeof path) {
        __android_log_print(ANDROID_LOG_ERROR, "Gex", "Cannot locate application log directory");
        return 1;
    }
    FILE *log = fopen(path, "w");
    if (!log || dup2(fileno(log), STDERR_FILENO) < 0 || dup2(fileno(log), STDOUT_FILENO) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Gex", "Cannot open application log: %s", path);
        if (log) fclose(log);
        return 1;
    }
    fclose(log);
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    int result = gex_android_main(argc, argv);
    fprintf(stderr, "[android] game returned %d\n", result);
    return result;
}
