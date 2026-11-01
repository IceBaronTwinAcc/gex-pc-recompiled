#include <kos.h>
#include <stdlib.h>
#include "platform/gl/context.h"

gex_gl_gen_fn gex_gl_gen_framebuffers = glGenFramebuffersEXT;
gex_gl_delete_fn gex_gl_delete_framebuffers = glDeleteFramebuffersEXT;
gex_gl_bind_fn gex_gl_bind_framebuffer = glBindFramebufferEXT;
gex_gl_attach_fn gex_gl_attach_texture = glFramebufferTexture2DEXT;
gex_gl_status_fn gex_gl_framebuffer_status = glCheckFramebufferStatusEXT;
void gex_gl_set_headless(int headless) { (void)headless; }
int gex_gl_open(char *error, size_t size) {
    static int initialized;
    if (initialized)
        return 0;
    if (atexit(glKosShutdown)) {
        snprintf(error, size, "cannot register GLdc shutdown");
        return -1;
    }
    GLdcConfig config;
    glKosInitConfig(&config);
    config.autosort_enabled = GL_FALSE;
    config.initial_op_capacity = 0;
    config.initial_tr_capacity = GEX_GL_BATCH_COMMANDS;
    config.initial_pt_capacity = 0;
    config.initial_immediate_capacity = 64;
    glKosInitEx(&config);
    initialized = 1;
    return 0;
}
int gex_gl_enter(char *error, size_t size) {
    (void)error;
    (void)size;
    return 0;
}
int gex_gl_leave(char *error, size_t size) {
    (void)error;
    (void)size;
    return 0;
}
/* GLdc shutdown retains heap containers; reuse one context until process exit. */
void gex_gl_close(void) {}
int gex_gl_submit(void) { return pvr_wait_ready() || pvr_wait_render_done() ? -1 : 0; }
int gex_gl_swap(void) {
    glKosSwapBuffers();
    return gex_gl_submit();
}
int gex_gl_width(void) { return vid_mode->width; }
int gex_gl_height(void) { return vid_mode->height; }
int gex_gl_fbo_top(void) { return 1; }
