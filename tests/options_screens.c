/* Exercise real title/Options drawing; only the platform pump adds scripted input. */
#define main gex_options_game_main
#define gex_platform_pump gex_options_test_pump
#include "../src/main.c"
#undef gex_platform_pump
#undef main

void gex_platform_pump(void);
static int step;
static uint32_t step_started;
static int restart_renderer, restart_api;

static void down(unsigned count) {
    while (count--) gex_options_key(0x28);
}

static void check(int condition, const char *message) {
    if (!condition) hle_fatal("Options screen regression: %s", message);
}

static void capture_screen(const char *name) {
    const char *directory = getenv("GEX_OPTIONS_CAPTURE_DIR");
    if (!directory) return;
    char path[4096];
    gex_render_frame frame;
    check(snprintf(path, sizeof path, "%s/%s.bmp", directory, name) < (int)sizeof path, "capture path");
    check(!gex_renderer_frame(&frame), "capture frame");
    check(!gex_host_screenshot(path, frame.pixels, frame.width, frame.height, frame.pitch), "capture pixels");
}

void gex_options_test_pump(void) {
    gex_platform_pump();
    if (getenv("GEX_OPTIONS_RELOAD_ONLY")) {
        check(gex_prefs.width == 320 && gex_prefs.height == 240, "saved resolution restored");
        if (gex_prefs.renderer == 4)
            check(gex_prefs.api == gex_prefs.active_api, "saved graphics API restored");
        fprintf(stderr, "[options-screens] saved video settings restored\n");
        hle_exit(0);
        return;
    }
    if (!step) {
        if (MEM32(0x4A2964) != 63 || scene_frames < 75) return;
        uint32_t controller = 0;
        for (uint32_t list = 0x4A28A0; list < 0x4A2918; list += 12)
            for (uint32_t object = MEM32(list); object && MEM32(object); object = MEM32(object))
                if (MEM32(object + 0x60) == 0x40DEF0) controller = object;
        check(controller != 0, "title controller exists");
        MEM32(controller + 0xB0) = 3;
        hle_input_key(1, 'X', 0x2D);
        step = -1;
        step_started = gex_host_ticks_ms();
        return;
    }
    if (step == -1) {
        if (gex_save_modal_open() || gex_host_ticks_ms() - step_started > 150)
            hle_input_key(0, 'X', 0x2D);
        if (gex_save_modal_open()) {
            step = 1;
            step_started = gex_host_ticks_ms();
        }
        return;
    }
    if (step == 19) {
        if (gex_save_modal_open()) return;
        step = 20;
        step_started = scene_frames;
        return;
    }
    if (step == 20) {
        if (scene_frames - step_started < 2) return;
        if (getenv("GEX_OPTIONS_TEST_RESIZE")) {
            gex_render_frame frame;
            check(!gex_renderer_frame(&frame), "resized title frame");
            check(frame.width == 320 && frame.height == 240, "resolution applied after return");
        }
        capture_screen("returned-title");
        fprintf(stderr, "[options-screens] category navigation, scrolling, sliders and title round trip passed\n");
        hle_exit(0);
        return;
    }
    if (gex_host_ticks_ms() - step_started < (step == 1 ? 700u : 350u)) return;
    step_started = gex_host_ticks_ms();
    if (step == 1) {
        capture_screen("categories");
        check(!gex_options_pointer(60, 65), "open Controls");
    } else if (step == 2) {
        capture_screen("controls");
        down(3);
    } else if (step == 3) {
        capture_screen("controls-middle");
        down(3);
    } else if (step == 4) {
        capture_screen("controls-menus");
        down(2);
    } else if (step == 5) {
        capture_screen("controls-bottom");
        check(!gex_options_pointer(60, 192), "scrolled Controls Exit");
        check(!gex_options_pointer(60, 65), "reopen Controls at top");
        check(!gex_options_key(0x26), "wrap Controls to Exit");
    } else if (step == 6) {
        capture_screen("controls-wrap-bottom");
        down(1);
    } else if (step == 7) {
        capture_screen("controls-wrap-top");
        check(!gex_options_key(0x1B), "Controls Back");
        check(!gex_options_pointer(60, 105), "open Audio");
    } else if (step == 8) {
        capture_screen("audio-full");
        check(!gex_options_pointer_event(55, 98, 1), "slider press");
        check(!gex_options_pointer_event(165, 98, -1), "slider drag");
        check(!gex_options_pointer_event(165, 98, 0), "slider release");
        check(gex_prefs.volume[0] == 50, "slider is fifty percent");
    } else if (step == 9) {
        capture_screen("audio-half");
        down(2);
    } else if (step == 10) {
        capture_screen("audio-scrolled");
        check(!gex_options_pointer_event(55, 190, 1), "scrolled Music slider press");
        check(gex_prefs.volume[2] == 0, "scrolled slider minimum");
        check(!gex_options_pointer_event(500, 250, -1), "drag outside viewport");
        check(gex_prefs.volume[2] == 100, "scrolled slider maximum");
        check(!gex_options_pointer_event(165, 190, -1), "scrolled Music slider drag");
        check(!gex_options_pointer_event(165, 190, 0), "scrolled Music slider release");
        check(gex_prefs.volume[2] == 50 && gex_prefs.volume[1] == 100, "correct scrolled slider changed");
        check(!gex_options_pointer_event(275, 190, -1), "released slider ignores motion");
        check(gex_prefs.volume[2] == 50, "release ends scrolled slider drag");
    } else if (step == 11) {
        capture_screen("audio-scrolled-half");
        down(1);
    } else if (step == 12) {
        capture_screen("audio-music-test");
        check(!gex_options_key(0x27), "change Music Test");
        check(MEM32(0x48A034) == 12, "original Music Test sequence");
        down(1);
    } else if (step == 13) {
        capture_screen("audio-bottom");
        check(!gex_options_pointer(60, 192), "scrolled Audio Exit");
        check(!gex_options_pointer(60, 150), "open Video");
    } else if (step == 14) {
        capture_screen("video");
        restart_renderer = gex_prefs.renderer;
        restart_api = gex_prefs.api;
        check(!gex_options_key(0x27), "change backend for restart notice");
        down(2);
    } else if (step == 15) {
        capture_screen("video-resolution");
        if (getenv("GEX_OPTIONS_TEST_RESIZE")) {
            check(!gex_options_key(0x27), "change scrolled resolution");
            check(gex_prefs.width == 320 && gex_prefs.height == 240, "select desktop-aspect resolution");
        }
        down(1);
    } else if (step == 16) {
        capture_screen("video-vsync");
        down(1);
    } else if (step == 17) {
        capture_screen("video-bottom");
        check(!gex_options_pointer(60, 180), "scrolled Video Exit");
        check(!gex_options_pointer(60, 150), "reopen Video at top");
        int desired_api = getenv("GEX_OPTIONS_TEST_RESIZE") && restart_renderer == 4 ?
            gex_prefs.active_api : restart_api;
        unsigned choices = 0;
        while (gex_prefs.renderer != restart_renderer || gex_prefs.api != desired_api) {
            check(++choices <= 16, "original backend and graphics API are selectable");
            check(!gex_options_key(0x27), "restore backend after restart notice");
        }
        check(!gex_options_key(0x1B), "Video Back");
    } else if (step == 18) {
        capture_screen("categories-return");
        check(gex_save_key(1, 0x1B, 0), "Options Back");
        check(gex_save_key(0, 0x1B, 0), "release Options Back");
    }
    if (step < 19) ++step;
}

int main(int argc, char **argv) { return gex_options_game_main(argc, argv); }
