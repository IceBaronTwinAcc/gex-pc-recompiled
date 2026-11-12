#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <unistd.h>
#include "platform/runtime.h"

int gex_vm_map_file(void *address, size_t bytes, int file, uint64_t offset,
                    int writable, int copy) {
    (void)writable;
    (void)copy;
    for (size_t done = 0; done < bytes;) {
        ssize_t count = pread(file, (uint8_t *)address + done, bytes - done, (off_t)(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        done += (size_t)count;
    }
    return 0;
}
