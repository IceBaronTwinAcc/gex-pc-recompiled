#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "host.h"
#include "win32hle.h"
#include "save_store.h"
#include "save_menu.h"
#include "options_menu.h"
#include "preferences.h"
#include "renderer.h"
#include "graphics_guest.h"
#include "platform/host.h"
#include "platform/filesystem.h"
#include "platform/input.h"

static int failures;
static const char *capture;
static const unsigned *keys;
static size_t key_count, key_index;
static uint32_t menu_started;
static int touch_exit, leave_held, close_request, close_after_back, owner_back;
static int consume_close_request;
static int transition_test, transition_stage, saw_black;
static int fade_out_frames[2], fade_in_frames[2];
static uint64_t previous_light;
#define CHECK(expression) do { \
    if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; } \
} while (0)

static void transition_frame(const gex_render_frame *frame) {
    uint64_t light = 0;
    CHECK(frame->pixels != NULL);
    if (!frame->pixels) return;
    for (int y = 0; y < frame->height; ++y)
        for (int x = 0; x < frame->width; ++x) {
            const uint8_t *p = frame->pixels + y * frame->pitch + x * 2;
            unsigned value = p[0] | (unsigned)p[1] << 8;
            light += (value >> 11) + (value >> 5 & 63) + (value & 31);
        }
    unsigned direction = (unsigned)transition_stage / 2;
    if (!(transition_stage & 1)) {
        CHECK(light <= previous_light);
        if (light < previous_light) ++fade_out_frames[direction];
        if (!light) {
            saw_black |= 1 << direction;
            ++transition_stage;
        }
    } else {
        CHECK(light >= previous_light);
        if (light > previous_light) ++fade_in_frames[direction];
    }
    previous_light = light;
}

static void pump(void) {
    if (gex_host_ticks_ms() - menu_started > 5000) hle_fatal("save menu test did not finish");
    if (close_request || (close_after_back && key_index == key_count && !gex_save_menu_open())) {
        CHECK(gex_save_key(1, UINT32_MAX, 0));
        if (consume_close_request) CHECK(gex_save_take_close_request());
        close_request = close_after_back = 0;
        return;
    }
    if (!gex_save_menu_open()) {
        if (transition_test) {
            CHECK(gex_save_key(1, 'X', 0));
            CHECK(gex_save_key(0, 'X', 0));
            CHECK(gex_input_pointer_filter(190, 50, 1));
            CHECK(gex_input_pointer_filter(190, 50, 0));
        }
        return;
    }
    if (transition_test && transition_stage == 1) transition_stage = 2;
    if (capture) {
        gex_render_frame frame;
        CHECK(!gex_renderer_frame(&frame));
        CHECK(!gex_host_screenshot(capture, frame.pixels, frame.width, frame.height, frame.pitch));
        capture = NULL;
    }
    if (touch_exit) {
        CHECK(gex_input_pointer_filter(250, 175, 1));
        CHECK(gex_input_pointer_filter(250, 175, 0));
        touch_exit = 0;
        return;
    }
    if (key_index < key_count) {
        unsigned key = keys[key_index++];
        CHECK(gex_save_key(1, key, 0));
        CHECK(gex_save_key(1, key, 0)); /* Keyboard repeat is consumed, not activated twice. */
        if (!leave_held) CHECK(gex_save_key(0, key, 0));
    }
}

static int menu(const unsigned *events, size_t count) {
    keys = events;
    key_count = count;
    key_index = 0;
    menu_started = gex_host_ticks_ms();
    int action = gex_save_choose();
    CHECK(key_index == key_count);
    CHECK(!gex_save_menu_open());
    return action;
}

static void options_closed(int code) {
    CHECK(code == 0);
    CHECK(!close_request);
    CHECK(!gex_save_menu_open());
    CHECK(gex_save_take_close_request() == !consume_close_request);
    CHECK(!MEM32(0x4A294C));
    gex_graphics_fade_capture();
    gex_graphics_fade_release();
    if (failures) _Exit(1);
    fprintf(stderr, "[options-test] closed without returning to title\n");
}

#define MENU(...) do { \
    const unsigned events[] = {__VA_ARGS__}; \
    CHECK(menu(events, sizeof events / sizeof *events) == 0); \
} while (0)

static int owner_input(void *unused) {
    (void)unused;
    mach_enter();
    while (!gex_save_menu_open()) {
        if (gex_host_ticks_ms() - menu_started > 5000) hle_fatal("save menu did not open");
        hle_block_begin();
        gex_host_delay(8);
        hle_block_end();
    }
    CHECK(gex_save_menu_open());
    if (!owner_back) {
        CHECK(gex_save_key(1, 0x28, 0));
        CHECK(gex_save_key(0, 0x28, 0));
    }
    unsigned key = owner_back ? 0x1B : 'X';
    CHECK(gex_save_key(1, key, 0));
    CHECK(gex_save_key(0, key, 0));
    g_eax = 0xDEADBEEF;
    g_ebx = 0x12345678;
    g_flag_a = 0x87654321;
    mach_leave();
    return 0;
}

static void encode(char password[GEX_PASSWORD_SIZE]) {
    memset(hle_ptr(0x4A2540), 0, 144);
    memset(hle_ptr(0x4A25D0), 0, 144);
    MEM32(0x4A2964) = 49;
    const unsigned completed[] = {3, 0, 1, 2, 6, 10};
    for (unsigned i = 0; i < sizeof completed / sizeof *completed; ++i)
        MEM8(0x4A2540 + completed[i]) = 3;
    MEM8(0x4A2547) = 1; /* Seventh normal stage unlocked: six completed. */
    MEM32(0x458C8C) = 7;
    uint32_t buffer = hle_va(hle_alloc(GEX_PASSWORD_SIZE));
    hle_call_guest(0x429200, 1, &buffer);
    memcpy(password, hle_ptr(buffer), GEX_PASSWORD_SIZE);
    CHECK(gex_password_valid(password));
    CHECK(gex_password_levels(password) == 6);
    CHECK(hle_call_guest(0x4295C0, 1, &buffer) == 1);
}

static void operations(const char *path, const char password[GEX_PASSWORD_SIZE]) {
    gex_save_store store, reopened;
    CHECK(!gex_save_store_open(&store, path));
    CHECK(store.available);
    CHECK(!gex_save_store_write(&store, 0, password));
    CHECK(!gex_save_store_copy(&store, 0, 2));
    CHECK(!strcmp(store.password[0], store.password[2]));
    CHECK(!store.password[1][0]);
    CHECK(gex_save_store_copy(&store, 1, 2) == -1);
    CHECK(gex_save_store_copy(&store, 0, 0) == -1);
    CHECK(gex_save_store_write(&store, 3, password) == -1);
    char bad[GEX_PASSWORD_SIZE];
    memcpy(bad, password, sizeof bad);
    bad[8] = 'X';
    CHECK(!gex_password_valid(bad));
    CHECK(gex_save_store_write(&store, 1, bad) == -1);
    bad[8] = 0;
    bad[0] = bad[0] == 'B' ? 'C' : 'B';
    CHECK(!gex_password_valid(bad));
    CHECK(!gex_save_store_erase(&store, 0));
    CHECK(!gex_save_store_open(&reopened, path));
    CHECK(!reopened.password[0][0] && !reopened.password[1][0]);
    CHECK(!strcmp(reopened.password[2], password));
    CHECK(!gex_file_remove(path));
    CHECK(!gex_save_store_open(&store, path));
    CHECK(!gex_directory_create(path));
    CHECK(gex_save_store_write(&store, 1, password) == -1);
    CHECK(!store.password[1][0]);
    CHECK(!gex_directory_remove(path));
    CHECK(!gex_save_store_write(&store, 1, password)); /* A failed write is retryable. */
}

static void validation(void) {
    uint32_t buffer = hle_va(hle_alloc(GEX_PASSWORD_SIZE));
    uint32_t source = hle_va(hle_alloc(3));
    const uint8_t invalid[][3] = {{0xE0,0,0}, {0x1F,0,0}, {0,0x90,0}, {0,0,6}, {0,0,1}};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        memcpy(hle_ptr(source), invalid[i], 3);
        const uint32_t args[] = {buffer + 2, source, 24};
        hle_call_guest(0x429850, 3, args);
        hle_call_guest(0x429690, 1, &buffer);
        CHECK(hle_call_guest(0x4295C0, 1, &buffer) == 1);
        CHECK(!gex_password_valid(hle_ptr(buffer)));
    }
    const unsigned worlds[] = {49, 50, 51, 54, 52, 53, 55};
    for (unsigned i = 0; i < sizeof worlds / sizeof *worlds; ++i) {
        MEM32(0x4A2964) = worlds[i];
        MEM8(0x4A2548) = i % 3 == 0 ? 3 : i % 3 == 1 ? 1 : 0;
        MEM8(0x4A257E) = 3;
        MEM32(0x458C8C) = i * 5;
        hle_call_guest(0x429200, 1, &buffer);
        CHECK(gex_password_valid(hle_ptr(buffer)));
        CHECK(gex_password_levels(hle_ptr(buffer)) == 6);
    }
}

static void options_input_test(void) {
    static const struct { unsigned key, action; } bindings[] = {
        {'C', 0x80}, {'Z', 0x20}, {'X', 0x40}, {'V', 4}
    };
    MEM32(0x487FF8) = 0;
    MEM32(0x455018) = UINT32_MAX;
    for (unsigned i = 0; i < sizeof bindings / sizeof *bindings; ++i) {
        hle_input_key(1, bindings[i].key, 0);
        CHECK((hle_call_guest(0x404BA0, 0, NULL) & 0xE4) == bindings[i].action);
        hle_input_key(0, bindings[i].key, 0);
        CHECK(!(hle_call_guest(0x404BA0, 0, NULL) & 0xE4));
    }
}

static void options_test(const char *mode, const char *settings) {
    CHECK(!hle_reg_load(settings));
    CHECK(!gex_options_load());
    if (!strcmp(mode, "options-read")) {
        int next = 0;
        for (int i = 1; i < 5; ++i)
            if (gex_renderer_available(i)) { next = i; break; }
        CHECK(gex_prefs.renderer == next && gex_prefs.api == GEX_API_AUTO);
    }
    CHECK(!gex_options_video_init(0, 320, 240));
    if (!strcmp(mode, "options-read")) {
        hle_call_guest(0x408B60, 0, NULL);
        for (unsigned i = 0; i < 3; ++i) CHECK(gex_prefs.volume[i] == 0);
        CHECK(gex_prefs.bindings[GEX_ACTION_LASH].key == 'Q');
        CHECK(gex_prefs.bindings[GEX_ACTION_HURL].key == 'W');
        CHECK(gex_prefs.bindings[GEX_ACTION_LASH].button == GEX_PAD_L);
        CHECK(gex_prefs.bindings[GEX_ACTION_HURL].button == GEX_PAD_R);
        CHECK(gex_prefs.bindings[GEX_ACTION_JUMP].key == 'X');
        CHECK(gex_prefs.bindings[GEX_ACTION_CONFIRM].key == 'X');
        options_input_test();
        return;
    }
    if (!strcmp(mode, "options-error")) {
        CHECK(!gex_file_remove(settings));
        CHECK(!gex_directory_create(settings));
        gex_options_begin();
        CHECK(!gex_options_pointer(60, 105)); /* Audio category. */
        CHECK(!gex_options_key(0x27)); /* Raise the Sound slider from zero. */
        CHECK(gex_prefs.volume[0] == 5);
        CHECK(!gex_options_key(0x1B)); /* Back returns to categories, not the title. */
        CHECK(!gex_options_key(0x1B)); /* Failed persistence keeps the menu open. */
        CHECK(!gex_directory_remove(settings));
        CHECK(gex_options_key(0x1B)); /* The same change can be retried. */
        gex_options_end();
        return;
    }
    uint32_t sentinel = hle_va(hle_alloc(4)), object = hle_va(hle_alloc(0x204));
    uint32_t label = hle_va(hle_alloc(sizeof "Password"));
    MEM32(sentinel) = 0;
    memset(hle_ptr(object), 0x5A, 0x204);
    MEM32(object) = sentinel;
    MEM32(object + 8) = 0x7B;
    MEM32(object + 0x98) = 3;
    MEM32(object + 0x9C) = label;
    memcpy(hle_ptr(label), "Password", sizeof "Password");
    for (unsigned at = 0x4A28A0; at < 0x4A2918; at += 12) MEM32(at) = sentinel;
    MEM32(0x4A28A0) = object;
    unsigned char before[0x204];
    memcpy(before, hle_ptr(object), sizeof before);
    gex_options_title_init();
    CHECK(!strcmp(hle_ptr(label), "Options"));
    CHECK(!memcmp(before, hle_ptr(object), sizeof before));
    gex_options_title_init();
    CHECK(!memcmp(before, hle_ptr(object), sizeof before));
    uint32_t platform = gex_options_platform();
    CHECK(platform && !(platform & (platform - 1)));
    CHECK(gex_options_platform_matches(GEX_OPTIONS_ALL));
    CHECK(gex_options_platform_matches(platform));
    CHECK(!gex_options_platform_matches(GEX_OPTIONS_ALL & ~platform));
    CHECK(!gex_options_platform_matches(0));
    uint32_t effects = MEM32(0x455000), voice = MEM32(0x455004);
    gex_options_begin();
    CHECK(!gex_options_scaled_text());
    CHECK(!gex_options_key(0x27)); /* Root left/right never changes a setting. */
    CHECK(MEM32(0x455000) == effects && MEM32(0x455004) == voice);
    uint32_t request = MEM32(0x48A034);
    CHECK(!gex_options_pointer(60, 65)); /* Open Controls, without playing Music Test. */
    CHECK(MEM32(0x48A034) == request);
    CHECK(!gex_options_pointer(60, 39)); /* Gameplay heading is not selectable. */
    CHECK(!gex_binding_key_filter);
    gex_options_end();
    MEM32(0x48A034) = 0; /* The worker has consumed the title's music request. */
    gex_options_begin();
    gex_options_end();
    CHECK(!MEM32(0x48A034)); /* Merely opening Options must not restart music. */
    gex_options_begin();
    CHECK(!gex_options_pointer(60, 105));
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned step = 0; step < 22; ++step) CHECK(!gex_options_key(0x25));
        CHECK(!MEM32(0x455000 + i * 4));
        CHECK(gex_prefs.volume[i] == 0);
        CHECK(!gex_options_key(0x28));
    }
    CHECK((int32_t)MEM32(0x49FB20) == -10000);
    CHECK((int32_t)MEM32(0x49FB50) == -10000);
    CHECK((int32_t)MEM32(0x48A030) == -10000);
    CHECK(!MEM32(0x455C0C));
    CHECK(!gex_options_key(0x27));
    CHECK(MEM32(0x48A034) == 12);
    CHECK(!gex_options_key(0x25));
    CHECK(MEM32(0x48A034) == 110);
    CHECK(!gex_options_key(0x25));
    CHECK(MEM32(0x48A034) == 239);
    CHECK(!gex_options_key(0x1B));
    CHECK(!gex_options_pointer(60, 65));
    CHECK(!gex_options_key(0x0D));
    CHECK(gex_binding_key_filter != NULL);
    CHECK(gex_binding_key_filter(1, 'X', 0x2D)); /* Reject a binding used by Jump. */
    CHECK(gex_binding_key_filter != NULL);
    CHECK(gex_binding_key_filter(1, 'Q', 0x10));
    CHECK(!gex_binding_key_filter);
    CHECK(gex_prefs.bindings[GEX_ACTION_LASH].key == 'Q');
    CHECK(!gex_options_key(0x27)); /* Select the gamepad column. */
    CHECK(!gex_options_key(0x0D));
    CHECK(gex_binding_pad_filter != NULL);
    CHECK(gex_binding_pad_filter(GEX_PAD_L));
    CHECK(!gex_binding_pad_filter);
    CHECK(!gex_options_pointer(70, 180)); /* Hurl has its own keyboard binding. */
    CHECK(gex_binding_key_filter && gex_binding_key_filter(1, 'W', 0x11));
    CHECK(!gex_options_pointer(210, 180));
    CHECK(gex_binding_pad_filter && gex_binding_pad_filter(GEX_PAD_R));
    CHECK(gex_prefs.bindings[GEX_ACTION_HURL].key == 'W');
    options_input_test();
    for (unsigned i = 1; i < 8; ++i) CHECK(!gex_options_key(0x28));
    CHECK(!gex_options_key(0x0D)); /* Submenu Exit only returns one level. */
    CHECK(gex_options_key(0x1B));
    gex_options_end();
    CHECK(MEM32(0x48A034) == 110);
    gex_options_begin();
    CHECK(!gex_options_pointer(319, 239));
    CHECK(gex_options_pointer(60, 195));
    gex_options_end();
    gex_options_begin();
    CHECK(!gex_options_key(0x26)); /* Up wraps to Exit. */
    CHECK(!gex_options_key(0x25));
    CHECK(!gex_options_key(0x27));
    CHECK(gex_options_key(0x0D));
    gex_options_end();
    gex_options_begin();
    CHECK(!gex_options_pointer(60, 140)); /* Video category. */
    CHECK(!gex_options_pointer(60, 150));
    CHECK(gex_prefs.display == GEX_DISPLAY_BORDERLESS);
    CHECK(!gex_options_pointer(60, 150));
    CHECK(gex_prefs.display == GEX_DISPLAY_EXCLUSIVE);
    CHECK(!gex_options_pointer(60, 150));
    CHECK(gex_prefs.display == GEX_DISPLAY_WINDOWED);
    CHECK(!gex_options_key(0x28));
    CHECK(!gex_options_key(0x28));
    CHECK(!gex_options_key(0x27)); /* Software must not claim triple-buffer support. */
    CHECK(!gex_prefs.triple);
    static const int triple_apis[] = {GEX_API_VULKAN, GEX_API_METAL, GEX_API_D3D11, GEX_API_D3D12};
    for (unsigned i = 0; i < sizeof triple_apis / sizeof *triple_apis; ++i) {
        gex_prefs.active_renderer = 4;
        gex_prefs.active_api = triple_apis[i];
        CHECK(!gex_options_key(0x27) && gex_prefs.triple);
        CHECK(!gex_options_key(0x27) && !gex_prefs.triple);
    }
    gex_prefs.active_renderer = 0;
    gex_prefs.active_api = GEX_API_AUTO;
    unsigned seen = 1, choices = 0;
    for (unsigned i = 0; i < 3; ++i) CHECK(!gex_options_key(0x26));
    do {
        CHECK(!gex_options_key(0x27));
        CHECK(gex_renderer_available(gex_prefs.renderer));
        seen |= 1u << gex_prefs.renderer;
        if (gex_prefs.renderer == 4 && gex_prefs.api)
            CHECK(gex_prefs.supported_apis & (1u << gex_prefs.api));
    } while ((gex_prefs.renderer || gex_prefs.api) && ++choices < 16);
    CHECK(choices < 16);
    for (int i = 0; i < 5; ++i)
        CHECK(((seen >> i) & 1) == (unsigned)gex_renderer_available(i));
    CHECK(!gex_options_key(0x27)); /* Persist the next compiled backend. */
    CHECK(!gex_options_key(0x1B));
    CHECK(gex_options_key(0x1B));
    gex_options_end();
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) return 2;
    recomp_host_init();
    if (recomp_host_boot_permissive(argv[1])) return 2;
    mach_enter();
    CHECK(!gex_renderer_init(gex_renderer_backend_get(), 1));
    CHECK(!gex_renderer_resize(320, 240));
    char path[4096], settings[4096];
    snprintf(path, sizeof path, "%s/passwords.sav", argv[3]);
    snprintf(settings, sizeof settings, "%s/settings.reg", argv[3]);
    char password[GEX_PASSWORD_SIZE];
    encode(password);
    if (!strcmp(argv[2], "operations")) {
        validation();
        operations(path, password);
    } else if (!strcmp(argv[2], "corrupt")) {
        gex_save_store store;
        CHECK(gex_save_store_open(&store, path) == -1);
        CHECK(!store.available);
        CHECK(gex_save_store_write(&store, 0, password) == -1);
    } else {
        CHECK(!gex_save_init(settings, pump));
        const unsigned choose_second[] = {0x28, 'X'};
        if (!strcmp(argv[2], "options-close") || !strcmp(argv[2], "options-close-consumed")) {
            menu_started = gex_host_ticks_ms();
            close_request = 1;
            consume_close_request = !strcmp(argv[2], "options-close-consumed");
            hle_on_exit = options_closed;
            gex_save_options();
            CHECK(0); /* A window close must not resume the title scene callback. */
        } else if (!strncmp(argv[2], "options-", 8)) {
            options_test(argv[2], settings);
        } else if (!strcmp(argv[2], "write")) {
            CHECK(menu(choose_second, 2) == 1);
            CHECK(!MEM8(0x4A0200));
            uint32_t buffer = hle_va(hle_alloc(GEX_PASSWORD_SIZE));
            memcpy(hle_ptr(buffer), password, sizeof password);
            uint32_t stack = g_esp, ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp;
            gex_save_checkpoint(buffer);
            CHECK(g_esp == stack && g_ebx == ebx && g_esi == esi && g_edi == edi && g_ebp == ebp);
            gex_save_store reopened;
            CHECK(!gex_save_store_open(&reopened, path));
            CHECK(!reopened.password[0][0] && !reopened.password[2][0]);
            CHECK(!strcmp(reopened.password[1], password));
        } else if (!strcmp(argv[2], "read")) {
            CHECK(menu(choose_second, 2) == 1);
            CHECK(!strcmp(hle_ptr(0x4A0200), password));
            memset(hle_ptr(0x4A2540), 0xFF, 144);
            memset(hle_ptr(0x4A25D0), 0xFF, 144);
            MEM32(0x458C8C) = 0;
            hle_call_guest(0x429940, 0, NULL);
            CHECK(MEM32(0x4A2964) == 49);
            CHECK(MEM32(0x4A2A74) == 55);
            CHECK(MEM32(0x458C8C) == 7);
            CHECK(MEM8(0x4A2543) == 3 && MEM8(0x4A254A) == 3);
            CHECK(MEM8(0x4A2547) == 1 && MEM8(0x4A2549) == 0);
        } else if (!strcmp(argv[2], "threaded") || !strcmp(argv[2], "threaded-back")) {
            owner_back = !strcmp(argv[2], "threaded-back");
            uint32_t stack = g_esp, eax = g_eax, ebx = g_ebx, flags = g_flag_a;
            SDL_Thread *input = SDL_CreateThread(owner_input, "save-owner-input", NULL);
            CHECK(input != NULL);
            if (input) {
                CHECK(menu(NULL, 0) == (owner_back ? 0 : 1));
                SDL_WaitThread(input, NULL);
                if (owner_back) CHECK(gex_host_ticks_ms() - menu_started >= 1000);
                else CHECK(!strcmp(hle_ptr(0x4A0200), password));
                CHECK(g_esp == stack && g_eax == eax && g_ebx == ebx && g_flag_a == flags);
            }
        } else if (!strcmp(argv[2], "transition")) {
            gex_render_frame frame;
            CHECK(!gex_renderer_frame(&frame));
            memset(frame.pixels, 255, (size_t)frame.pitch * frame.height);
            previous_light = (uint64_t)frame.width * frame.height * 125;
            transition_test = 1;
            gex_renderer_observe(transition_frame);
            if (argc == 5) capture = argv[4];
            MENU(0x1B);
            CHECK(gex_host_ticks_ms() - menu_started >= 1000);
            CHECK(saw_black == 3);
            for (unsigned direction = 0; direction < 2; ++direction)
                CHECK(fade_out_frames[direction] >= 4 && fade_in_frames[direction] >= 4);
            CHECK(previous_light == (uint64_t)frame.width * frame.height * 125);
            for (int y = 0; y < frame.height; ++y)
                for (int x = 0; x < frame.width * 2; ++x)
                    CHECK(frame.pixels[y * frame.pitch + x] == 255);
            CHECK(!capture);
            gex_renderer_observe(NULL);
            transition_test = 0;
            close_after_back = 1;
            MENU(0x1B);
            CHECK(!close_after_back);
            CHECK(gex_save_take_close_request());
            MENU(0x1B); /* An interrupted return must release its captured title. */
        } else if (!strcmp(argv[2], "menu")) {
            /* Copy deck 2 to deck 3, cancel erasing it, then confirm erasing deck 2. */
            MENU(0x28, 0x27, 0x27, 'X', 0x28, 'X', 0x1B);
            MENU(0x28, 0x28, 0x27, 'X', 'X', 0x1B);
            gex_save_store current;
            CHECK(!gex_save_store_open(&current, path));
            CHECK(!strcmp(current.password[1], password));
            CHECK(!strcmp(current.password[2], password));
            MENU(0x28, 0x27, 'X', 0x27, 'X', 0x1B);
            CHECK(!gex_save_store_open(&current, path));
            CHECK(!current.password[1][0] && !strcmp(current.password[2], password));
            /* Copy overwrite defaults to cancel, then explicit confirmation. */
            MENU(0x28, 0x28, 0x27, 0x27, 'X', 0x26, 0x26, 'X', 0x1B);
            MENU(0x28, 0x28, 0x27, 0x27, 'X', 0x26, 0x26, 'X', 'X', 0x1B, 0x1B);
            MENU(0x28, 0x28, 0x27, 0x27, 'X', 0x26, 0x26, 'X', 0x27, 'X', 0x1B);
            /* An occupied deck requires confirmation before manual replacement. */
            MENU('V', 'X', 0x1B);
            const unsigned manual[] = {'V', 0x27, 'X'};
            CHECK(menu(manual, 3) == 2);
            CHECK(!gex_save_store_open(&current, path));
            CHECK(!strcmp(current.password[0], password));
            if (argc == 5) capture = argv[4];
            MENU(0x1B);
            CHECK(!gex_save_take_close_request());
            touch_exit = 1;
            CHECK(menu(NULL, 0) == 0);
            close_request = 1;
            CHECK(menu(NULL, 0) == 0);
            CHECK(gex_save_take_close_request());
            CHECK(!gex_save_take_close_request());
            close_request = 1;
            CHECK(menu(NULL, 0) == 0);
            MEM32(0x487F88) = 1;
            MEM32(0x487F74) = MEM32(0x451794) = 0;
            MEM32(0x45103C) = 0;
            MEM32(0x4A2970) = 2;
            MEM32(0x4A2A84) = MEM32(0x4A294C) = 0;
            CHECK(hle_call_guest(0x4052D0, 0, NULL) == 1);
            CHECK(!MEM32(0x4A294C)); /* Quit must not start a retail pause. */
            CHECK(MEM32(0x487F88) == 1);
            CHECK(!gex_save_take_close_request());
            const unsigned start[] = {'X'};
            leave_held = 1;
            CHECK(menu(start, 1) == 1);
            leave_held = 0;
            CHECK(gex_save_key(1, 'X', 0)); /* Held confirm must not reach gameplay. */
            CHECK(gex_save_key(0, 'X', 0));
            CHECK(!gex_save_key(1, 'X', 0));
            CHECK(!gex_save_key(0, 'X', 0));
        } else return 2;
        gex_save_shutdown();
    }
    gex_renderer_shutdown();
    mach_leave();
    return failures ? 1 : 0;
}
