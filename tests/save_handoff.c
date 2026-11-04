/* Drive the actual title callback and inspect every presented handoff frame. */
#define main gex_save_game_main
#define gex_platform_pump gex_save_test_pump
#include "../src/main.c"
#undef gex_platform_pump
#undef main

void gex_platform_pump(void);
static int step, observing;
static unsigned black_frames, destination_frames;
static uint32_t selected_at;

static int has_callback(uint32_t callback) {
    for (uint32_t list = 0x4A28A0; list < 0x4A2918; list += 12)
        for (uint32_t object = MEM32(list); object && MEM32(object); object = MEM32(object))
            if (MEM32(object + 0x60) == callback) return 1;
    return 0;
}

static void capture_frame(const gex_render_frame *frame, const char *name) {
    const char *directory = getenv("GEX_SAVE_HANDOFF_CAPTURE");
    if (!directory) return;
    char path[4096];
    if (snprintf(path, sizeof path, "%s/%s.bmp", directory, name) >= (int)sizeof path ||
        gex_host_screenshot(path, frame->pixels, frame->width, frame->height, frame->pitch))
        hle_fatal("Save handoff capture failed");
}

static void check(int condition, const char *message) {
    if (!condition) hle_fatal("Save handoff regression: %s", message);
}

static void handoff_frame(const gex_render_frame *frame) {
    observe_frame(frame);
    if (step < 3 || gex_save_modal_open()) return;
    check(frame->pixels != NULL, "readable headless frame");
    int visible = 0;
    for (int y = 0; y < frame->height; ++y)
        for (int x = 0; x < frame->width * 2; ++x)
            visible |= frame->pixels[y * frame->pitch + x];
    if (getenv("GEX_SAVE_HANDOFF_MANUAL")) {
        check(has_callback(0x40E6A0), "retail manual entry initialized");
        if (visible && ++destination_frames >= 12) {
            capture_frame(frame, "manual");
            fprintf(stderr, "[save-handoff] manual password entry remains visible\n");
            hle_exit(0);
        }
        return;
    }
    /* The requested level changes before the loaded title scene retires. */
    if (MEM32(0x4A2A98) == 63) {
        if (visible) {
            const char *path = getenv("GEX_SAVE_HANDOFF_FAILURE");
            if (path) check(!gex_host_screenshot(path, frame->pixels, frame->width,
                                                frame->height, frame->pitch), "failure capture");
        }
        check(!visible, "title must stay black after selecting a deck");
        if (!black_frames) capture_frame(frame, "black");
        ++black_frames;
    } else if (visible && ++destination_frames >= 12) {
        unsigned expected = getenv("GEX_SAVE_HANDOFF_OCCUPIED") ? 49 : 55;
        check(MEM32(0x4A2964) == expected, "selected deck reaches its destination");
        check(MEM32(0x4A2A98) == expected, "destination scene is loaded");
        check(black_frames != 0, "observed outgoing title frames");
        capture_frame(frame, getenv("GEX_SAVE_HANDOFF_OCCUPIED") ?
                      "destination-occupied" : "destination-empty");
        fprintf(stderr, "[save-handoff] no title flash; %u black frames, destination %u\n",
                black_frames, expected);
        hle_exit(0);
    }
}

void gex_save_test_pump(void) {
    gex_platform_pump();
    if (!observing) {
        gex_renderer_observe(handoff_frame);
        observing = 1;
    }
    if (!step) {
        if (MEM32(0x4A2964) != 63 || scene_frames < 75) return;
        uint32_t controller = 0;
        for (uint32_t list = 0x4A28A0; list < 0x4A2918; list += 12)
            for (uint32_t object = MEM32(list); object && MEM32(object); object = MEM32(object))
                if (MEM32(object + 0x60) == 0x40DEF0) controller = object;
        check(controller != 0, "title controller exists");
        MEM32(controller + 0xB0) = 1;
        hle_input_key(1, 'X', 0x2D);
        step = 1;
    } else if (step == 1 && gex_save_modal_open()) {
        hle_input_key(0, 'X', 0x2D);
        step = 2;
    } else if (step == 2 && gex_save_menu_open()) {
        if (getenv("GEX_SAVE_HANDOFF_OCCUPIED")) {
            check(gex_save_key(1, 0x28, 0), "select occupied deck");
            check(gex_save_key(0, 0x28, 0), "release Down");
        }
        unsigned key = getenv("GEX_SAVE_HANDOFF_MANUAL") ? 'V' : 'X';
        check(gex_save_key(1, key, 0), "confirm deck or manual entry");
        check(gex_save_key(0, key, 0), "release confirm");
        selected_at = gex_host_ticks_ms();
        step = 3;
    } else if (step == 3) {
        check(gex_host_ticks_ms() - selected_at < 4000, "destination becomes visible");
    }
}

int main(int argc, char **argv) {
    int code = gex_save_game_main(argc, argv);
    check(destination_frames >= 12, "completed save handoff");
    return code;
}
