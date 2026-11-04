#include <kos.h>
#include "win32hle.h"
#include "platform/video.h"
#include "preferences.h"

int gex_video_display_count(void) { return 1; }
int gex_video_display(int mode, char *error, size_t size) {
    if (mode == GEX_DISPLAY_BORDERLESS) return 0;
    snprintf(error, size, "Dreamcast requires fullscreen");
    return -1;
}
int gex_video_desktop(int *width, int *height) { *width = 640; *height = 480; return 0; }

int (*hle_screen_key_hook)(int, int);
int (*hle_screen_cursor_hook)(void);
void (*hle_screen_frame_hook)(const uint8_t *, int, int, int, int);
int (*hle_screen_compose_hook)(const uint8_t *, int, int, int, uint32_t *);
int (*hle_screen_overlay_hook)(const uint8_t *, int, int, int, uint32_t *,
                               int *, int *, int *, int *);
void (*hle_screen_settings_hook)(const char *, int, int);

int hle_screen_open(const char *title, int fullscreen, int headless) {
    (void)title; (void)fullscreen; (void)headless;
    return 0;
}
void hle_screen_pump(void) { thd_pass(); }
void hle_screen_close(void) {}
void hle_screen_title(const char *title) { fprintf(stderr, "[dreamcast] title: %s\n", title); }
int hle_screen_scale(const char *name) { return name && !strcmp(name, "nearest"); }
void hle_screen_bars(int blur) {
    if (blur) fprintf(stderr, "[dreamcast] blurred borders unavailable; using black borders\n");
}
void hle_screen_fullscreen(int on) {
    if (!on) fprintf(stderr, "[dreamcast] windowed output is unavailable\n");
}
const char *hle_screen_settings(int *bars, int *fullscreen) {
    if (bars) *bars = 0;
    if (fullscreen) *fullscreen = 1;
    return "nearest";
}
void hle_screen_menu(const hle_menu_item_t *items, int n, void (*chosen)(int)) {
    (void)items; (void)n; (void)chosen;
    fprintf(stderr, "[dreamcast] desktop popup menus are unavailable\n");
}
int hle_screen_menu_open(void) { return 0; }
int hle_screen_desktop(int *w, int *h) { *w = 640; *h = 480; return 1; }
