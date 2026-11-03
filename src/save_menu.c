#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "win32hle.h"
#include "save_store.h"
#include "save_menu.h"
#include "options_menu.h"
#include "renderer.h"
#include "graphics_guest.h"
#include "platform/host.h"
#include "platform/input.h"
#include "save_menu_art.h"

static gex_save_store store;
static void (*pump_host)(void);
static int initialized, opened, close_requested, active_deck = -1;
/* The retail quit handler consumes close_requested before the modal unwinds. */
static int modal_close_requested;
static int selection, copy_source = -1, confirmation, yes, result, notice_only;
static char message[256];
static uint8_t held[256], blocked[256];
static uint16_t *pixels;
enum { FADE_MS = 250 };

/* Five-column host UI font; the cabinet, labels and selections are extracted art. */
static const uint8_t font[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},{0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},
    {6,0x49,0x49,0x29,0x1E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},{0x7F,9,9,9,1},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,8,8,8,0x7F},{0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,1},
    {0x7F,8,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,2,0xC,2,0x7F},{0x7F,4,8,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,9,9,9,6},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{1,1,0x7F,1,1},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,8,0x14,0x63},{7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43}
};

static uint32_t read32(size_t at) {
    if (at + 4 > sizeof gex_save_art) hle_fatal("invalid embedded save menu");
    return gex_save_art[at] | (uint32_t)gex_save_art[at + 1] << 8 |
        (uint32_t)gex_save_art[at + 2] << 16 | (uint32_t)gex_save_art[at + 3] << 24;
}

static uint16_t color(unsigned index) {
    unsigned at = 8 + index * 2;
    unsigned value = gex_save_art[at] | gex_save_art[at + 1] << 8;
    return (uint16_t)(((value & 31) << 11) | ((value & 0x3E0) << 1) | ((value >> 10) & 31));
}

static void rectangle(int x, int y, int width, int height, uint16_t value) {
    for (int row = y; row < y + height; ++row)
        for (int column = x; column < x + width; ++column)
            pixels[row * 320 + column] = value;
}

static void text(int x, int y, const char *label, uint16_t value) {
    int start = x;
    for (; *label; ++label) {
        if (*label == '\n' || x > 302) { x = start; y += 9; }
        if (y > 231) break;
        if (*label == '\n') continue;
        int glyph = *label >= '0' && *label <= '9' ? *label - '0' :
            *label >= 'A' && *label <= 'Z' ? *label - 'A' + 10 : -1;
        if (glyph >= 0)
            for (int column = 0; column < 5; ++column)
                for (int row = 0; row < 7; ++row)
                    if (font[glyph][column] & (1u << row))
                        pixels[(y + row) * 320 + x + column] = value;
        x += 6;
    }
}

static void render(void) {
    for (unsigned i = 0; i < 76800; ++i) pixels[i] = color(gex_save_art[520 + i]);
    size_t at = read32(77320 + (unsigned)selection * 4);
    for (;;) {
        uint32_t offset = read32(at);
        at += 4;
        if (offset == UINT32_MAX) break;
        if (at + 2 > sizeof gex_save_art) hle_fatal("invalid save menu patch");
        unsigned count = gex_save_art[at] | gex_save_art[at + 1] << 8;
        at += 2;
        if (offset >= 76800 || count > 76800 - offset || count > sizeof gex_save_art - at)
            hle_fatal("invalid save menu patch bounds");
        for (unsigned i = 0; i < count; ++i) pixels[offset + i] = color(gex_save_art[at + i]);
        at += count;
    }
    const int top[] = {52, 97, 135};
    for (unsigned deck = 0; deck < GEX_SAVE_SLOTS; ++deck) {
        /* Copy the blank LCD column across its numeric field, keeping its gradient. */
        for (int y = top[deck]; y < top[deck] + 13; ++y)
            for (int x = 132; x < 149; ++x) pixels[y * 320 + x] = pixels[y * 320 + 129];
        char count[4];
        snprintf(count, sizeof count, "%u", gex_password_levels(store.password[deck]));
        for (size_t digit = 0; digit < strlen(count); ++digit)
            for (int x = 0; x < 5; ++x)
                for (int y = 0; y < 11; ++y)
                    if (font[count[digit] - '0'][x] & (1u << (y * 7 / 11)))
                        pixels[(top[deck] + 1 + y) * 320 +
                               145 - strlen(count) * 6 + digit * 6 + x] = 0x1040;
    }
    const uint16_t gold = 0xF5C8, white = 0xFFFF;
    if (message[0] || confirmation || copy_source >= 0)
        rectangle(12, 208, 296, 28, 0x0841);
    if (message[0]) {
        char upper[sizeof message];
        for (size_t i = 0; i < sizeof upper; ++i) {
            unsigned char c = (unsigned char)message[i];
            upper[i] = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
            if (!c) break;
        }
        text(18, 211, upper, white);
    } else if (confirmation) {
        text(18, 211, confirmation == 1 ? "ERASE THIS DECK" :
             confirmation == 2 ? "REPLACE DESTINATION DECK" : "REPLACE DECK WITH MANUAL PASSWORD", white);
        text(65, 224, "CANCEL", gold);
        text(190, 224, "YES", gold);
        rectangle(yes ? 188 : 63, 222, yes ? 22 : 40, 1, white);
    } else if (copy_source >= 0) {
        text(18, 211, "CHOOSE DESTINATION DECK", gold);
        text(18, 223, "X CONFIRM   C CANCEL", white);
    }
}

static void present(void) {
    if (gex_graphics_gpu_owned()) {
        gex_graphics_movie((const uint8_t *)pixels, 320, 240, 640);
    } else {
        gex_render_frame frame;
        if (gex_renderer_frame(&frame)) hle_fatal("save menu framebuffer unavailable");
        gex_render_rect fit = gex_renderer_fit(frame.width, frame.height, 320, 240);
        memset(frame.pixels, 0, (size_t)frame.pitch * frame.height);
        for (int y = 0; y < fit.height; ++y)
            for (int x = 0; x < fit.width; ++x) {
                uint16_t value = pixels[(y * 240 / fit.height) * 320 + x * 320 / fit.width];
                uint8_t *destination = frame.pixels + (y + fit.y) * frame.pitch + (x + fit.x) * 2;
                destination[0] = (uint8_t)value;
                destination[1] = (uint8_t)(value >> 8);
            }
        if (gex_renderer_commit()) hle_fatal("save menu presentation: %s", gex_renderer_error());
    }
}

static void show_error(void) {
    snprintf(message, sizeof message, "SAVE FAILED\n%.110s\nPRESS X TO RETURN", store.error);
}

static void finish(int action) {
    active_deck = selection / 3;
    if (action == 1)
        memcpy(hle_ptr(0x4A0200), store.password[active_deck], GEX_PASSWORD_SIZE);
    result = action;
}

static void activate(void) {
    unsigned deck = (unsigned)selection / 3;
    if (message[0]) {
        message[0] = 0;
        if (notice_only) result = 0;
    } else if (confirmation) {
        int operation = confirmation;
        confirmation = 0;
        if (!yes) return;
        int failed = 0;
        if (operation == 1) failed = gex_save_store_erase(&store, deck);
        else if (operation == 2) {
            failed = gex_save_store_copy(&store, (unsigned)copy_source, deck);
            copy_source = -1;
        } else finish(2);
        if (failed) show_error();
    } else if (selection == 9) {
        result = 0;
    } else if (copy_source >= 0) {
        if (deck == (unsigned)copy_source) return;
        if (store.password[deck][0]) { confirmation = 2; yes = 0; }
        else {
            if (gex_save_store_copy(&store, (unsigned)copy_source, deck)) show_error();
            copy_source = -1;
        }
    } else if (selection % 3 == 0) {
        finish(1);
    } else if (!store.password[deck][0]) {
        snprintf(message, sizeof message, "THIS DECK IS EMPTY\nPRESS X TO RETURN");
    } else if (selection % 3 == 1) {
        confirmation = 1;
        yes = 0;
    } else {
        copy_source = (int)deck;
    }
}

int gex_save_key(int down, uint32_t key, uint32_t scan) {
    (void)scan;
    if (opened && key == UINT32_MAX) {
        modal_close_requested = close_requested = 1;
        if (opened != 2 || gex_options_key(key)) result = 0;
        return 1;
    }
    if (key >= 256) return opened;
    int was_down = held[key];
    held[key] = down != 0;
    if (!opened) {
        int consumed = blocked[key];
        if (!down) blocked[key] = 0;
        return consumed;
    }
    if (!down || was_down) return 1;
    if (opened == 3) return 1;
    if (opened == 2) {
        if (gex_options_key(key)) result = 0;
        return 1;
    }
    if (key == 0x1B || key == 'C' || key == 'Z') {
        if (message[0]) { message[0] = 0; if (notice_only) result = 0; }
        else if (confirmation) confirmation = 0;
        else if (copy_source >= 0) copy_source = -1;
        else result = 0;
    } else if (key == 'X' || key == 0x0D || key == 0x20) {
        activate();
    } else if (!message[0] && confirmation) {
        if (key >= 0x25 && key <= 0x28) yes = !yes;
    } else if (!message[0] && key == 'V' && copy_source < 0 && selection < 9) {
        if (store.password[selection / 3][0]) { confirmation = 3; yes = 0; }
        else finish(2);
    } else if (!message[0] && key >= 0x25 && key <= 0x28) {
        if (key == 0x26) selection = selection == 9 ? 6 : selection < 3 ? 9 : selection - 3;
        if (key == 0x28) selection = selection == 9 ? 0 : selection >= 6 ? 9 : selection + 3;
        if (key == 0x25 && selection < 9) selection = selection / 3 * 3 + (selection + 2) % 3;
        if (key == 0x27 && selection < 9) selection = selection / 3 * 3 + (selection + 1) % 3;
    }
    return 1;
}

static int pointer(int x, int y, int down) {
    if (!opened) return 0;
    if (opened == 3) return 1;
    if (down <= 0) return 1;
    if (opened == 2) {
        if (gex_graphics_screen_point(&x, &y) && gex_options_pointer(x, y)) result = 0;
        return 1;
    }
    gex_render_frame frame;
    if (gex_renderer_frame(&frame)) hle_fatal("save menu pointer target unavailable");
    gex_render_rect fit = gex_renderer_fit(frame.width, frame.height, 320, 240);
    if (x < fit.x || y < fit.y || x >= fit.x + fit.width || y >= fit.y + fit.height) return 1;
    x = (x - fit.x) * 320 / fit.width;
    y = (y - fit.y) * 240 / fit.height;
    if (message[0]) { activate(); return 1; }
    if (confirmation) {
        if (y >= 220 && ((x >= 40 && x < 130) || (x >= 170 && x < 240))) {
            yes = x >= 170;
            activate();
        }
        return 1;
    }
    if (y >= 220 && x >= 12 && x <= 308 && copy_source < 0 && selection < 9) {
        if (store.password[selection / 3][0]) { confirmation = 3; yes = 0; }
        else finish(2);
        return 1;
    }
    if (x >= 225 && x <= 277 && y >= 160 && y <= 192) selection = 9;
    else {
        int row = y >= 29 && y < 75 ? 0 : y >= 75 && y < 115 ? 1 :
            y >= 115 && y < 151 ? 2 : -1;
        int column = x >= 175 && x < 211 ? 0 : x >= 211 && x < 252 ? 1 :
            x >= 252 && x < 296 ? 2 : -1;
        if (row < 0 || column < 0) return 1;
        selection = row * 3 + column;
    }
    activate();
    return 1;
}

static int run_menu(int notice) {
    if (notice != 2 && !pixels) pixels = malloc(76800 * sizeof *pixels);
    if (notice != 2 && !pixels) hle_fatal("cannot allocate save menu framebuffer");
    int menu_kind = notice == 2 ? 2 : 1;
    int animated = notice != 1;
    modal_close_requested = 0;
    opened = animated ? 3 : menu_kind;
    notice_only = notice;
    result = -1;
    for (unsigned key = 0; key < 256; ++key)
        if (held[key]) hle_input_key(0, key, 0);
    fprintf(stderr, menu_kind == 2 ? "[options] Options menu opened\n" : "[saves] Video Deck menu opened\n");
    enum { TITLE_OUT, MENU_IN, MENU_IDLE, MENU_OUT, TITLE_IN };
    int transition = opened == 3 ? TITLE_OUT : MENU_IDLE;
    uint32_t fade_started = gex_host_ticks_ms();
    if (animated) gex_graphics_fade_capture();
    uint32_t last = gex_host_ticks_ms() - 34;
    for (;;) {
        if (animated && !modal_close_requested && opened == menu_kind && result == 0) {
            opened = 3;
            transition = MENU_OUT;
            result = -1;
            fade_started = gex_host_ticks_ms();
            last = fade_started - 34;
        }
        if (result >= 0) break;
        if (gex_host_ticks_ms() - last >= 33) {
            uint32_t now = gex_host_ticks_ms();
            unsigned elapsed = now - fade_started;
            if (elapsed > FADE_MS) elapsed = FADE_MS;
            unsigned shade = transition == TITLE_OUT || transition == MENU_OUT ?
                255 * (FADE_MS - elapsed) / FADE_MS : 255 * elapsed / FADE_MS;
            if (transition == TITLE_OUT || transition == TITLE_IN)
                gex_graphics_fade_present(shade);
            else if (menu_kind == 2) {
                gex_graphics_frame_shade(transition == MENU_IDLE ? 255 : shade);
                gex_options_render();
                gex_graphics_frame_shade(255);
            } else {
                render();
                if (opened == 3) {
                    for (unsigned i = 0; i < 76800; ++i) {
                        unsigned value = pixels[i];
                        pixels[i] = (uint16_t)((((value >> 11) * shade / 255) << 11) |
                            (((value >> 5 & 63) * shade / 255) << 5) |
                            ((value & 31) * shade / 255));
                    }
                }
                present();
            }
            if (elapsed == FADE_MS) {
                if (transition == TITLE_OUT) transition = MENU_IN;
                else if (transition == MENU_IN) { transition = MENU_IDLE; opened = menu_kind; }
                else if (transition == MENU_OUT) transition = TITLE_IN;
                else if (transition == TITLE_IN) { transition = MENU_IDLE; result = 0; }
                fade_started = gex_host_ticks_ms();
            }
            last = gex_host_ticks_ms();
        }
        pump_host();
        /* Let the SDL owner and guest audio timers take the machine while waiting. */
        hle_block_begin();
        gex_host_delay(8);
        hle_block_end();
    }
    if (animated) gex_graphics_fade_release();
    for (unsigned key = 0; key < 256; ++key) blocked[key] |= held[key];
    opened = 0;
    free(pixels);
    pixels = NULL;
    return result;
}

int gex_save_choose(void) {
    if (!initialized) return 1;
    selection = active_deck >= 0 ? active_deck * 3 : 0;
    confirmation = yes = 0;
    copy_source = -1;
    message[0] = 0;
    return run_menu(0);
}

void gex_save_options(void) {
    if (!initialized) return;
    gex_options_begin();
    run_menu(2);
    gex_options_end();
    if (modal_close_requested) hle_exit(0);
}

int gex_save_menu_open(void) { return opened == 1; }

int gex_save_take_close_request(void) {
    int requested = close_requested;
    close_requested = 0;
    return requested;
}

int gex_save_init(const char *settings_path, void (*pump)(void)) {
    char path[4096];
    const char *slash = strrchr(settings_path, '/');
    const char *backslash = strrchr(settings_path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    size_t prefix = slash ? (size_t)(slash - settings_path) + 1 : 0;
    if (prefix + sizeof "passwords.sav" > sizeof path) {
        fprintf(stderr, "[saves] save path is too long\n");
        return -1;
    }
    memcpy(path, settings_path, prefix);
    strcpy(path + prefix, "passwords.sav");
    if (gex_save_store_open(&store, path)) return -1;
    pump_host = pump;
    initialized = 1;
    modal_close_requested = close_requested = 0;
    active_deck = -1;
    memset(held, 0, sizeof held);
    memset(blocked, 0, sizeof blocked);
    gex_input_key_filter = gex_save_key;
    gex_input_pointer_filter = pointer;
    fprintf(stderr, "[saves] three password decks: %s\n", path);
    return 0;
}

void gex_save_checkpoint(uint32_t address) {
    if (!initialized) return;
    const char *password = hle_ptr(address);
    if (active_deck < 0) hle_fatal("password earned without a selected Video Deck");
    if (!gex_password_valid(password)) hle_fatal("retail encoder produced an invalid password");
    if (!memcmp(store.password[active_deck], password, GEX_PASSWORD_SIZE)) return;
    if (gex_save_store_write(&store, (unsigned)active_deck, password)) {
        selection = active_deck * 3;
        show_error();
        run_menu(1);
    } else fprintf(stderr, "[saves] checkpoint saved to deck %d (%u levels)\n",
                   active_deck + 1, gex_password_levels(password));
}

void gex_save_shutdown(void) {
    initialized = 0;
    modal_close_requested = close_requested = 0;
    gex_input_key_filter = NULL;
    gex_input_pointer_filter = NULL;
    free(pixels);
    pixels = NULL;
}
