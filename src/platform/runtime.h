#ifndef GEX_PLATFORM_RUNTIME_H
#define GEX_PLATFORM_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>

typedef struct { _Atomic(void *) implementation; } gex_mutex;
#define GEX_MUTEX_INIT { NULL }
typedef struct { atomic_uint state; } gex_once;
#define GEX_ONCE_INIT { 0 }

void gex_mutex_lock(gex_mutex *mutex);
void gex_mutex_unlock(gex_mutex *mutex);
void gex_call_once(gex_once *once, void (*initialize)(void));
_Noreturn void gex_runtime_exit_thread(void);
_Noreturn void gex_runtime_exit_process(int code);
uint32_t gex_runtime_process_id(void);
int gex_runtime_executable_path(char *path, size_t size);
void gex_runtime_realtime(int64_t *seconds, long *nanoseconds);
void gex_runtime_calendar(int64_t seconds, int local, struct tm *calendar);
int64_t gex_runtime_utc_seconds(struct tm *calendar);
/* Seconds east of UTC, including the current daylight-saving offset. */
int32_t gex_runtime_timezone_bias(void);
char *gex_string_duplicate(const char *text);
int gex_string_compare_case(const char *left, const char *right);
int gex_string_compare_case_n(const char *left, const char *right, size_t count);

/* Reserve never replaces occupied host memory. Replace operates only inside
 * a reservation owned by this runtime. Neither operation executes guest code. */
void *gex_vm_reserve(void *hint, uint64_t bytes, int accessible, int fixed);
int gex_vm_replace(void *address, uint64_t bytes, int accessible);
void gex_vm_release(void *address, uint64_t bytes);
void gex_vm_discard(void *address, uint64_t bytes);
size_t gex_vm_page_size(void);
int gex_vm_map_file(void *address, size_t bytes, int file, uint64_t offset,
                    int writable, int copy);
uint32_t gex_flat_vm_reserve(uint32_t address, uint32_t bytes);
void gex_flat_vm_release(uint32_t address, uint32_t bytes);
void gex_flat_vm_zero(uint32_t address, uint32_t bytes);

/* One non-recursive lock protects the guest's global register file. */
void gex_runtime_machine_lock(void);
void gex_runtime_machine_unlock(void);
void gex_runtime_yield(void);
unsigned gex_runtime_spin_limit(void);

/* Monotonic, arbitrary epoch; unsigned millisecond arithmetic may wrap. */
uint64_t gex_runtime_time_us(void);
void gex_runtime_sleep_us(uint64_t microseconds);

/* Detached worker; returns zero on success, logs host failure otherwise.
 * The caller owns arg, which must remain live until entry has finished. */
int gex_runtime_start_thread(void *(*entry)(void *), void *arg);

#endif
