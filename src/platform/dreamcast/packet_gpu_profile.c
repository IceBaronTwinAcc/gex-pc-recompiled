#include <kos.h>
#include "packet_gpu.h"


uint64_t gex_packet_gpu_profile_clock(uint64_t *cpu) {
    *cpu = thd_get_cpu_time(thd_get_current());
    return timer_us_gettime64();
}
