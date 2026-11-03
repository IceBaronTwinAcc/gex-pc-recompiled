#include <stdio.h>
#include <string.h>
#include "host.h"
#include "win32hle.h"
#include "options_menu.h"

static const uint32_t actions[] = {0x45560C, 0x4555D0, 0x455594, 0x455648};
static const uint32_t keys[] = {0x082E0043, 0x082C005A, 0x082D0058, 0x082F0056};
static const unsigned tracks[] = {
    110, 12, 15, 24, 27, 36, 39, 48, 50, 59, 61, 76, 78, 83, 85, 89,
    92, 98, 100, 112, 117, 118, 119, 120, 121, 122, 126, 127, 128, 129,
    130, 131, 134, 137, 141, 202, 237, 239
};
enum option_kind { OPTION_AUDIO, OPTION_MUSIC_TEST, OPTION_BINDING, OPTION_EXIT };
static const struct option {
    const char *label;
    enum option_kind kind;
    unsigned value;
    int y;
    uint32_t platforms;
} options[] = {
    {"Sound", OPTION_AUDIO, 0, 39, GEX_OPTIONS_ALL},
    {"Voice", OPTION_AUDIO, 1, 55, GEX_OPTIONS_ALL},
    {"Music", OPTION_AUDIO, 2, 71, GEX_OPTIONS_ALL},
    {"Music Test", OPTION_MUSIC_TEST, 0, 88, GEX_OPTIONS_ALL},
    {"Tongue Lash\n    Hurl", OPTION_BINDING, 0, 122, GEX_OPTIONS_ALL},
    {"Tailwhip", OPTION_BINDING, 1, 155, GEX_OPTIONS_ALL},
    {"Jump", OPTION_BINDING, 2, 172, GEX_OPTIONS_ALL},
    {"Run", OPTION_BINDING, 3, 188, GEX_OPTIONS_ALL},
    {"Exit", OPTION_EXIT, 0, 210, GEX_OPTIONS_ALL}
};
#define OPTION_COUNT (sizeof options / sizeof *options)
static unsigned selection, track;
static int dirty, save_error, music_test, scaled_text;
static uint32_t wave_phase;
static uint32_t background_object, font_object;
static uint32_t hand_bank, hand_size, hand_model, hand_texture, hand_palette;
static unsigned visible[OPTION_COUNT], visible_count;

uint32_t gex_options_platform(void) {
#if defined(__ANDROID__)
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
    MEM32(object + 0xB4) = 0;
    MEM32(object + 0xC8) = MEM32(object + 0xCC) = heading ? 0x10000 : 0xC000;
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
            heading ? 0x5A : 0x5B, 3, 0x7B,
            selected ? wave_phase : UINT32_MAX, g_esp, object
        };
        scaled_text = 1;
        hle_call_guest(0x40BC70, 8, args);
        scaled_text = 0;
        if (!end) break;
        label = end + 1;
        y += 17;
    } while (*label);
    g_esp = saved_esp;
}

static uint32_t registry_root(void) {
    return hle_call_guest(hle_resolve("GetVersion"), 0, NULL) & 0x80000000u ?
        0x80000002u : 0x80000001u;
}

static int save(void) {
    static const char path[] = "Software\\Microsoft\\Games\\GEX";
    static const char *const names[] = {"Option_Effects", "Option_Voice", "Option_Music"};
    uint32_t root = registry_root();
    for (unsigned i = 0; i < 3; ++i)
        hle_reg_set_dword(root, path, names[i], MEM32(0x455000 + i * 4));
    for (unsigned i = 0; i < 4; ++i)
        hle_reg_set_dword(root, path, hle_ptr(actions[i] + 8), MEM32(actions[i]));
    save_error = hle_reg_flush() != 0;
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
    selection = track = 0;
    wave_phase = 0;
    dirty = save_error = music_test = 0;
    visible_count = 0;
    for (unsigned i = 0; i < OPTION_COUNT; ++i)
        if (gex_options_platform_matches(options[i].platforms)) visible[visible_count++] = i;
    if (!visible_count) hle_fatal("Options: no entries for this platform");
}

static int activate(int direction) {
    const struct option *option = &options[visible[selection]];
    if (option->kind == OPTION_AUDIO) {
        uint32_t address = 0x455000 + option->value * 4;
        MEM32(address) = !MEM32(address);
        audio(option->value);
        dirty = 1;
    } else if (option->kind == OPTION_MUSIC_TEST) {
        track = (track + (direction < 0 ? sizeof tracks / sizeof *tracks - 1 : 1)) %
            (sizeof tracks / sizeof *tracks);
        MEM32(0x48A034) = tracks[track];
        music_test = 1;
    } else if (option->kind == OPTION_BINDING) {
        unsigned action = option->value, current = 0;
        while (current < 4 && MEM32(actions[action]) != keys[current]) ++current;
        unsigned next = current == 4 ? 0 : (current + (direction < 0 ? 3 : 1)) % 4;
        uint32_t old = MEM32(actions[action]);
        /* Swap occupied bindings, keeping every action reachable. */
        for (unsigned i = 0; i < 4; ++i)
            if (i != action && MEM32(actions[i]) == keys[next]) MEM32(actions[i]) = old;
        MEM32(actions[action]) = keys[next];
        dirty = 1;
    } else return !dirty || save();
    save_error = 0;
    return 0;
}

int gex_options_key(uint32_t key) {
    if (key == UINT32_MAX) {
        if (dirty) save();
        return 1;
    }
    if (key == 0x1B || key == 'C' || key == 'Z') return !dirty || save();
    if (key == 0x26) { selection = (selection + visible_count - 1) % visible_count; wave_phase = 0; }
    else if (key == 0x28) { selection = (selection + 1) % visible_count; wave_phase = 0; }
    else if (key == 0x25 || key == 0x27) {
        if (options[visible[selection]].kind != OPTION_EXIT)
            return activate(key == 0x25 ? -1 : 1);
    } else if (key == 'X' || key == 0x0D || key == 0x20) return activate(1);
    return 0;
}

int gex_options_pointer(int x, int y) {
    if (x < 20 || x >= 275) return 0;
    for (unsigned i = 0; i < visible_count; ++i) {
        const struct option *option = &options[visible[i]];
        int bottom = option->y + (option->kind == OPTION_BINDING && option->value == 0 ? 33 : 17);
        if (i + 1 < visible_count && bottom > options[visible[i + 1]].y)
            bottom = options[visible[i + 1]].y;
        if (y >= option->y && y < bottom) {
            selection = i;
            wave_phase = 0;
            return activate(1);
        }
    }
    return 0;
}

void gex_options_render(void) {
    uint32_t object = background();
    hle_call_guest(0x40E570, 1, &object);
    text(save_error ? 60 : 84, 14, save_error ? "Save failed" : "Options", 1, 0);
    text(39, 105, "Controls", 0, 0);
    for (unsigned i = 0; i < visible_count; ++i) {
        const struct option *option = &options[visible[i]];
        int selected = i == selection;
        if (selected) hand(option->kind == OPTION_BINDING ? 33 : 22, option->y);
        text(option->kind == OPTION_BINDING ? 50 : 39, option->y, option->label, 0, selected);
        char value[16];
        if (option->kind == OPTION_AUDIO)
            snprintf(value, sizeof value, "%s", MEM32(0x455000 + option->value * 4) ? "On" : "Off");
        else if (option->kind == OPTION_MUSIC_TEST) {
            if (!track) snprintf(value, sizeof value, "Title");
            else snprintf(value, sizeof value, "%03u", tracks[track]);
        } else if (option->kind == OPTION_BINDING) {
            unsigned key = MEM32(actions[option->value]) & 0xFFFFu;
            if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
                snprintf(value, sizeof value, "%c", (int)key);
            else snprintf(value, sizeof value, "Key %X", key);
        }
        else continue;
        text(201, option->kind == OPTION_BINDING && option->value == 0 ? 139 : option->y, value, 0, selected);
    }
    const uint32_t immediate = 1;
    hle_call_guest(0x43DB70, 1, &immediate);
    hle_call_guest(0x43F2D0, 1, &immediate);
    hle_call_guest(0x406C30, 0, NULL);
    wave_phase = (wave_phase + 0x10000u) & 0x7FFFFFFFu;
}

void gex_options_end(void) {
    if (hand_model) {
        hle_call_guest(0x43EEA0, 1, &hand_texture);
        hle_call_guest(0x43EE60, 1, &hand_palette);
        hle_free(hle_ptr(hand_bank));
        hand_bank = hand_size = hand_model = hand_texture = hand_palette = 0;
    }
    /* The worker clears its request word after loading, so it is not a track ID. */
    if (music_test) MEM32(0x48A034) = tracks[0];
}
