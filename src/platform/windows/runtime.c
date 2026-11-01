#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "platform/runtime.h"
#include "win32hle.h"

static SRWLOCK *native_mutex(gex_mutex *mutex) {
    void *implementation = atomic_load(&mutex->implementation);
    if (!implementation) {
        SRWLOCK *created = malloc(sizeof *created);
        if (!created) hle_fatal("cannot allocate host mutex");
        InitializeSRWLock(created);
        if (!atomic_compare_exchange_strong(&mutex->implementation, &implementation, created))
            free(created);
        else implementation = created;
    }
    return implementation;
}
void gex_mutex_lock(gex_mutex *mutex) { AcquireSRWLockExclusive(native_mutex(mutex)); }
void gex_mutex_unlock(gex_mutex *mutex) { ReleaseSRWLockExclusive(native_mutex(mutex)); }
static SRWLOCK machine = SRWLOCK_INIT;
void gex_runtime_machine_lock(void) { AcquireSRWLockExclusive(&machine); }
void gex_runtime_machine_unlock(void) { ReleaseSRWLockExclusive(&machine); }
_Noreturn void gex_runtime_exit_thread(void) { _endthreadex(0); abort(); }
_Noreturn void gex_runtime_exit_process(int code) { ExitProcess((UINT)code); }
uint32_t gex_runtime_process_id(void) { return GetCurrentProcessId(); }
int gex_runtime_executable_path(char *path, size_t size) {
    if (!size || size > MAXDWORD) return 0;
    DWORD length = GetModuleFileNameA(NULL, path, (DWORD)size);
    if (!length || length >= size) return 0;
    for (char *p = path; *p; ++p) if (*p == '\\') *p = '/';
    return 1;
}
void gex_runtime_realtime(int64_t *seconds, long *nanoseconds) {
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    uint64_t ticks = ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
    *seconds = (int64_t)(ticks / 10000000) - 11644473600LL;
    *nanoseconds = (long)(ticks % 10000000) * 100;
}
void gex_runtime_calendar(int64_t seconds, int local, struct tm *calendar) {
    __time64_t value = seconds;
    if (local ? _localtime64_s(calendar, &value) : _gmtime64_s(calendar, &value))
        hle_fatal("calendar conversion failed for %lld", (long long)seconds);
}
int64_t gex_runtime_utc_seconds(struct tm *calendar) {
    errno = 0;
    __time64_t seconds = _mkgmtime64(calendar);
    if (seconds == -1 && errno) hle_fatal("UTC conversion failed");
    return seconds;
}
int32_t gex_runtime_timezone_bias(void) {
    TIME_ZONE_INFORMATION zone;
    DWORD mode = GetTimeZoneInformation(&zone);
    if (mode == TIME_ZONE_ID_INVALID) hle_fatal("cannot determine host time zone");
    LONG bias = zone.Bias;
    if (mode == TIME_ZONE_ID_DAYLIGHT) bias += zone.DaylightBias;
    else if (mode == TIME_ZONE_ID_STANDARD) bias += zone.StandardBias;
    return -bias * 60;
}
void gex_runtime_yield(void) { SwitchToThread(); }
unsigned gex_runtime_spin_limit(void) { return 1000; }
uint64_t gex_runtime_time_us(void) {
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter))
        hle_fatal("cannot read host monotonic clock");
    return (uint64_t)(counter.QuadPart / frequency.QuadPart) * 1000000 +
        (uint64_t)(counter.QuadPart % frequency.QuadPart) * 1000000 / frequency.QuadPart;
}
void gex_runtime_sleep_us(uint64_t microseconds) {
    uint64_t milliseconds = microseconds / 1000 + (microseconds % 1000 != 0);
    while (milliseconds > MAXDWORD - 1) {
        Sleep(MAXDWORD - 1);
        milliseconds -= MAXDWORD - 1;
    }
    Sleep((DWORD)milliseconds);
}
typedef struct { void *(*entry)(void *); void *arg; } worker;
static unsigned __stdcall start_worker(void *arg) {
    worker value = *(worker *)arg;
    free(arg);
    value.entry(value.arg);
    return 0;
}
int gex_runtime_start_thread(void *(*entry)(void *), void *arg) {
    worker *value = malloc(sizeof *value);
    if (!value) {
        fprintf(stderr, "[gex] cannot allocate worker\n");
        return -1;
    }
    *value = (worker){entry, arg};
    uintptr_t thread = _beginthreadex(NULL, 8u << 20, start_worker, value, 0, NULL);
    if (!thread) {
        fprintf(stderr, "[gex] cannot start worker: %s\n", strerror(errno));
        free(value);
        return -1;
    }
    CloseHandle((HANDLE)thread);
    return 0;
}
