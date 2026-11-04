#ifndef GEX_PREFERENCES_H
#define GEX_PREFERENCES_H

#include <stddef.h>
#include <stdint.h>
#include "platform/input.h"

enum gex_action {
    GEX_ACTION_LASH, GEX_ACTION_HURL, GEX_ACTION_WHIP, GEX_ACTION_JUMP,
    GEX_ACTION_RUN, GEX_ACTION_PAUSE, GEX_ACTION_CONFIRM, GEX_ACTION_BACK,
    GEX_ACTION_COUNT
};
enum gex_graphics_api {
    GEX_API_AUTO, GEX_API_VULKAN, GEX_API_METAL, GEX_API_OPENGL,
    GEX_API_OPENGLES, GEX_API_D3D11, GEX_API_D3D12, GEX_API_COUNT
};
enum gex_display_mode { GEX_DISPLAY_WINDOWED, GEX_DISPLAY_BORDERLESS, GEX_DISPLAY_EXCLUSIVE };
typedef struct { uint32_t key, scan, button; } gex_binding;
typedef struct {
    int bindings_active;
    gex_binding bindings[GEX_ACTION_COUNT];
    unsigned volume[3];
    int renderer, api, display, width, height, triple;
    int active_renderer, active_api;
    uint32_t supported_apis;
} gex_preferences;

extern gex_preferences gex_prefs;
extern int (*gex_controls_menu_hook)(void);
extern int (*gex_binding_key_filter)(int down, uint32_t key, uint32_t scan);
extern int (*gex_binding_pad_filter)(uint32_t buttons);

void gex_preferences_reset(void);
int gex_controls_menu(void);
uint32_t gex_controls_keyboard(uint32_t key);
uint32_t gex_controls_gamepad(uint32_t buttons);
int gex_controls_bind(unsigned action, int gamepad, uint32_t key, uint32_t scan);
void gex_controls_key_name(uint32_t key, char *out, size_t size);
const char *gex_controls_button_name(uint32_t button);
const char *gex_api_name(int api);
const char *gex_api_label(int api);
int gex_renderer_available(int renderer);
int gex_preferences_default_size(int display_width, int display_height, int *width, int *height);

#endif
