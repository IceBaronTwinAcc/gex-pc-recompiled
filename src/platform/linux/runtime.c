#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include "platform/runtime.h"
#include "win32hle.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static pthread_mutex_t *native_mutex(gex_mutex *mutex) {
    void *implementation = atomic_load(&mutex->implementation);
    if (!implementation) {
        pthread_mutex_t *created = malloc(sizeof *created);
        if (!created) hle_fatal("cannot allocate host mutex");
        int error = pthread_mutex_init(created, NULL);
        if (error) hle_fatal("mutex initialization: %s", strerror(error));
        if (!atomic_compare_exchange_strong(&mutex->implementation, &implementation, created)) {
            error = pthread_mutex_destroy(created);
            if (error) hle_fatal("unused mutex cleanup: %s", strerror(error));
            free(created);
        } else implementation = created;
    }
    return implementation;
}

void gex_mutex_lock(gex_mutex *mutex) {
    int error = pthread_mutex_lock(native_mutex(mutex));
    if (error) hle_fatal("host mutex lock: %s", strerror(error));
}

void gex_mutex_unlock(gex_mutex *mutex) {
    int error = pthread_mutex_unlock(native_mutex(mutex));
    if (error) hle_fatal("host mutex unlock: %s", strerror(error));
}

_Noreturn void gex_runtime_exit_thread(void) { pthread_exit(NULL); }
_Noreturn void gex_runtime_exit_process(int code) {
#ifdef __EMSCRIPTEN__
    exit(code);
#else
    _exit(code);
#endif
}
uint32_t gex_runtime_process_id(void) { return (uint32_t)getpid(); }

int gex_runtime_executable_path(char *out, size_t size) {
#ifdef __EMSCRIPTEN__
    const char *path = "/Gex/GEX.exe";
    if (strlen(path) >= size) return 0;
    strcpy(out, path);
#elif defined(__APPLE__)
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

void gex_runtime_realtime(int64_t *seconds, long *nanoseconds) {
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value))
        hle_fatal("wall clock: %s", strerror(errno));
    *seconds = value.tv_sec;
    *nanoseconds = value.tv_nsec;
}

void gex_runtime_calendar(int64_t seconds, int local, struct tm *calendar) {
    time_t value = (time_t)seconds;
    if ((int64_t)value != seconds) hle_fatal("calendar timestamp exceeds host time range");
    if (!(local ? localtime_r(&value, calendar) : gmtime_r(&value, calendar)))
        hle_fatal("calendar conversion failed for %lld", (long long)seconds);
}

int64_t gex_runtime_utc_seconds(struct tm *calendar) {
    errno = 0;
    time_t seconds = timegm(calendar);
    if (seconds == (time_t)-1 && errno)
        hle_fatal("UTC conversion: %s", strerror(errno));
    return (int64_t)seconds;
}
int32_t gex_runtime_timezone_bias(void) {
    time_t now = time(NULL);
    struct tm calendar;
    gex_runtime_calendar(now, 1, &calendar);
    return (int32_t)calendar.tm_gmtoff;
}

static pthread_mutex_t machine = PTHREAD_MUTEX_INITIALIZER;

void gex_runtime_machine_lock(void) {
    int error = pthread_mutex_lock(&machine);
    if (error) hle_fatal("machine lock: %s", strerror(error));
}

void gex_runtime_machine_unlock(void) {
    int error = pthread_mutex_unlock(&machine);
    if (error) hle_fatal("machine unlock: %s", strerror(error));
}

void gex_runtime_yield(void) {
#ifdef __EMSCRIPTEN__
    gex_runtime_sleep_us(50);
#else
    if (sched_yield()) hle_fatal("machine yield: %s", strerror(errno));
#endif
}

unsigned gex_runtime_spin_limit(void) {
#ifdef __EMSCRIPTEN__
    return 200;
#else
    return 1000;
#endif
}

uint64_t gex_runtime_time_us(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time))
        hle_fatal("monotonic clock: %s", strerror(errno));
    return (uint64_t)time.tv_sec * 1000000u + (uint64_t)time.tv_nsec / 1000u;
}

void gex_runtime_sleep_us(uint64_t microseconds) {
    struct timespec request = {
        .tv_sec = (time_t)(microseconds / 1000000u),
        .tv_nsec = (long)(microseconds % 1000000u) * 1000L
    };
    while (nanosleep(&request, &request)) {
        if (errno != EINTR) hle_fatal("runtime sleep: %s", strerror(errno));
    }
}

int gex_runtime_start_thread(void *(*entry)(void *), void *arg) {
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error) {
        fprintf(stderr, "[gex] worker attributes: %s\n", strerror(error));
        return -1;
    }
    error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (!error) {
        pthread_t thread;
        error = pthread_create(&thread, &attributes, entry, arg);
    }
    int destroy_error = pthread_attr_destroy(&attributes);
    if (destroy_error) hle_fatal("worker attribute cleanup: %s", strerror(destroy_error));
    if (error) fprintf(stderr, "[gex] cannot create worker: %s\n", strerror(error));
    return error ? -1 : 0;
}
