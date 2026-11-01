#ifndef GEX_GL_CONTEXT_H
#define GEX_GL_CONTEXT_H
#ifdef GEX_GLDC_DREAMCAST
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glkos.h>
#else
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_opengl_glext.h>
#endif
#include <stddef.h>
enum { GEX_GL_BATCH_COMMANDS = 2048 };

typedef void(APIENTRY *gex_gl_gen_fn)(GLsizei, GLuint *);
typedef void(APIENTRY *gex_gl_delete_fn)(GLsizei, const GLuint *);
typedef void(APIENTRY *gex_gl_bind_fn)(GLenum, GLuint);
typedef void(APIENTRY *gex_gl_attach_fn)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY *gex_gl_status_fn)(GLenum);
extern gex_gl_gen_fn gex_gl_gen_framebuffers;
extern gex_gl_delete_fn gex_gl_delete_framebuffers;
extern gex_gl_bind_fn gex_gl_bind_framebuffer;
extern gex_gl_attach_fn gex_gl_attach_texture;
extern gex_gl_status_fn gex_gl_framebuffer_status;
int gex_gl_open(char *error, size_t size);
void gex_gl_set_headless(int headless);
int gex_gl_enter(char *error, size_t size);
int gex_gl_leave(char *error, size_t size);
void gex_gl_close(void);
int gex_gl_submit(void);
int gex_gl_swap(void);
int gex_gl_height(void);
int gex_gl_width(void);
int gex_gl_fbo_top(void);
#endif
