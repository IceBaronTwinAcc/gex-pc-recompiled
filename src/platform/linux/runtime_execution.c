#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/runtime.h"
#include "win32hle.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

_Noreturn void gex_runtime_exit_process(int code) { _exit(code); }

int gex_runtime_executable_path(char *out, size_t size) {
#ifdef __APPLE__
    char raw[4096], real[4096];
    uint32_t length = sizeof raw;
    if (!size || _NSGetExecutablePath(raw, &length) || !realpath(raw, real) || strlen(real) >= size)
        return 0;
    strcpy(out, real);
#else
    ssize_t length = size ? readlink("/proc/self/exe", out, size - 1) : -1;
    if (length <= 0 || (size_t)length == size - 1) return 0;
    out[length] = 0;
#endif
    return 1;
}

void gex_runtime_yield(void) {
    if (sched_yield()) hle_fatal("machine yield: %s", strerror(errno));
}

unsigned gex_runtime_spin_limit(void) { return 1000; }
