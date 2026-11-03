#ifndef GEX_OPTIONS_MENU_H
#define GEX_OPTIONS_MENU_H

#include <stdint.h>

enum gex_options_platform {
    GEX_OPTIONS_WINDOWS = 1u << 0,
    GEX_OPTIONS_LINUX = 1u << 1,
    GEX_OPTIONS_MACOS = 1u << 2,
    GEX_OPTIONS_ANDROID = 1u << 3,
    GEX_OPTIONS_DREAMCAST = 1u << 4,
    GEX_OPTIONS_ALL = (1u << 5) - 1
};

uint32_t gex_options_platform(void);
int gex_options_platform_matches(uint32_t platforms);
void gex_options_title_init(void);
int gex_options_scaled_text(void);
void gex_options_begin(void);
int gex_options_key(uint32_t key);
int gex_options_pointer(int x, int y);
void gex_options_render(void);
void gex_options_end(void);
void gex_save_options(void);

#endif
