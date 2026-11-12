#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL_syswm.h>
#include "platform/bgfx/packet_policy.h"
#include "preferences.h"

static SDL_Thread *thread;

static int requested_api(gex_bgfx_request *r, bgfx_renderer_type_t *type) {
    static const struct { const char *name; bgfx_renderer_type_t type; } names[] = {
        {"auto", BGFX_RENDERER_TYPE_COUNT}, {"vulkan", BGFX_RENDERER_TYPE_VULKAN},
        {"metal", BGFX_RENDERER_TYPE_METAL}, {"opengl", BGFX_RENDERER_TYPE_OPENGL},
        {"opengles", BGFX_RENDERER_TYPE_OPENGLES},
        {"direct3d11", BGFX_RENDERER_TYPE_DIRECT3D11},
        {"direct3d12", BGFX_RENDERER_TYPE_DIRECT3D12}
    };
    const char *name = SDL_getenv("GEX_BGFX_API");
    if (!name) name = gex_api_name(gex_prefs.api);
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i)
        if (!strcmp(name, names[i].name)) {
            *type = names[i].type;
            return 0;
        }
    snprintf(r->error, r->size, "bgfx: GEX_BGFX_API requires auto, vulkan, metal, opengl, opengles, direct3d11 or direct3d12");
    return -1;
}

static int window(SDL_Window *window, bgfx_platform_data_t *platform,
                  void **native_window, void **native_display, char *error, size_t size) {
    if (!window) return 0;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) {
        snprintf(error, size, "bgfx native window handle: %s", SDL_GetError());
        return -1;
    }
    switch (info.subsystem) {
#if defined(SDL_VIDEO_DRIVER_X11)
    case SDL_SYSWM_X11:
        *native_display = info.info.x11.display;
        *native_window = (void *)(uintptr_t)info.info.x11.window;
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
        *native_display = info.info.wl.display;
        *native_window = info.info.wl.surface;
        platform->type = BGFX_NATIVE_WINDOW_HANDLE_TYPE_WAYLAND;
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
    case SDL_SYSWM_WINDOWS: *native_window = info.info.win.window; break;
#endif
#if defined(SDL_VIDEO_DRIVER_COCOA)
    case SDL_SYSWM_COCOA: *native_window = info.info.cocoa.window; break;
#endif
#if defined(SDL_VIDEO_DRIVER_ANDROID)
    case SDL_SYSWM_ANDROID: *native_window = info.info.android.window; break;
#endif
    default:
        snprintf(error, size, "bgfx: unsupported SDL window subsystem %d", info.subsystem);
        return -1;
    }
    return 0;
}

static int thread_start(int (*worker)(void *), char *error, size_t size) {
    int thread_error = worker ? EAGAIN : ENOMEM;
    thread = worker ? SDL_CreateThread(worker, "bgfx-api", NULL) : NULL;
    if (thread) return 0;
    snprintf(error, size, "bgfx: cannot create the graphics API worker: %s", strerror(thread_error));
    return -1;
}
static void thread_join(void) { SDL_WaitThread(thread, NULL); thread = NULL; }
static void after_execute(gex_bgfx_request *r) { (void)r; }
static gex_bgfx_shader_binary shader(int kind) {
    (void)kind;
    return (gex_bgfx_shader_binary){NULL, 0};
}
static int open_extra(gex_bgfx_policy_context *ctx, gex_bgfx_request *r) {
    (void)ctx; (void)r;
    return 0;
}
static void close_extra(void) {}
static int execute_extra(gex_bgfx_policy_context *ctx, gex_bgfx_request *r) {
    return ctx->fail(r, "unknown worker operation");
}
const gex_gpu_native_ops gex_bgfx_packet_native_ops = {0};

const gex_bgfx_packet_policy *gex_bgfx_packet_platform(void) {
    static const gex_bgfx_packet_policy policy = {
        0, "cannot initialise the platform GPU backend", NULL,
        requested_api, window, thread_start, thread_join, after_execute, shader,
        open_extra, close_extra, execute_extra
    };
    return &policy;
}
