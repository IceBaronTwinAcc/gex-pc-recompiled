#include <stdio.h>
#include <string.h>
#include "preferences.h"
#include "renderer.h"

gex_preferences gex_prefs;
int (*gex_controls_menu_hook)(void);
int (*gex_binding_key_filter)(int, uint32_t, uint32_t);
int (*gex_binding_pad_filter)(uint32_t);
static const unsigned destinations[GEX_ACTION_COUNT] = {
    GEX_KEY_C, GEX_KEY_C, GEX_KEY_Z, GEX_KEY_X,
    GEX_KEY_V, GEX_KEY_ESCAPE, GEX_KEY_X, GEX_KEY_ESCAPE
};

void gex_preferences_reset(void) {
    gex_prefs = (gex_preferences){
        .bindings_active = 1,
        .bindings = {
            {'C', 0x2E, GEX_PAD_B}, {'C', 0x2E, GEX_PAD_B},
            {'Z', 0x2C, GEX_PAD_X}, {'X', 0x2D, GEX_PAD_A},
            {'V', 0x2F, GEX_PAD_Y}, {0x1B, 1, GEX_PAD_START},
            {'X', 0x2D, GEX_PAD_A}, {'C', 0x2E, GEX_PAD_B}
        },
        .volume = {100, 100, 100},
        .renderer = -1, .active_renderer = -1,
    };
}

int gex_controls_menu(void) { return gex_controls_menu_hook && gex_controls_menu_hook(); }

uint32_t gex_controls_keyboard(uint32_t key) {
    if (key >= 0x25 && key <= 0x28) {
        static const unsigned arrows[] = {GEX_KEY_LEFT, GEX_KEY_UP, GEX_KEY_RIGHT, GEX_KEY_DOWN};
        return 1u << arrows[key - 0x25];
    }
    int menu = gex_controls_menu();
    uint32_t result = 0;
    for (unsigned i = menu ? GEX_ACTION_CONFIRM : 0;
         i < (menu ? GEX_ACTION_COUNT : GEX_ACTION_CONFIRM); ++i)
        if (gex_prefs.bindings[i].key == key) result |= 1u << destinations[i];
    /* Keep an escape hatch if a user rebinds both menu actions. */
    if (menu && key == 0x1B) result |= 1u << GEX_KEY_ESCAPE;
    if (menu && (key == 0x0D || key == 0x20)) result |= 1u << GEX_KEY_X;
    return result;
}

uint32_t gex_controls_gamepad(uint32_t buttons) {
    int menu = gex_controls_menu();
    uint32_t result = 0;
    for (unsigned i = menu ? GEX_ACTION_CONFIRM : 0;
         i < (menu ? GEX_ACTION_COUNT : GEX_ACTION_CONFIRM); ++i)
        if (buttons & gex_prefs.bindings[i].button) result |= 1u << destinations[i];
    return result;
}

int gex_controls_bind(unsigned action, int gamepad, uint32_t key, uint32_t scan) {
    if (action >= GEX_ACTION_COUNT || !key ||
        (gamepad ? ((key & (key - 1)) || key < GEX_PAD_A || key > GEX_PAD_R) :
         (key >= 256 || scan > 255 || (key >= 0x25 && key <= 0x28) || key == 0x7A || key == 0x7B)))
        return -1;
    if (!gamepad && ((action == GEX_ACTION_CONFIRM && key == 0x1B) ||
        (action == GEX_ACTION_BACK && (key == 0x0D || key == 0x20))))
        return -1;
    int menu = action >= GEX_ACTION_CONFIRM;
    for (unsigned i = menu ? GEX_ACTION_CONFIRM : 0;
         i < (menu ? GEX_ACTION_COUNT : GEX_ACTION_CONFIRM); ++i)
        if (i != action && destinations[i] != destinations[action] &&
            (gamepad ? gex_prefs.bindings[i].button : gex_prefs.bindings[i].key) == key)
            return -1;
    if (gamepad) gex_prefs.bindings[action].button = key;
    else {
        gex_prefs.bindings[action].key = key;
        gex_prefs.bindings[action].scan = scan;
    }
    return 0;
}

void gex_controls_key_name(uint32_t key, char *out, size_t size) {
    const char *name = NULL;
    switch (key) {
    case 0x1B: name = "Esc"; break;
    case 0x0D: name = "Enter"; break;
    case 0x20: name = "Space"; break;
    case 0x09: name = "Tab"; break;
    case 0x08: name = "Bksp"; break;
    case 0x10: name = "Shift"; break;
    case 0x11: name = "Ctrl"; break;
    case 0x12: name = "Alt"; break;
    }
    if (name) snprintf(out, size, "%s", name);
    else if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        snprintf(out, size, "%c", (int)key);
    else if (key >= 0x70 && key <= 0x79) snprintf(out, size, "F%u", key - 0x6F);
    else snprintf(out, size, "%02X", key);
}

const char *gex_controls_button_name(uint32_t button) {
    static const char *const names[] = {"A", "B", "X", "Y", "Start", "Back", "L", "R"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i)
        if (button == (GEX_PAD_A << i)) return names[i];
    return "?";
}

const char *gex_api_name(int api) {
    static const char *const names[] = {"auto", "vulkan", "metal", "opengl", "opengles", "direct3d11", "direct3d12"};
    return api >= 0 && api < GEX_API_COUNT ? names[api] : NULL;
}
const char *gex_api_label(int api) {
    static const char *const names[] = {"Auto", "Vulkan", "Metal", "OpenGL", "OpenGL ES", "Direct3D 11", "Direct3D 12"};
    return api >= 0 && api < GEX_API_COUNT ? names[api] : NULL;
}
int gex_renderer_available(int renderer) {
    static const int available[] = {GEX_ENABLE_SOFTWARE_RENDERER, GEX_ENABLE_OPENGL_RENDERER,
        GEX_ENABLE_PVR_RENDERER, GEX_ENABLE_GLDC_RENDERER, GEX_ENABLE_BGFX_RENDERER};
    return renderer >= 0 && renderer < 5 && available[renderer];
}
int gex_preferences_default_size(int display_width, int display_height, int *width, int *height) {
    if (display_width <= 0 || display_height <= 0) return -1;
    int64_t scaled = ((int64_t)display_width * 240 + display_height / 2) / display_height;
    if (scaled < 1 || scaled > 8192) return -1;
    *width = (int)scaled;
    *height = 240;
    return 0;
}
