#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include "platform/gl/context.h"
#include "renderer.h"

gex_gl_gen_fn gex_gl_gen_framebuffers;
gex_gl_delete_fn gex_gl_delete_framebuffers;
gex_gl_bind_fn gex_gl_bind_framebuffer;
gex_gl_attach_fn gex_gl_attach_texture;
gex_gl_status_fn gex_gl_framebuffer_status;
static SDL_Window *window, *caller_window;
static SDL_GLContext context, caller_context;
static int video_owned, headless = 1, attached;
void gex_gl_set_headless(int value) { headless = value; }

int gex_gl_enter(char *error, size_t size) {
    caller_window = SDL_GL_GetCurrentWindow();
    caller_context = SDL_GL_GetCurrentContext();
    if (!SDL_GL_MakeCurrent(window, context))
        return 0;
    snprintf(error, size, "cannot acquire fixed-function GL context: %s", SDL_GetError());
    return -1;
}
int gex_gl_leave(char *error, size_t size) {
    if (!SDL_GL_MakeCurrent(caller_context ? caller_window : window, caller_context))
        return 0;
    snprintf(error, size, "cannot release fixed-function GL context: %s", SDL_GetError());
    return -1;
}
void gex_gl_close(void) {
    if (attached)
        gex_renderer_device_window(NULL);
    attached = 0;
    if (context)
        SDL_GL_DeleteContext(context);
    if (window)
        SDL_DestroyWindow(window);
    context = NULL;
    window = NULL;
    if (video_owned)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    video_owned = 0;
}
int gex_gl_open(char *error, size_t size) {
    SDL_Window *saved_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext saved_context = SDL_GL_GetCurrentContext();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO))
        goto failed;
    video_owned = 1;
    if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0) ||
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0) || SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0))
        goto failed;
    window = SDL_CreateWindow(
        "Gex - OpenGL 1.2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (headless ? SDL_WINDOW_HIDDEN : 0));
    if (window)
        context = SDL_GL_CreateContext(window);
    if (!context)
        goto failed;
#define LOAD(field, name)                                                                          \
    do {                                                                                           \
        void *address = SDL_GL_GetProcAddress(name "EXT");                                         \
        if (!address)                                                                              \
            address = SDL_GL_GetProcAddress(name);                                                 \
        memcpy(&field, &address, sizeof address);                                                  \
        if (!field) {                                                                              \
            snprintf(error, size, "missing " name);                                                \
            goto failed_restore;                                                                   \
        }                                                                                          \
    } while (0)
    LOAD(gex_gl_gen_framebuffers, "glGenFramebuffers");
    LOAD(gex_gl_delete_framebuffers, "glDeleteFramebuffers");
    LOAD(gex_gl_bind_framebuffer, "glBindFramebuffer");
    LOAD(gex_gl_attach_texture, "glFramebufferTexture2D");
    LOAD(gex_gl_framebuffer_status, "glCheckFramebufferStatus");
#undef LOAD
    if (SDL_GL_SetSwapInterval(0))
        fprintf(stderr, "[opengl12] swap control unavailable: %s\n", SDL_GetError());
    if (SDL_GL_MakeCurrent(saved_context ? saved_window : window, saved_context))
        goto failed;
    if (!headless) {
        gex_renderer_device_window(window);
        attached = 1;
    }
    return 0;
failed:
    snprintf(error, size, "fixed-function GL initialization failed: %s", SDL_GetError());
failed_restore:
    if (saved_context && SDL_GL_MakeCurrent(saved_window, saved_context))
        fprintf(stderr, "[opengl12] caller context restore failed: %s\n", SDL_GetError());
    gex_gl_close();
    return -1;
}
int gex_gl_submit(void) {
    glFinish();
    return 0;
}
int gex_gl_swap(void) {
    SDL_GL_SwapWindow(window);
    return 0;
}
int gex_gl_width(void) {
    int width;
    SDL_GL_GetDrawableSize(window, &width, NULL);
    return width;
}
int gex_gl_height(void) {
    int height;
    SDL_GL_GetDrawableSize(window, NULL, &height);
    return height;
}
int gex_gl_fbo_top(void) { return 0; }
