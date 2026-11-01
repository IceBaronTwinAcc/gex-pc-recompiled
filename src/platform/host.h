#ifndef GEX_PLATFORM_HOST_H
#define GEX_PLATFORM_HOST_H

#include <stddef.h>
#include <stdint.h>

/* All successful allocations handed to the guest must have a valid guest VA. */
void *gex_host_guest_alloc(uint32_t bytes, uint32_t *address);
void gex_host_guest_free(void *memory);

uint32_t gex_host_ticks_ms(void);
void gex_host_delay(uint32_t milliseconds);
int gex_host_screensaver(void);
void gex_host_set_screensaver(int enabled);
int gex_host_enter_directory(const char *path);
int gex_host_working_directory(char *path, size_t size);
/* 0: writable persistent path; 1: explicitly session-only; -1: host error. */
int gex_host_settings_path(char *path, size_t size);
void gex_host_install_diagnostics(void);
int gex_host_screenshot(const char *path, const uint8_t *pixels,
                        int width, int height, int pitch);

typedef struct {
    int16_t *samples;
    int frames, channels, rate;
} gex_host_wave;

/* Exactly one of path/data must be non-NULL. Samples are freed with free(). */
int gex_host_load_wave(const char *path, const void *data, size_t bytes,
                       gex_host_wave *wave);

#endif
