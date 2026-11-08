#include <stdio.h>
#include <string.h>
#include "host.h"
#include "win32hle.h"
#include "options_menu.h"
#include "preferences.h"
#include "renderer.h"
#include "graphics_guest.h"
#include "platform/video.h"
#if GEX_ENABLE_BGFX_RENDERER
#include "packet_gpu.h"
#endif

static const uint32_t actions[] = {0x45560C, 0x4555D0, 0x455594, 0x455648};
static const uint32_t canonical_keys[] = {0x082E0043, 0x082C005A, 0x082D0058, 0x082F0056};
static const unsigned tracks[] = {
    110, 12, 15, 24, 27, 36, 39, 48, 50, 59, 61, 76, 78, 83, 85, 89,
    92, 98, 100, 112, 117, 118, 119, 120, 121, 122, 126, 127, 128, 129,
    130, 131, 134, 137, 141, 202, 237, 239
};
enum page { CATEGORIES, CONTROLS, AUDIO, VIDEO };
static const char *const headings[] = {"Options", "Controls", "Audio", "Video"};
static const char *const control_labels[] = {
    "Tongue Lash", "Hurl", "Tailwhip", "Jump", "Run", "Pause", "Confirm", "Back", "Exit"
};
static const char *const audio_labels[] = {"Sound", "Voice", "Music", "Music Test", "Exit"};
static const char *const video_labels[] = {"Render backend", "Display", "Render resolution", "Vsync", "Exit"};
static enum page page;
static unsigned selection, track;
static int dirty, save_error, music_test, scaled_text;
static uint32_t wave_phase;
static uint32_t body_scale = 0x10000;
static int scroll_position, scroll_target;
static uint32_t scroll_tick;
enum { VIEW_TOP = 54, VIEW_BOTTOM = 212, SLIDER_LEFT = 55, SLIDER_WIDTH = 220 };
static uint32_t background_object, font_object;
static uint32_t hand_bank, hand_size, hand_model, hand_texture, hand_palette;
static int binding_column, capturing, video_ready, original_width, original_height;
static int original_api;
static int slider_drag = -1;
static int desktop_width = 320, desktop_height = 240;
static char status[96];
static struct { int renderer, api; } backends[16];
static unsigned backend_count;
static struct { int width, height; } resolutions[10];
static unsigned resolution_count;
static const char settings_path[] = "Software\\GexRecomp\\Options";
static void audio(unsigned channel);
static int save(void);

uint32_t gex_options_platform(void) {
#if defined(__EMSCRIPTEN__)
    return GEX_OPTIONS_WEB;
#elif defined(__ANDROID__)
    return GEX_OPTIONS_ANDROID;
#elif defined(_WIN32)
    return GEX_OPTIONS_WINDOWS;
#elif defined(__APPLE__)
    return GEX_OPTIONS_MACOS;
#elif defined(__DREAMCAST__)
    return GEX_OPTIONS_DREAMCAST;
#elif defined(__linux__)
    return GEX_OPTIONS_LINUX;
#else
#error Define the Options platform for this target.
#endif
}

int gex_options_platform_matches(uint32_t platforms) {
    return (platforms & gex_options_platform()) != 0;
}

void gex_options_title_init(void) {
    if (gex_prefs.bindings_active) {
        for (unsigned i = 0; i < 4; ++i) MEM32(actions[i]) = canonical_keys[i];
        for (unsigned i = 0; i < 3; ++i) {
            MEM32(0x455000 + i * 4) = gex_prefs.volume[i] != 0;
            MEM32(0x45500C + i * 4) = gex_prefs.volume[i];
            audio(i);
        }
    }
    const uint32_t args[] = {0x7B, 3};
    uint32_t title_object = hle_call_guest(0x40C110, 2, args);
    if (!title_object) hle_fatal("Options: retail title item is missing");
    font_object = title_object;
    /* Replace the string, not the widget, font, position, or animation. */
    uint32_t label = MEM32(title_object + 0x9C);
    memcpy(hle_ptr(label), "Options", sizeof "Options");
    background_object = 0;
}

static uint32_t background(void) {
    if (background_object) return background_object;
    for (uint32_t list = 0x4A28A0; list < 0x4A2918; list += 12)
        for (uint32_t object = MEM32(list); object && MEM32(object); object = MEM32(object))
            if (MEM32(object + 0x60) == 0x40E570) return background_object = object;
    hle_fatal("Options: retail TV static object is missing");
    return 0;
}

int gex_options_scaled_text(void) { return scaled_text; }

static uint32_t little_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t hand_pointer(uint32_t encoded, uint32_t length) {
    uint32_t offset = ((encoded & 0x7FF00000u) >> 20) * 8192u +
        (encoded & 0x000FFFFEu);
    if (!(encoded & 1) || (encoded & 0x80000000u) ||
        length > hand_size || offset > hand_size - length)
        hle_fatal("Options: invalid HUD resource pointer 0x%08X", encoded);
    return hand_bank + offset;
}

static void load_hand(void) {
    char path[4096];
    if (!hle_host_path("LEV\\GEX125.LEV", path, sizeof path))
        hle_fatal("Options: cannot locate LEV/GEX125.LEV for the HUD hand");
    FILE *file = fopen(path, "rb");
    if (!file) hle_fatal("Options: cannot open HUD resources: %s", path);
    unsigned char header[0x50];
    if (fread(header, 1, sizeof header, file) != sizeof header ||
        little_u32(header) < 4)
        hle_fatal("Options: invalid HUD bank header");
    hand_size = little_u32(header + 0x48);
    uint32_t offset = little_u32(header + 0x4C);
    if (hand_size < 0x13704 || hand_size > 0x100000 ||
        offset > 0x100000 || fseek(file, (long)offset, SEEK_SET))
        hle_fatal("Options: invalid HUD data chunk");
    void *data = hle_alloc(hand_size + 0x30);
    if (!data) hle_fatal("Options: cannot allocate HUD resource memory");
    hand_bank = hle_va(data);
    if (fread(data, 1, hand_size, file) != hand_size)
        hle_fatal("Options: cannot read HUD resources");
    if (fclose(file)) hle_fatal("Options: cannot close HUD resource file");

    /* Retail GEX125's HUD model: animation 2, frame 1 is the filled health hand. */
    uint32_t hud = hand_bank + 0x136EC;
    if (MEM32(hud + 8) != 1 || MEM32(hud + 12) != 5)
        hle_fatal("Options: unsupported HUD model");
    uint32_t groups = hand_pointer(MEM32(hud), 20);
    uint32_t frames = hand_pointer(MEM32(groups + 8), 8);
    uint32_t cel = hand_pointer(MEM32(frames + 4), 28);
    uint32_t parts = hand_pointer(MEM32(cel + 24), 8);
    uint32_t part = hand_pointer(MEM32(parts), 20);
    if (MEM32(parts + 4)) hle_fatal("Options: unsupported multi-part HUD hand");
    hand_texture = hand_pointer(MEM32(part + 8), 20 + 16 * 14);
    hand_palette = hand_pointer(MEM32(part + 12), 36) + 4;
    if (MEM32(hand_texture) != 16u * 65536u ||
        MEM32(hand_texture + 4) != 14u * 65536u)
        hle_fatal("Options: unsupported HUD hand dimensions");
    MEM32(cel + 24) = parts;
    MEM32(parts) = part;
    MEM32(part + 8) = hand_texture;
    MEM32(part + 12) = hand_palette;
    hle_call_guest(0x441010, 1, &hand_texture);

    /* Expose only this CEL, without loading a level or relocating the other HUD assets. */
    hand_model = hand_bank + hand_size;
    MEM32(hand_model) = hand_model + 0x18;
    MEM32(hand_model + 8) = MEM32(hand_model + 12) = 1;
    MEM32(hand_model + 16) = hand_model + 0x28;
    MEM32(hand_model + 0x18) = hand_model + 0x20;
    MEM32(hand_model + 0x20) = cel;
    MEM32(hand_model + 0x28) = 1;
}

static void hand(int x, int y) {
    if (!hand_model) load_hand();
    uint32_t saved_esp = g_esp;
    g_esp -= 0x204;
    uint32_t object = g_esp;
    memcpy(hle_ptr(object), hle_ptr(font_object), 0x204);
    MEM32(object + 0xC) = hand_model;
    MEM32(object + 0x50) = MEM32(object + 0x54) = 0;
    MEM32(object + 0xBC) = MEM32(object + 0xC0) = MEM32(object + 0xB4) = 0;
    MEM32(object + 0xE0) |= 0x01000000u;
    MEM32(object + 0x78) = MEM32(0x4A2974) + (uint32_t)(x + 6) * 65536u;
    MEM32(object + 0x7C) = MEM32(0x4A2988) + (uint32_t)(y * 224 / 240 + 15) * 65536u;
    hle_call_guest(0x444590, 1, &object);
    g_esp = saved_esp;
}

static void text(int x, int y, const char *label, int heading, int selected) {
    if (!font_object) hle_fatal("Options: retail title font is missing");
    uint32_t saved_esp = g_esp;
    /* A guest-stack copy keeps the title widget untouched by sprite drawing. */
    g_esp -= 0x284;
    uint32_t object = g_esp + 128;
    memcpy(hle_ptr(object), hle_ptr(font_object), 0x204);
    MEM32(object + 0xBC) = MEM32(object + 0xC0) = 0;
    unsigned spacing = heading || body_scale == 0x10000 ? 10 : 6;
    /* Retail's decimal font scale also scales character advances. */
    MEM32(object + 0xB4) = spacing == 10 ? 0 : spacing << 8;
    MEM32(object + 0xC8) = MEM32(object + 0xCC) = heading ? 0x10000 : body_scale * 10 / spacing;
    MEM32(object + 0xE0) |= 0x01000000u;
    do {
        const char *end = strchr(label, '\n');
        size_t length = end ? (size_t)(end - label) : strlen(label);
        if (length >= 128) hle_fatal("Options: text exceeds guest string buffer");
        memcpy(hle_ptr(g_esp), label, length);
        MEM8(g_esp + (uint32_t)length) = 0;
        uint32_t args[] = {
            MEM32(0x4A2974) + (uint32_t)(x + 6) * 65536u,
            MEM32(0x4A2988) + (uint32_t)(y * 224 / 240 + 8 + (heading ? 22 : 16)) * 65536u,
            heading || body_scale == 0x10000 ? 0x5A : 0x5B, 3, 0x7B,
            selected ? wave_phase : UINT32_MAX, g_esp, object
        };
        scaled_text = 1;
        hle_call_guest(0x40BC70, 8, args);
        scaled_text = 0;
        if (!end) break;
        label = end + 1;
        y += 26;
    } while (*label);
    g_esp = saved_esp;
}

static uint32_t registry_root(void) {
    return hle_call_guest(hle_resolve("GetVersion"), 0, NULL) & 0x80000000u ?
        0x80000002u : 0x80000001u;
}

static int read_setting(const char *path, const char *name, uint32_t *value) {
    uint32_t saved_esp = g_esp;
    g_esp -= 256;
    memcpy(hle_ptr(g_esp), path, strlen(path) + 1);
    memcpy(hle_ptr(g_esp + 96), name, strlen(name) + 1);
    const uint32_t open[] = {registry_root(), g_esp, g_esp + 224};
    uint32_t error = hle_call_guest(hle_resolve("RegOpenKeyA"), 3, open);
    if (!error) {
        uint32_t key = MEM32(g_esp + 224);
        MEM32(g_esp + 232) = 4;
        const uint32_t query[] = {key, g_esp + 96, 0, g_esp + 228, g_esp + 236, g_esp + 232};
        error = hle_call_guest(hle_resolve("RegQueryValueExA"), 6, query);
        if (!error && (MEM32(g_esp + 228) != 4 || MEM32(g_esp + 232) != 4)) error = 13;
        if (!error) *value = MEM32(g_esp + 236);
        hle_call_guest(hle_resolve("RegCloseKey"), 1, &key);
    }
    g_esp = saved_esp;
    if (error == 2) return 0;
    if (error) {
        fprintf(stderr, "[options] cannot read %s\\%s (error %u)\n", path, name, error);
        return -1;
    }
    return 1;
}

int gex_options_load(void) {
    gex_preferences_reset();
    if (gex_video_display_count() == 1) gex_prefs.display = GEX_DISPLAY_BORDERLESS;
    uint32_t version = 0;
    int found = read_setting(settings_path, "Version", &version);
    if (found < 0 || (found && version != 1)) goto invalid;
    if (!found) {
        static const char *const names[] = {"Option_Effects", "Option_Voice", "Option_Music"};
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t enabled = 1;
            if (read_setting("Software\\Microsoft\\Games\\GEX", names[i], &enabled) < 0) return -1;
            gex_prefs.volume[i] = enabled ? 100 : 0;
        }
        static const unsigned migrated[] = {GEX_ACTION_LASH, GEX_ACTION_WHIP, GEX_ACTION_JUMP, GEX_ACTION_RUN};
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t value = canonical_keys[i];
            if (read_setting("Software\\Microsoft\\Games\\GEX", hle_ptr(actions[i] + 8), &value) < 0)
                return -1;
            gex_prefs.bindings[migrated[i]].key = value & 0xFFFF;
            gex_prefs.bindings[migrated[i]].scan = (value >> 16) & 0xFF;
        }
        gex_prefs.bindings[GEX_ACTION_HURL] = gex_prefs.bindings[GEX_ACTION_LASH];
    } else {
        for (unsigned i = 0; i < GEX_ACTION_COUNT; ++i) {
            char name[32];
            snprintf(name, sizeof name, "Key%u", i);
            uint32_t value = 0;
            if (read_setting(settings_path, name, &value) != 1) goto invalid;
            gex_prefs.bindings[i].key = value & 0xFFFF;
            gex_prefs.bindings[i].scan = value >> 16;
            snprintf(name, sizeof name, "Pad%u", i);
            if (read_setting(settings_path, name, &gex_prefs.bindings[i].button) != 1) goto invalid;
        }
        for (unsigned i = 0; i < 3; ++i) {
            char name[32];
            snprintf(name, sizeof name, "Volume%u", i);
            if (read_setting(settings_path, name, &gex_prefs.volume[i]) != 1 ||
                gex_prefs.volume[i] > 100) goto invalid;
        }
        static const char *const names[] = {"Renderer", "API", "Display", "Width", "Height", "Triple"};
        int *const values[] = {&gex_prefs.renderer, &gex_prefs.api, &gex_prefs.display,
            &gex_prefs.width, &gex_prefs.height, &gex_prefs.triple};
        for (unsigned i = 0; i < sizeof values / sizeof *values; ++i) {
            uint32_t value;
            if (read_setting(settings_path, names[i], &value) != 1 || value > 8192) goto invalid;
            *values[i] = (int)value;
        }
        if (gex_prefs.renderer < 0 || gex_prefs.renderer >= 5 ||
            gex_prefs.api >= GEX_API_COUNT ||
            gex_prefs.display > GEX_DISPLAY_EXCLUSIVE || gex_prefs.triple > 1 ||
            !gex_renderer_valid_size(gex_prefs.width, gex_prefs.height)) goto invalid;
        if (!gex_renderer_available(gex_prefs.renderer)) {
            int fallback = 0;
            while (fallback < 5 && !gex_renderer_available(fallback)) ++fallback;
            if (fallback == 5) goto invalid;
            fprintf(stderr, "[options] saved renderer %d is unavailable in this build; using renderer %d\n",
                    gex_prefs.renderer, fallback);
            gex_prefs.renderer = fallback;
            gex_prefs.api = GEX_API_AUTO;
        }
        int saved_width = gex_prefs.width, saved_height = gex_prefs.height;
        int migrated = gex_video_migrate_saved_resolution(&gex_prefs.width, &gex_prefs.height);
        if (migrated < 0) goto invalid;
        if (migrated)
            fprintf(stderr, "[options] migrated unsupported saved resolution %dx%d to %dx%d\n",
                    saved_width, saved_height, gex_prefs.width, gex_prefs.height);
    }
    for (unsigned i = 0; i < GEX_ACTION_COUNT; ++i)
        if (gex_controls_bind(i, 0, gex_prefs.bindings[i].key, gex_prefs.bindings[i].scan) ||
            gex_controls_bind(i, 1, gex_prefs.bindings[i].button, 0)) goto invalid;
    return 0;
invalid:
    fprintf(stderr, "[options] saved Options settings are invalid or unavailable in this build\n");
    return -1;
}

int gex_options_video_init(int renderer, int width, int height) {
    gex_prefs.active_renderer = gex_prefs.renderer = renderer;
    original_api = gex_prefs.api;
#if GEX_ENABLE_BGFX_RENDERER
    gex_prefs.supported_apis = gex_bgfx_supported_apis();
#endif
    backend_count = 0;
    for (int i = 0; i < 5; ++i) {
        if (!gex_renderer_available(i)) continue;
        if (i == 4) {
            for (int api = 0; api < GEX_API_COUNT; ++api)
                if (!api || (gex_prefs.supported_apis & (1u << api))) {
                    backends[backend_count].renderer = i;
                    backends[backend_count++].api = api;
                }
        } else {
            backends[backend_count].renderer = i;
            backends[backend_count++].api = GEX_API_AUTO;
        }
    }
    if (gex_video_desktop(&desktop_width, &desktop_height)) return -1;
    resolution_count = 0;
    static const int heights[] = {240, 360, 480, 720, 1080, 1440, 2160};
    for (unsigned i = 0; i < sizeof heights / sizeof *heights + 1; ++i) {
        int h = i == sizeof heights / sizeof *heights ? desktop_height : heights[i];
        if (h > desktop_height && h != 240) continue;
        int w = (int)(((int64_t)desktop_width * h + desktop_height / 2) / desktop_height);
        if (!gex_renderer_valid_size(w, h)) continue;
        unsigned j;
        for (j = 0; j < resolution_count && resolutions[j].height != h; ++j) {}
        if (j < resolution_count) continue;
        resolutions[resolution_count].width = w;
        resolutions[resolution_count++].height = h;
    }
    if (!width && gex_preferences_default_size(desktop_width, desktop_height, &width, &height))
        return -1;
    gex_prefs.width = original_width = width;
    gex_prefs.height = original_height = height;
    char error[256];
    if (gex_video_display(gex_prefs.display, error, sizeof error)) {
        fprintf(stderr, "[options] %s\n", error);
        return -1;
    }
    video_ready = 1;
    return 0;
}

static int save(void) {
    static const char path[] = "Software\\Microsoft\\Games\\GEX";
    static const char *const names[] = {"Option_Effects", "Option_Voice", "Option_Music"};
    uint32_t root = registry_root();
    for (unsigned i = 0; i < 3; ++i)
        hle_reg_set_dword(root, path, names[i], MEM32(0x455000 + i * 4));
    hle_reg_set_dword(root, settings_path, "Version", 1);
    for (unsigned i = 0; i < GEX_ACTION_COUNT; ++i) {
        char name[32];
        snprintf(name, sizeof name, "Key%u", i);
        hle_reg_set_dword(root, settings_path, name,
            gex_prefs.bindings[i].key | gex_prefs.bindings[i].scan << 16);
        snprintf(name, sizeof name, "Pad%u", i);
        hle_reg_set_dword(root, settings_path, name, gex_prefs.bindings[i].button);
    }
    for (unsigned i = 0; i < 3; ++i) {
        char name[32];
        snprintf(name, sizeof name, "Volume%u", i);
        hle_reg_set_dword(root, settings_path, name, gex_prefs.volume[i]);
    }
    static const char *const video_names[] = {"Renderer", "API", "Display", "Width", "Height", "Triple"};
    const int values[] = {gex_prefs.renderer, gex_prefs.api, gex_prefs.display,
        gex_prefs.width, gex_prefs.height, gex_prefs.triple};
    for (unsigned i = 0; i < sizeof values / sizeof *values; ++i)
        hle_reg_set_dword(root, settings_path, video_names[i], (uint32_t)values[i]);
    save_error = hle_reg_flush() != 0;
    if (save_error) fprintf(stderr, "[options] cannot save Options settings\n");
    if (!save_error) dirty = 0;
    return !save_error;
}

static void audio(unsigned channel) {
    static const uint32_t setters[] = {0x401E20, 0x401E70, 0x401ED0};
    uint32_t args[] = {MEM32(0x455000 + channel * 4) ? MEM32(0x45500C + channel * 4) : 0, 0};
    hle_call_guest(setters[channel], 2, args);
    if (channel == 2 && MEM32(0x48A040)) {
        args[0] = MEM32(0x48A040);
        args[1] = MEM32(0x48A030);
        uint32_t status = hle_call_guest(MEM32(MEM32(args[0]) + 0x3C), 2, args);
        if (status) hle_fatal("Options: cannot update music volume (0x%08X)", status);
    }
}

void gex_options_begin(void) {
    page = CATEGORIES;
    selection = track = 0;
    wave_phase = 0;
    dirty = save_error = music_test = 0;
    binding_column = capturing = 0;
    slider_drag = -1;
    scroll_position = scroll_target = 0;
    scroll_tick = hle_ticks_ms();
    status[0] = 0;
}

static unsigned entry_count(void) {
    return page == CATEGORIES ? 4 : page == CONTROLS ? 9 : 5;
}

static int entry_top(unsigned i) {
    if (page == CATEGORIES) return 60 + (int)i * 42;
    if (page == CONTROLS) return 90 + (int)i * 64 + (i >= 6 ? 28 : 0);
    return 60 + (int)i * (page == VIDEO ? 84 : 64);
}

static int entry_height(unsigned i) {
    if (page == CATEGORIES || i + 1 == entry_count()) return 26;
    return page == VIDEO ? 78 : 56;
}

static int entry_y(unsigned i) {
    return entry_top(i) - ((scroll_position + 0x8000) >> 16);
}

static int viewport_bottom(void) {
    return page == VIDEO ? 196 : VIEW_BOTTOM;
}

static void follow_selection(void) {
    if (page == CATEGORIES) { scroll_target = 0; return; }
    int top = entry_top(selection), bottom = top + entry_height(selection);
    if (top - scroll_target < VIEW_TOP + 4) scroll_target = top - VIEW_TOP - 4;
    if (bottom - scroll_target > viewport_bottom() - 4) scroll_target = bottom - viewport_bottom() + 4;
    int maximum = entry_top(entry_count() - 1) + 26 - viewport_bottom() + 4;
    if (scroll_target > maximum) scroll_target = maximum;
    if (scroll_target < 0) scroll_target = 0;
}

static void reset_scroll(void) {
    scroll_position = scroll_target = 0;
    scroll_tick = hle_ticks_ms();
    follow_selection();
}

static unsigned slider_value(int x) {
    return x <= SLIDER_LEFT ? 0 : x >= SLIDER_LEFT + SLIDER_WIDTH ? 100 :
        (unsigned)(x - SLIDER_LEFT) * 100 / SLIDER_WIDTH;
}

static void slider_rect(int x, int y, int width, int height, unsigned rgb) {
    int top = VIEW_TOP * 224 / 240, bottom = VIEW_BOTTOM * 224 / 240;
    if (y + height > bottom) height = bottom - y;
    if (y < top) { height -= top - y; y = top; }
    if (height > 0) gex_graphics_menu_rect(x, y, width, height, rgb);
}

static void cancel_capture(void) {
    capturing = 0;
    gex_binding_key_filter = NULL;
    gex_binding_pad_filter = NULL;
}

static int capture_key(int down, uint32_t key, uint32_t scan) {
    if (!down) return 1;
    if (key == 0x1B) { cancel_capture(); status[0] = 0; return 1; }
    if (binding_column) return 1;
    if (gex_controls_bind(selection, 0, key, scan)) {
        snprintf(status, sizeof status, "Key unavailable or already bound");
        return 1;
    }
    dirty = 1;
    status[0] = 0;
    cancel_capture();
    return 1;
}

static int capture_pad(uint32_t buttons) {
    if (!binding_column && (buttons & (GEX_PAD_BACK | gex_prefs.bindings[GEX_ACTION_BACK].button))) {
        cancel_capture();
        status[0] = 0;
        return 1;
    }
    if (buttons && binding_column) {
        uint32_t button = buttons & (0u - buttons);
        if (gex_controls_bind(selection, 1, button, 0)) {
            snprintf(status, sizeof status, "Button unavailable or already bound");
            return 1;
        }
        dirty = 1;
        status[0] = 0;
        cancel_capture();
    }
    return 1;
}

static void volume(unsigned channel, unsigned value) {
    gex_prefs.volume[channel] = value;
    MEM32(0x455000 + channel * 4) = value != 0;
    MEM32(0x45500C + channel * 4) = value;
    audio(channel);
    dirty = 1;
}

static int triple_supported(void) {
    return gex_prefs.active_renderer == 4 &&
        (gex_prefs.active_api == GEX_API_VULKAN || gex_prefs.active_api == GEX_API_METAL ||
         gex_prefs.active_api == GEX_API_D3D11 || gex_prefs.active_api == GEX_API_D3D12);
}

static int activate(int direction) {
    if (selection + 1 == entry_count()) {
        if (page != CATEGORIES) {
            selection = page - 1;
            page = CATEGORIES;
            reset_scroll();
            status[0] = 0;
            return 0;
        }
        return !dirty || save();
    }
    if (page == CATEGORIES) {
        page = (enum page)(selection + 1);
        selection = 0;
        reset_scroll();
    } else if (page == CONTROLS) {
        capturing = 1;
        status[0] = 0;
        gex_binding_key_filter = capture_key;
        gex_binding_pad_filter = capture_pad;
    } else if (page == AUDIO && selection < 3) {
        int value = (int)gex_prefs.volume[selection] + direction * 5;
        volume(selection, (unsigned)(value < 0 ? 0 : value > 100 ? 100 : value));
    } else if (page == AUDIO) {
        track = (track + (direction < 0 ? sizeof tracks / sizeof *tracks - 1 : 1)) %
            (sizeof tracks / sizeof *tracks);
        MEM32(0x48A034) = tracks[track];
        music_test = 1;
    } else if (page == VIDEO) {
        if (!video_ready) {
            snprintf(status, sizeof status, "Video settings unavailable");
            return 0;
        }
        if (selection == 0) {
            unsigned index = 0;
            while (index < backend_count && (backends[index].renderer != gex_prefs.renderer ||
                   backends[index].api != gex_prefs.api)) ++index;
            index = (index + backend_count + direction) % backend_count;
            gex_prefs.renderer = backends[index].renderer;
            gex_prefs.api = backends[index].api;
        } else if (selection == 1) {
            int mode = gex_video_display_count() == 1 ? GEX_DISPLAY_BORDERLESS :
                (gex_prefs.display + 3 + direction) % 3;
            char error[256];
            if (gex_video_display(mode, error, sizeof error)) {
                fprintf(stderr, "[options] %s\n", error);
                snprintf(status, sizeof status, "Display change failed");
                return 0;
            }
        } else if (selection == 2) {
            unsigned index = 0;
            while (index < resolution_count && (resolutions[index].width != gex_prefs.width ||
                   resolutions[index].height != gex_prefs.height)) ++index;
            index = (index + resolution_count + direction) % resolution_count;
            gex_prefs.width = resolutions[index].width;
            gex_prefs.height = resolutions[index].height;
        } else {
            if (!triple_supported()) {
                snprintf(status, sizeof status, "Triple buffering unavailable");
                return 0;
            }
            gex_prefs.triple = !gex_prefs.triple;
        }
        dirty = 1;
        status[0] = 0;
    }
    save_error = 0;
    return 0;
}

int gex_options_key(uint32_t key) {
    slider_drag = -1;
    if (key == UINT32_MAX) {
        cancel_capture();
        if (dirty) save();
        return 1;
    }
    if (capturing) { capture_key(1, key, 0); return 0; }
    if (key == 0x1B || key == 'C' || key == 'Z') {
        if (page == CATEGORIES) return !dirty || save();
        selection = page - 1;
        page = CATEGORIES;
        reset_scroll();
        status[0] = 0;
        return 0;
    }
    if (key == 0x26) { selection = (selection + entry_count() - 1) % entry_count(); wave_phase = 0; }
    else if (key == 0x28) { selection = (selection + 1) % entry_count(); wave_phase = 0; }
    else if (key == 0x25 || key == 0x27) {
        if (page == CONTROLS && selection < GEX_ACTION_COUNT) binding_column = !binding_column;
        else if (page != CATEGORIES && selection + 1 != entry_count())
            return activate(key == 0x25 ? -1 : 1);
    } else if (key == 'X' || key == 0x0D || key == 0x20) return activate(1);
    follow_selection();
    return 0;
}

int gex_options_pointer(int x, int y) {
    if (capturing) { cancel_capture(); status[0] = 0; return 0; }
    if (x < 20 || x >= 306 || y < VIEW_TOP || y >= viewport_bottom()) return 0;
    for (unsigned i = 0; i < entry_count(); ++i) {
        int top = entry_y(i), bottom = top + entry_height(i);
        if (y >= top - 4 && y < bottom) {
            selection = i;
            wave_phase = 0;
            follow_selection();
            if (page == CONTROLS && i < GEX_ACTION_COUNT) binding_column = x >= 174;
            if (page == AUDIO && i < 3 && x >= SLIDER_LEFT && y >= top + 24) {
                volume(i, slider_value(x));
                return 0;
            }
            return activate(1);
        }
    }
    return 0;
}

int gex_options_pointer_event(int x, int y, int down) {
    if (!down) { slider_drag = -1; return 0; }
    if (down < 0) {
        if (slider_drag >= 0) {
            volume((unsigned)slider_drag, slider_value(x));
        }
        return 0;
    }
    int result = gex_options_pointer(x, y);
    if (page == AUDIO && selection < 3 && x >= SLIDER_LEFT && y >= VIEW_TOP && y < viewport_bottom() &&
        y >= entry_y(selection) + 24 && y < entry_y(selection) + entry_height(selection))
        slider_drag = (int)selection;
    return result;
}

void gex_options_render(void) {
    uint32_t now = hle_ticks_ms(), elapsed = now - scroll_tick;
    scroll_tick = now;
    if (elapsed > 50) elapsed = 50;
    int difference = scroll_target * 65536 - scroll_position;
    scroll_position += (int)((int64_t)difference * elapsed / (32 + elapsed));
    if (difference > -0x4000 && difference < 0x4000) scroll_position = scroll_target * 65536;
    uint32_t object = background();
    hle_call_guest(0x40E570, 1, &object);
    body_scale = 0x10000;
    text(84, 14, headings[page], 1, 0);
    gex_graphics_menu_clip_begin(VIEW_TOP * 224 / 240, viewport_bottom() * 224 / 240);
    if (page == CONTROLS) {
        int offset = (scroll_position + 0x8000) >> 16;
        int group_y[] = {60 - offset, 60 + 6 * 64 + 28 - offset};
        for (unsigned i = 0; i < 2; ++i)
            if (group_y[i] > VIEW_TOP - 30 && group_y[i] < viewport_bottom())
                text(39, group_y[i], i ? "Menus" : "Gameplay", 0, 0);
    }
    for (unsigned i = 0; i < entry_count(); ++i) {
        int selected = i == selection;
        int y = entry_y(i);
        if (y + entry_height(i) < VIEW_TOP || y >= viewport_bottom()) continue;
        const char *label = page == CATEGORIES ? i < 3 ? headings[i + 1] : "Exit" :
            page == CONTROLS ? control_labels[i] : page == AUDIO ? audio_labels[i] :
            i == 2 ? "Render\nresolution" : video_labels[i];
        if (selected) hand(22, y);
        text(39, y, label, 0, selected);
        char value[64];
        if (page == CONTROLS && i < GEX_ACTION_COUNT) {
            gex_controls_key_name(gex_prefs.bindings[i].key, value, sizeof value);
            text(55, y + 26, capturing && selected && !binding_column ? "..." : value, 0, selected && !binding_column);
            text(190, y + 26, capturing && selected && binding_column ? "..." :
                gex_controls_button_name(gex_prefs.bindings[i].button), 0, selected && binding_column);
        } else if (page == AUDIO && i < 3) {
            int middle = (y + 34) * 224 / 240;
            int knob = SLIDER_LEFT + (int)gex_prefs.volume[i] * SLIDER_WIDTH / 100;
            slider_rect(SLIDER_LEFT - 2, middle - 2, SLIDER_WIDTH + 6, 6, 0x301000);
            slider_rect(SLIDER_LEFT, middle, SLIDER_WIDTH, 2, 0x20D020);
            slider_rect(knob - 2, middle - 7, 7, 17, 0x301000);
            slider_rect(knob, middle - 5, 3, 13, selected ? 0x20FF20 : 0x20D020);
        } else if (page == AUDIO && i == 3) {
            if (!track) snprintf(value, sizeof value, "Title");
            else snprintf(value, sizeof value, "%03u", tracks[track]);
            text(55, y + 26, value, 0, 0);
        } else if (page == VIDEO && i < 4) {
            static const char *const renderer_labels[] = {"Software", "OpenGL packet", "PVR", "OpenGL 1.2", "bgfx"};
            static const char *const display_labels[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
            if (i == 0) {
                if (gex_prefs.renderer == 4) snprintf(value, sizeof value,
                    "bgfx\n%s", gex_api_label(gex_prefs.api));
                else snprintf(value, sizeof value, "%s", gex_prefs.renderer >= 0 ?
                    renderer_labels[gex_prefs.renderer] : "Unavailable");
            } else if (i == 1) snprintf(value, sizeof value, "%s",
                gex_prefs.display == GEX_DISPLAY_BORDERLESS ? "Borderless\nfullscreen" :
                gex_prefs.display == GEX_DISPLAY_EXCLUSIVE ? "Exclusive\nfullscreen" : display_labels[gex_prefs.display]);
            else if (i == 2) snprintf(value, sizeof value, "%dx%d", gex_prefs.width, gex_prefs.height);
            else snprintf(value, sizeof value, "%s", gex_prefs.triple && triple_supported() ? "Triple\nbuffering" : "On");
            text(55, y + (i == 2 ? 52 : 26), value, 0, 0);
        }
    }
    gex_graphics_menu_clip_end();
    body_scale = 0x9000;
    if (save_error) text(39, 210, "Save failed", 0, 0);
    else if (status[0]) text(22, 210, status, 0, 0);
    else if (capturing) {
        body_scale = 0x10000;
        text(22, 200, binding_column ? "Press a button" : "Press a key", 0, 0);
    } else if (page == VIDEO && (gex_prefs.renderer != gex_prefs.active_renderer ||
             (gex_prefs.renderer == 4 && gex_prefs.api != original_api))) {
        body_scale = 0x10000;
        text(22, 210, "Restart to apply", 0, 0);
    }
    const uint32_t immediate = 1;
    hle_call_guest(0x43DB70, 1, &immediate);
    hle_call_guest(0x43F2D0, 1, &immediate);
    hle_call_guest(0x406C30, 0, NULL);
    wave_phase = (wave_phase + 0x10000u) & 0x7FFFFFFFu;
}

void gex_options_end(void) {
    cancel_capture();
    if (hand_model) {
        hle_call_guest(0x43EEA0, 1, &hand_texture);
        hle_call_guest(0x43EE60, 1, &hand_palette);
        hle_free(hle_ptr(hand_bank));
        hand_bank = hand_size = hand_model = hand_texture = hand_palette = 0;
    }
    /* The worker clears its request word after loading, so it is not a track ID. */
    if (music_test) MEM32(0x48A034) = tracks[0];
    if (video_ready && (gex_prefs.width != original_width || gex_prefs.height != original_height)) {
        if (gex_graphics_resize(gex_prefs.width, gex_prefs.height))
            hle_fatal("Options: render resolution change failed");
        original_width = gex_prefs.width;
        original_height = gex_prefs.height;
    }
}
