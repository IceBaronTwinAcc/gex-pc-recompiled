#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "win32hle.h"
#include "platform_shims.h"
#include "movie.h"
#include "identity.h"
#include "autoplay.h"
#include "gameplay.h"
#include "gamepad.h"
#include "renderer.h"
#include "graphics_guest.h"
#include "packet_gpu.h"
#include "platform/host.h"
#include "platform/filesystem.h"
#include "preferences.h"
#ifdef GEX_DESKTOP_SETUP
#include "game_install.h"
#endif
#if GEX_ENABLE_PASSWORD_SAVES
#include "save_menu.h"
#include "options_menu.h"
#include "platform/input.h"
#endif

static uint32_t started;
static unsigned smoke_seconds, frames, changes;
static int autoplay, verify_gameplay;
static gex_gameplay_state gameplay;
static uint32_t frame_level = UINT32_MAX;
static unsigned scene_frames;
static gex_autoplay_state autoplay_state;
static uint64_t last_hash;
static char screenshot[4096];
static char settings_file[4096];
static const char *const renderer_names[] = {"software", "opengl", "pvr", "opengl12", "bgfx"};
#if GEX_ENABLE_PASSWORD_SAVES
static int menu_input(void) {
    uint32_t level = MEM32(0x4A2964);
    return gex_save_modal_open() || MEM32(0x4A294C) || level >= 49;
}
#endif

static void record_frame(const uint8_t *px, int w, int h, int pitch, int bpp) {
    uint64_t hash = 1469598103934665603ull;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w * bpp / 8; ++x)
            hash = (hash ^ px[y * pitch + x]) * 1099511628211ull;
    if (frames && hash != last_hash) ++changes;
    last_hash = hash;
    ++frames;
    uint32_t level = MEM32(0x4A2964);
    if (level != frame_level) {
        fprintf(stderr, "[smoke] scene=%u elapsed-ms=%u\n", level,
                gex_host_ticks_ms() - started);
        scene_frames = 0;
        frame_level = level;
    }
    ++scene_frames;
    if (verify_gameplay) {
        uint32_t player = level == 3 && autoplay_state.motion_started ? MEM32(0x4A27FC) : 0;
        gex_gameplay_record(&gameplay, level, hash, player,
            player ? (int32_t)MEM32(player + 0x78) : 0,
            player ? (int32_t)MEM32(player + 0x7C) : 0);
    }
}

static void observe_frame(const gex_render_frame *frame) {
    if (!frame->pixels) {
        ++frames;
        uint32_t level = MEM32(0x4A2964);
        if (level != frame_level) {
            fprintf(stderr, "[smoke] scene=%u elapsed-ms=%u presentations=%u\n",
                    level, gex_host_ticks_ms() - started, frames);
            frame_level = level; scene_frames = 0;
        }
        ++scene_frames;
        return;
    }
    record_frame(frame->pixels, frame->width, frame->height, frame->pitch, 16);
}

static void monitor(void) {
    gex_platform_pump();
    gex_movie_pump();
    if (gex_renderer_pump()) hle_fatal("renderer event pump failed: %s", gex_renderer_error());
    gex_gamepad_pump();
    unsigned elapsed_ms = gex_host_ticks_ms() - started;
    unsigned elapsed = elapsed_ms / 1000;
#if GEX_ENABLE_PASSWORD_SAVES
    static int deck_autoplay;
    static uint32_t deck_started;
    int choosing_deck = gex_save_menu_open();
    if (autoplay && choosing_deck) {
        if (!deck_autoplay) { deck_started = elapsed_ms; deck_autoplay = 1; }
        if (elapsed_ms - deck_started >= 100) gex_save_key(0, 'X', 0x2D);
        if (elapsed_ms - deck_started >= 300) {
            gex_save_key(1, 'X', 0x2D);
            deck_autoplay = 2;
        }
    } else if (deck_autoplay) {
        gex_save_key(0, 'X', 0x2D);
        deck_autoplay = 0;
    }
#else
    int choosing_deck = 0;
#endif
    gex_autoplay_event event;
    if (autoplay && !choosing_deck && gex_autoplay_next(&autoplay_state, elapsed_ms, MEM32(0x4A2964),
                                     frame_level == MEM32(0x4A2964) ? scene_frames : 0, &event) &&
        (!verify_gameplay || event.key != 0x1B)) {
#if GEX_ENABLE_PASSWORD_SAVES
        if (!gex_save_key(event.down, event.key, event.scan))
#endif
            hle_input_key(event.down, event.key, event.scan);
        fprintf(stderr, "[smoke] %u ms level=%u key 0x%X %s\n", elapsed_ms,
                MEM32(0x4A2964), event.key, event.down ? "down" : "up");
    }
    int verified = verify_gameplay &&
        gex_gameplay_passed(&gameplay, MEM32(0x4A2964), MEM32(0x4A27FC));
    if (smoke_seconds && (elapsed >= smoke_seconds || verified)) {
        gex_render_frame frame;
        if (gex_renderer_frame(&frame)) hle_fatal("no framebuffer at smoke deadline");
        if (screenshot[0]) {
            if (gex_host_screenshot(screenshot, frame.pixels, frame.width,
                                   frame.height, frame.pitch))
                hle_fatal("screenshot capture failed");
        }
        if (gex_graphics_gpu_owned())
            fprintf(stderr, "[smoke] gpu-presentations=%u; readbacks forbidden; pixel checks unavailable\n", frames);
        else fprintf(stderr, "[smoke] frames=%u changes=%u checksum=%016llX\n",
                     frames, changes, (unsigned long long)last_hash);
        int passed = frames && (gex_graphics_gpu_owned() || changes);
        if (verify_gameplay) {
            uint32_t level = MEM32(0x4A2964);
            int gameplay_passed = gex_gameplay_passed(&gameplay, level, MEM32(0x4A27FC));
            fprintf(stderr, "[smoke] first-graveyard=%s level-index=%u frames=%u changes=%u\n",
                    gameplay_passed ? "PASS" : "FAIL", level, gameplay.frames, gameplay.changes);
            fprintf(stderr, "[smoke] player=0x%08X x=[%d,%d] y=[%d,%d]\n",
                    gameplay.player, gameplay.min_x, gameplay.max_x, gameplay.min_y, gameplay.max_y);
            passed = passed && gameplay_passed;
        }
        fflush(NULL);
        _Exit(passed ? 0 : 3);
    }
}

static void early_gameplay_exit(int code) {
    fprintf(stderr, "[smoke] first-graveyard=FAIL guest exited (%d) before the verification deadline\n", code);
    fflush(NULL);
    _Exit(code ? code : 3);
}

int main(int argc, char **argv) {
    const char *data = "Gex";
    int headless = 0, trace = 0, check = 0, explicit_data = 0;
    int skip_movies = gex_host_skip_movies_by_default();
    const int default_renderer = GEX_ENABLE_PVR_RENDERER ? 2 : GEX_ENABLE_BGFX_RENDERER ? 4 :
                                 GEX_ENABLE_SOFTWARE_RENDERER ? 0 : GEX_ENABLE_GLDC_RENDERER ? 3 : 1;
    int hardware_renderer = default_renderer;
    int render_width = 0, render_height = 0;
    int explicit_renderer = 0, explicit_size = 0;
#ifdef GEX_NATIVE_GAMEPLAY_WIDTH
    render_width = GEX_NATIVE_GAMEPLAY_WIDTH;
    render_height = GEX_NATIVE_GAMEPLAY_HEIGHT;
#endif
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) {
            data = argv[++i];
            explicit_data = 1;
        }
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--skip-movies")) skip_movies = 1;
        else if (!strcmp(argv[i], "--hardware-renderer")) {
            explicit_renderer = 1;
            hardware_renderer = GEX_ENABLE_PVR_RENDERER ? 2 :
                GEX_ENABLE_BGFX_RENDERER ? 4 : GEX_ENABLE_GLDC_RENDERER ? 3 : 1;
        }
        else if (!strcmp(argv[i], "--renderer")) {
            explicit_renderer = 1;
            if (i + 1 == argc) {
                fprintf(stderr, "[gex] --renderer requires software, opengl, pvr, opengl12 or bgfx\n");
                return 64;
            }
            const char *name = argv[++i];
            if (!strcmp(name, "software")) hardware_renderer = 0;
            else if (!strcmp(name, "opengl")) hardware_renderer = 1;
            else if (!strcmp(name, "pvr")) hardware_renderer = 2;
            else if (!strcmp(name, "opengl12")) hardware_renderer = 3;
            else if (!strcmp(name, "bgfx")) hardware_renderer = 4;
            else {
                fprintf(stderr, "[gex] --renderer requires software, opengl, pvr, opengl12 or bgfx\n");
                return 64;
            }
        }
        else if (!strcmp(argv[i], "--render-size")) {
            explicit_size = 1;
            if (i + 1 == argc || gex_renderer_parse_size(argv[++i], &render_width, &render_height)) {
                fprintf(stderr, "[gex] --render-size requires WIDTHxHEIGHT: 1..8192 per axis, at most 16777216 pixels\n");
                return 64;
            }
        }
        else if (!strcmp(argv[i], "--autoplay")) autoplay = 1;
        else if (!strcmp(argv[i], "--verify-gameplay")) verify_gameplay = 1;
        else if (!strcmp(argv[i], "--smoke-seconds") && i + 1 < argc) {
            char *end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end || value < 1 || value > 3600) {
                fprintf(stderr, "[gex] --smoke-seconds must be between 1 and 3600\n");
                return 64;
            }
            smoke_seconds = (unsigned)value;
        }
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            const char *path = argv[++i];
            if (gex_fs_path_absolute(path)) {
                if (snprintf(screenshot, sizeof screenshot, "%s", path) >= (int)sizeof screenshot) {
                    fprintf(stderr, "[gex] screenshot path is too long\n");
                    return 64;
                }
            }
            else {
                if (gex_host_working_directory(screenshot, sizeof screenshot)) return 1;
                size_t n = strlen(screenshot);
                if (snprintf(screenshot + n, sizeof screenshot - n, "/%s", path) >= (int)(sizeof screenshot - n))
                    return 64;
            }
        }
        else {
            fprintf(stderr, "Usage: %s [--data GOG-directory] [--headless] [--trace] [--check]\n"
                "       [--skip-movies] [--smoke-seconds 1..3600] [--screenshot file.bmp] [--autoplay]\n"
                "       [--verify-gameplay] [--render-size WIDTHxHEIGHT] [--renderer software|opengl|pvr|opengl12|bgfx]\n"
                "       [--hardware-renderer (alias for this platform's packet GPU)]\n"
                "       Built packet renderers: %s%s%s%s%s\n"
                "       Default packet renderer: %s\n", argv[0],
                GEX_ENABLE_SOFTWARE_RENDERER ? "software " : "",
                GEX_ENABLE_OPENGL_RENDERER ? "opengl " : "",
                GEX_ENABLE_PVR_RENDERER ? "pvr " : "",
                GEX_ENABLE_GLDC_RENDERER ? "opengl12 " : "",
                GEX_ENABLE_BGFX_RENDERER ? "bgfx" : "", renderer_names[default_renderer]);
            return !strcmp(argv[i], "--help") ? 0 : 64;
        }
    }
    if ((hardware_renderer == 1 && !GEX_ENABLE_OPENGL_RENDERER) ||
        (hardware_renderer == 2 && !GEX_ENABLE_PVR_RENDERER) ||
        (hardware_renderer == 3 && !GEX_ENABLE_GLDC_RENDERER) ||
        (hardware_renderer == 4 && !GEX_ENABLE_BGFX_RENDERER) ||
        (!hardware_renderer && !GEX_ENABLE_SOFTWARE_RENDERER)) {
        fprintf(stderr, "[gex] %s renderer was excluded at build time\n",
                renderer_names[hardware_renderer]);
        return 64;
    }
#ifdef GEX_NATIVE_GAMEPLAY_WIDTH
    if (render_width != GEX_NATIVE_GAMEPLAY_WIDTH || render_height != GEX_NATIVE_GAMEPLAY_HEIGHT) {
        fprintf(stderr, "[gex] this platform requires native %dx%d gameplay\n",
                GEX_NATIVE_GAMEPLAY_WIDTH, GEX_NATIVE_GAMEPLAY_HEIGHT);
        return 64;
    }
#endif
#ifdef GEX_DREAMCAST_PVR_MOVIES
    if (!check && hardware_renderer != 2) {
        fprintf(stderr, "[gex] Dreamcast FFmpeg builds require --renderer pvr; other renderers need a separate movie_metadata.c build with --skip-movies\n");
        return 64;
    }
#endif
#ifdef GEX_DREAMCAST_SKIP_MOVIES
    if (!skip_movies && !check) {
        fprintf(stderr, "[gex] metadata-only Dreamcast build requires --skip-movies; use the PVR/FFmpeg build for playback\n");
        return 64;
    }
#endif
    if ((screenshot[0] || autoplay) && !smoke_seconds) {
        fprintf(stderr, "[gex] --screenshot and --autoplay require --smoke-seconds\n");
        return 64;
    }
    if (autoplay && !skip_movies) {
        fprintf(stderr, "[gex] timed --autoplay requires --skip-movies\n");
        return 64;
    }
    if (verify_gameplay && (!headless || !autoplay || !smoke_seconds || check)) {
        fprintf(stderr, "[gex] --verify-gameplay requires --headless, --autoplay and a smoke deadline, without --check\n");
        return 64;
    }
#ifdef GEX_DESKTOP_SETUP
    if (!explicit_data) {
        static char default_data[4096];
        if (gex_default_game_folder(default_data, sizeof default_data)) {
            fprintf(stderr, "[setup] cannot resolve the default game folder\n");
            return 1;
        }
        data = default_data;
        const char *video = getenv("SDL_VIDEODRIVER");
        int setup = gex_prepare_game(data, !headless && !check && !smoke_seconds &&
                                    (!video || strcmp(video, "dummy")));
        if (setup) return setup == 1 ? 0 : 1;
    }
#else
    (void)explicit_data;
#endif
    if (gex_host_enter_directory(data)) return 1;
    if (gex_verify_executable("GEX.exe")) return 1;
    gex_host_install_diagnostics();
    win32hle_trace = trace;
    recomp_host_init();
    gex_register_platform_shims();
    gex_movie_register(skip_movies);
    int rc = check ? recomp_host_boot("GEX.exe") : recomp_host_boot_permissive("GEX.exe");
    if (rc) return rc;
    char command[128];
    snprintf(command, sizeof command, "GEX.exe %s", (const char *)hle_ptr(0x454FD0));
    hle_set_command_line(command);
    fprintf(stderr, "[gex] %u statically recompiled functions; original x86 is never executed\n",
            recomp_dispatch_count);
    if (check) return 0;
    int settings_mode = gex_host_settings_path(settings_file, sizeof settings_file);
    if (settings_mode < 0) return 1;
    if (!settings_mode && hle_reg_load(settings_file)) return 1;
#if GEX_ENABLE_PASSWORD_SAVES
    mach_enter();
    int options_error = gex_options_load();
    mach_leave();
    if (options_error) return 1;
    if (!explicit_renderer && gex_prefs.renderer >= 0) hardware_renderer = gex_prefs.renderer;
    if (!explicit_size && gex_prefs.width) {
        render_width = gex_prefs.width; render_height = gex_prefs.height;
    }
    gex_controls_menu_hook = menu_input;
    if (gex_save_init(settings_file, monitor)) return 1;
#else
    (void)explicit_renderer; (void)explicit_size;
#endif
    hardware_renderer = gex_host_renderer_preference(hardware_renderer, explicit_renderer);
    const gex_renderer_backend *renderer = gex_renderer_backend_get();
#if GEX_ENABLE_BGFX_RENDERER
    if (hardware_renderer == 4) renderer = gex_bgfx_renderer();
#endif
    if (gex_renderer_init(renderer, headless) || gex_renderer_resize(320, 240))
        hle_fatal("renderer initialization failed: %s", gex_renderer_error());
#if GEX_ENABLE_PASSWORD_SAVES
    if (gex_options_video_init(hardware_renderer, render_width, render_height)) goto renderer_failed;
    render_width = gex_prefs.width; render_height = gex_prefs.height;
#endif
    if (render_width) {
        if (gex_graphics_set_render_size(render_width, render_height)) goto renderer_failed;
        fprintf(stderr, "[renderer] level viewport %dx%d (one logical pixel per render pixel)\n",
                render_width, render_height);
    }
    if (hardware_renderer == 3 && gex_packet_gpu_select_gldc(headless)) goto renderer_failed;
    if (hardware_renderer == 4 && gex_packet_gpu_select_bgfx()) goto renderer_failed;
    if (hardware_renderer && gex_graphics_enable_gpu()) goto renderer_failed;
    if (gex_graphics_gpu_owned() && (screenshot[0] || verify_gameplay)) {
        fprintf(stderr, "[gex] GPU-owned output cannot provide CPU screenshots or pixel-based "
                        "--verify-gameplay; capture and verify emulator scanout externally\n");
        gex_graphics_shutdown();
        gex_renderer_shutdown();
        return 64;
    }
    fprintf(stderr, "[renderer] selected %s packet backend\n", renderer_names[hardware_renderer]);
    if (!headless && gex_gamepad_init(0)) goto renderer_failed;
    if (!headless && gex_host_prepare_playback()) goto renderer_failed;
    started = gex_host_ticks_ms();
    if (smoke_seconds) gex_renderer_observe(observe_frame);
    hle_set_pump_hook(monitor);
    monitor();
    if (verify_gameplay) hle_on_exit = early_gameplay_exit;
    else hle_on_exit = gex_host_game_exit_callback();
    rc = (int)recomp_host_run();
#if GEX_ENABLE_PASSWORD_SAVES
    gex_save_shutdown();
#endif
    if (!headless) gex_gamepad_shutdown();
    gex_graphics_shutdown();
    gex_renderer_shutdown();
    if (verify_gameplay) early_gameplay_exit(rc);
    return rc;

renderer_failed:
    gex_graphics_shutdown();
    gex_renderer_shutdown();
    return 1;
}
