#include <kos.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "win32hle.h"
#include "platform/runtime.h"

static mutex_t machine = MUTEX_INITIALIZER;
static mutex_t *native_mutex(gex_mutex *mutex) {
    void *value = atomic_load(&mutex->implementation);
    if (!value) {
        mutex_t *created = malloc(sizeof *created);
        if (!created || mutex_init(created, MUTEX_TYPE_NORMAL))
            hle_fatal("KOS mutex initialization failed");
        if (!atomic_compare_exchange_strong(&mutex->implementation, &value, created)) {
            mutex_destroy(created);
            free(created);
        } else value = created;
    }
    return value;
}
void gex_mutex_lock(gex_mutex *mutex) {
    if (mutex_lock(native_mutex(mutex))) hle_fatal("KOS mutex lock failed");
}
void gex_mutex_unlock(gex_mutex *mutex) {
    if (mutex_unlock(native_mutex(mutex))) hle_fatal("KOS mutex unlock failed");
}
void gex_runtime_machine_lock(void) {
    if (mutex_lock(&machine)) hle_fatal("KOS machine lock failed");
}
void gex_runtime_machine_unlock(void) {
    if (mutex_unlock(&machine)) hle_fatal("KOS machine unlock failed");
}
void gex_runtime_yield(void) {
    thd_pass();
}
unsigned gex_runtime_spin_limit(void) { return 200; }
uint64_t gex_runtime_time_us(void) { return timer_us_gettime64(); }
void gex_runtime_sleep_us(uint64_t us) {
    if (us) thd_sleep((unsigned)((us + 999) / 1000));
    else thd_pass();
}
int gex_runtime_start_thread(void *(*entry)(void *), void *arg) {
    kthread_attr_t attr = {.create_detached = true, .stack_size = 64 * 1024,
                          .label = "gex-worker"};
    if (thd_create_ex(&attr, entry, arg)) return 0;
    fprintf(stderr, "[dreamcast] cannot create worker: %s\n", strerror(errno));
    return -1;
}
_Noreturn void gex_runtime_exit_thread(void) { thd_exit(NULL); }
_Noreturn void gex_runtime_exit_process(int code) { exit(code); }
uint32_t gex_runtime_process_id(void) { return 1; }
int gex_runtime_executable_path(char *path, size_t size) {
    const char *name = "/cd/gex_recomp.elf";
    if (strlen(name) >= size) return 0;
    strcpy(path, name);
    return 1;
}
void gex_runtime_realtime(int64_t *seconds, long *nanoseconds) {
    *seconds = (int64_t)time(NULL);
    *nanoseconds = 0;
}
void gex_runtime_calendar(int64_t seconds, int local, struct tm *calendar) {
    time_t value = (time_t)seconds;
    if ((int64_t)value != seconds ||
        !(local ? localtime_r(&value, calendar) : gmtime_r(&value, calendar)))
        hle_fatal("KOS calendar conversion failed");
}
int64_t gex_runtime_utc_seconds(struct tm *calendar) {
    return (int64_t)mktime(calendar);
}
int32_t gex_runtime_timezone_bias(void) { return 0; }
