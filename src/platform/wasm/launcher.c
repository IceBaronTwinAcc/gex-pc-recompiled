#include <stdio.h>
#include <emscripten.h>
#include "platform/host.h"
#include "gamepad.h"
#include "graphics_guest.h"
#include "renderer.h"

#if GEX_ENABLE_BGFX_RENDERER && GEX_ENABLE_SOFTWARE_RENDERER
EM_ASYNC_JS(int, browser_webgpu_available, (void), {
    if (typeof OffscreenCanvas === "undefined") {
        console.warn("[renderer] OffscreenCanvas is unavailable; using software");
        return 0;
    }
    if (!navigator.gpu) {
        console.warn("[renderer] WebGPU is not available in this browser; using software");
        return 0;
    }
    try {
        const timed = async (promise, onLateValue) => {
            let timer, expired = false;
            const operation = Promise.resolve(promise).then(value => {
                if (expired && onLateValue) onLateValue(value);
                return value;
            });
            try {
                return await Promise.race([operation, new Promise((resolve, reject) => {
                    timer = setTimeout(() => {
                        expired = true;
                        reject(new Error("WebGPU request timed out"));
                    }, 10000);
                })]);
            } finally {
                clearTimeout(timer);
            }
        };
        const adapter = await timed(navigator.gpu.requestAdapter({powerPreference: "high-performance"}));
        if (!adapter) {
            console.warn("[renderer] WebGPU returned no compatible adapter; using software");
            return 0;
        }
        if (!adapter.features.has("indirect-first-instance")) {
            console.warn("[renderer] WebGPU adapter lacks indirect-first-instance; using software");
            return 0;
        }
        const device = await timed(
            adapter.requestDevice({requiredFeatures: ["indirect-first-instance"]}),
            lateDevice => lateDevice.destroy());
        device.destroy();
        return 1;
    } catch (error) {
        console.warn("[renderer] WebGPU adapter/device check failed; using software:", error);
        return 0;
    }
});
#endif

int gex_host_renderer_preference(int renderer, int explicit_renderer) {
#if GEX_ENABLE_BGFX_RENDERER && GEX_ENABLE_SOFTWARE_RENDERER
    if (!explicit_renderer) {
        int canvas_supported = MAIN_THREAD_EM_ASM_INT({
            const canvas = document.getElementById("canvas");
            return !!canvas && typeof canvas.transferControlToOffscreen === "function";
        });
        if (!canvas_supported) {
            fprintf(stderr, "[renderer] OffscreenCanvas transfer unavailable; selecting software\n");
            return 0;
        }
        if (!browser_webgpu_available()) {
            fprintf(stderr, "[renderer] WebGPU unavailable; selecting software "
                            "(see browser console for adapter details)\n");
            return 0;
        }
    }
#else
    (void)explicit_renderer;
#endif
    return renderer;
}

static void browser_game_exit(int code) {
    (void)code;
    gex_gamepad_shutdown();
    gex_graphics_shutdown();
    gex_renderer_shutdown();
    MAIN_THREAD_ASYNC_EM_ASM({
        window.gexSetStatus("Gex has exited. Reload the page to play again.");
    });
}

void (*gex_host_game_exit_callback(void))(int code) { return browser_game_exit; }
