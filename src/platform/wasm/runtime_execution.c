#include <stdlib.h>
#include <string.h>
#include "platform/runtime.h"

_Noreturn void gex_runtime_exit_process(int code) { exit(code); }

int gex_runtime_executable_path(char *out, size_t size) {
    const char *path = "/Gex/GEX.exe";
    if (strlen(path) >= size) return 0;
    strcpy(out, path);
    return 1;
}

void gex_runtime_yield(void) { gex_runtime_sleep_us(50); }
unsigned gex_runtime_spin_limit(void) { return 200; }
