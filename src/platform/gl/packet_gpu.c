#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "packet_gpu.h"
#include "platform/gl/context.h"

enum { CACHE_COUNT = 512, CACHE_BYTES = 4 * 1024 * 1024 };
typedef struct target {
    GLuint image[3], framebuffer[3];
    unsigned current;
    int width, height, tw, th, owned;
    const uint8_t *atlas;
    struct target *next;
} target;
typedef struct {
    GLuint image[2];
    unsigned page, clut, alpha, u, v, dimension;
    const uint8_t *atlas;
    int valid;
    size_t bytes;
    uint64_t used;
} texture;
static target *active, *targets, *pictures[2];
static texture cache[CACHE_COUNT];
static unsigned users;
static size_t cache_bytes;
static uint64_t cache_clock;
static const uint8_t *tracked_atlas;
static GLuint white;
static uint16_t *conversion;
static unsigned queued_commands;
#if GEX_PROFILE_RENDERER
static unsigned profile_draws, profile_uploads, profile_resolves;
static size_t profile_bytes;
#define PROFILE(field, value) (profile_##field += (value))
#else
#define PROFILE(field, value) ((void)0)
#endif
static int fail(char *error, size_t size, const char *text) {
    snprintf(error, size, "%s", text);
    return -1;
}
static int check(char *error, size_t size) {
    GLenum code = glGetError();
    if (code != GL_NO_ERROR) {
        snprintf(error, size, "fixed-function GL operation failed: 0x%x", (unsigned)code);
        gex_gl_leave(error, size);
        return -1;
    }
    return gex_gl_leave(error, size);
}
static unsigned word(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned power2(unsigned n) {
    unsigned result = 8;
    while (result < n)
        result *= 2;
    return result;
}
static void projection(int width, int height, int backing_height, int screen) {
    int viewport_y = !screen && gex_gl_fbo_top() ? backing_height - height : 0;
    glViewport(0, viewport_y, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_LIGHTING);
#ifndef GEX_GLDC_DREAMCAST
    glDisable(GL_DITHER);
#endif
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}
static void quad(GLuint image, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                 float v1, unsigned rgb, unsigned alpha) {
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, image ? image : white);
    glColor4ub(rgb & 255, rgb >> 8 & 255, rgb >> 16 & 255, alpha);
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(u0, v0);
    glVertex2f(x0, y0);
    glTexCoord2f(u1, v0);
    glVertex2f(x1, y0);
    glTexCoord2f(u0, v1);
    glVertex2f(x0, y1);
    glTexCoord2f(u1, v1);
    glVertex2f(x1, y1);
    glEnd();
    queued_commands += 5;
}
static float image_top(const target *t) { return gex_gl_fbo_top() ? 1 : (float)t->height / t->th; }
static float image_bottom(const target *t) {
    return gex_gl_fbo_top() ? 1 - (float)t->height / t->th : 0;
}
static int finish(char *error, size_t size) {
    if (!active)
        return 0;
    gex_gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
    if (gex_gl_submit())
        return fail(error, size, "fixed-function GL target wait failed");
    active->current ^= 1;
    active = NULL;
    queued_commands = 0;
    PROFILE(resolves, 1);
    GLenum code = glGetError();
    if (code == GL_NO_ERROR)
        return 0;
    snprintf(error, size, "GLdc target resolve failed: 0x%x", (unsigned)code);
    return -1;
}
static int select_target(target *t, int preserve, char *error, size_t size) {
    if (active == t)
        return 0;
    if (finish(error, size))
        return -1;
    gex_gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, t->framebuffer[t->current ^ 1]);
    projection(t->width, t->height, t->th, 0);
    active = t;
    queued_commands = 0;
    if (preserve) {
        glBlendFunc(GL_ONE, GL_ZERO);
        quad(t->image[t->current], 0, 0, t->width, t->height, 0, image_top(t),
             (float)t->width / t->tw, image_bottom(t), 0xffffff, 255);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    return 0;
}
static void allocate_image(GLuint *image, int width, int height, int rgb, const void *pixels) {
    glGenTextures(1, image);
    glBindTexture(GL_TEXTURE_2D, *image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(GL_TEXTURE_2D, 0,
#ifdef GEX_GLDC_DREAMCAST
                 rgb ? GL_RGB565_KOS : GL_ARGB1555_KOS,
#else
                 rgb ? GL_RGB : GL_RGBA,
#endif
                 width, height, 0, rgb ? GL_RGB : GL_BGRA,
                 rgb ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_SHORT_1_5_5_5_REV, pixels);
    PROFILE(uploads, 1);
    PROFILE(bytes, (size_t)width * height * 2);
}
static int attachment(target *t, unsigned slot, char *error, size_t size) {
    allocate_image(&t->image[slot], t->tw, t->th, 1, NULL);
    gex_gl_gen_framebuffers(1, &t->framebuffer[slot]);
    gex_gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, t->framebuffer[slot]);
    gex_gl_attach_texture(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D,
                          t->image[slot], 0);
    GLenum status = gex_gl_framebuffer_status(GL_FRAMEBUFFER_EXT);
    if (status == GL_FRAMEBUFFER_COMPLETE_EXT)
        return 0;
    snprintf(error, size, "GLdc requires functional render-to-texture FBOs (0x%x)",
             (unsigned)status);
    return -1;
}
static void delete_images(target *t) {
    glDeleteTextures(3, t->image);
    /* GLdc does not ignore deletion of the reserved default framebuffer. */
    for (unsigned i = 0; i < 3; ++i)
        if (t->framebuffer[i])
            gex_gl_delete_framebuffers(1, &t->framebuffer[i]);
}
static int snapshot(target *t, char *error, size_t size) {
    if (finish(error, size))
        return -1;
    if (!t->image[2] && attachment(t, 2, error, size))
        return -1;
    gex_gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, t->framebuffer[2]);
    projection(t->width, t->height, t->th, 0);
    glBlendFunc(GL_ONE, GL_ZERO);
    quad(t->image[t->current], 0, 0, t->width, t->height, 0, image_top(t), (float)t->width / t->tw,
         image_bottom(t), 0xffffff, 255);
    gex_gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
    if (gex_gl_submit())
        return fail(error, size, "fixed-function GL snapshot wait failed");
    if (glGetError() == GL_NO_ERROR)
        return 0;
    return fail(error, size, "GLdc framebuffer feedback snapshot failed");
}
static void *create(int width, int height, char *error, size_t size) {
#ifdef GEX_GLDC_DREAMCAST
    if (width != 320 || height != 240) {
        fail(error, size, "Dreamcast fixed-function GL requires native 320x240 targets");
        return NULL;
    }
#endif
    if (width < 1 || height < 1 || width > 640 || height > 480) {
        fail(error, size, "fixed-function GL targets support up to 640x480");
        return NULL;
    }
    target *t = calloc(1, sizeof *t);
    if (!t) {
        fail(error, size, "cannot allocate GLdc target");
        return NULL;
    }
    int opened = !users, entered = 1;
    if (opened && gex_gl_open(error, size)) {
        free(t);
        return NULL;
    }
    if (gex_gl_enter(error, size)) {
        if (opened)
            gex_gl_close();
        free(t);
        return NULL;
    }
    if (finish(error, size))
        goto failed;
    if (!users) {
        conversion = malloc(256 * 256 * sizeof *conversion);
        if (!conversion) {
            fail(error, size, "cannot allocate fixed-function GL conversion buffer");
            goto failed;
        }
        fprintf(stderr, "[opengl12] GL=%s renderer=%s\n", glGetString(GL_VERSION),
                glGetString(GL_RENDERER));
        uint16_t pixels[64];
        for (unsigned i = 0; i < 64; ++i)
            pixels[i] = 0xffff;
        allocate_image(&white, 8, 8, 0, pixels);
    }
    t->width = width;
    t->height = height;
    t->tw = power2(width);
    t->th = power2(height);
    for (unsigned i = 0; i < 2; ++i) {
        if (attachment(t, i, error, size))
            goto failed;
        projection(width, height, t->th, 0);
        quad(0, 0, 0, width, height, 0, 0, 0, 0, 0, 255);
        gex_gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
        if (gex_gl_submit()) {
            fail(error, size, "fixed-function GL target creation wait failed");
            goto failed;
        }
    }
    if (check(error, size)) {
        entered = 0;
        goto failed;
    }
    t->next = targets;
    targets = t;
    ++users;
    return t;
failed:
    {
        char cleanup[256];
        if (entered || !gex_gl_enter(cleanup, sizeof cleanup)) {
            gex_gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
            if (gex_gl_submit())
                fprintf(stderr, "[opengl12] target cleanup wait failed\n");
            delete_images(t);
            if (opened) {
                glDeleteTextures(1, &white);
                white = 0;
            }
            if (check(cleanup, sizeof cleanup))
                fprintf(stderr, "[opengl12] target cleanup failed: %s\n", cleanup);
        } else
            fprintf(stderr, "[opengl12] target cleanup failed: %s\n", cleanup);
    }
    free(t);
    if (opened) {
        free(conversion); conversion = NULL;
        gex_gl_close();
    }
    return NULL;
}
static void destroy(void *data) {
    target *t = data;
    char error[256];
    if (gex_gl_enter(error, sizeof error) || finish(error, sizeof error))
        goto failed;
    delete_images(t);
    for (unsigned i = 0; i < 2; ++i)
        if (pictures[i] == t)
            pictures[i] = NULL;
    target **link = &targets;
    while (*link != t)
        link = &(*link)->next;
    *link = t->next;
    free(t);
    if (!--users) {
        free(conversion); conversion = NULL;
        glDeleteTextures(1, &white);
        white = 0;
        for (unsigned i = 0; i < CACHE_COUNT; ++i)
            glDeleteTextures(2, cache[i].image);
        memset(cache, 0, sizeof cache);
        cache_bytes = 0;
        cache_clock = 0;
        tracked_atlas = NULL;
    }
    if (check(error, sizeof error))
        goto failed;
    if (!users)
        gex_gl_close();
    return;
failed:
    fprintf(stderr, "[gldc] %s\n", error);
    abort();
}
static int clear(void *data, char *error, size_t size) {
    if (gex_gl_enter(error, size))
        return -1;
    target *t = data;
    if (finish(error, size)) {
        gex_gl_leave(error, size);
        return -1;
    }
    for (unsigned i = 0; i < CACHE_COUNT; ++i) {
        texture *tx = &cache[i];
        if (!tx->valid && tx->bytes) {
            glDeleteTextures(2, tx->image);
            cache_bytes -= tx->bytes;
            memset(tx, 0, sizeof *tx);
        }
    }
    if (select_target(t, 0, error, size)) {
        gex_gl_leave(error, size);
        return -1;
    }
    glBlendFunc(GL_ONE, GL_ZERO);
    quad(0, 0, 0, t->width, t->height, 0, 0, 0, 0, 0, 255);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return check(error, size);
}
static int copy(void *destination, void *source, unsigned shade, char *error, size_t size) {
    if (shade > 255)
        return fail(error, size, "invalid GLdc copy shade");
    if (gex_gl_enter(error, size))
        return -1;
    target *dst = destination, *src = source;
    if (finish(error, size) || select_target(dst, 0, error, size)) {
        gex_gl_leave(error, size);
        return -1;
    }
    glBlendFunc(GL_ONE, GL_ZERO);
    quad(src->image[src->current], 0, 0, dst->width, dst->height, 0, image_top(src),
         (float)src->width / src->tw, image_bottom(src), shade | shade << 8 | shade << 16, 255);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return check(error, size);
}
static int present(void *data, char *error, size_t size) {
    if (gex_gl_enter(error, size))
        return -1;
    target *t = data;
    if (finish(error, size)) {
        gex_gl_leave(error, size);
        return -1;
    }
    int width = gex_gl_width(), height = gex_gl_height();
    projection(width, height, height, 1);
    glBlendFunc(GL_ONE, GL_ZERO);
    quad(0, 0, 0, width, height, 0, 0, 0, 0, 0, 255);
    float w = width, h = (float)t->height * width / t->width;
    if (h > height) {
        h = height;
        w = (float)t->width * height / t->height;
    }
    quad(t->image[t->current], (width - w) / 2, (height - h) / 2, (width + w) / 2, (height + h) / 2,
         0, image_top(t), (float)t->width / t->tw, image_bottom(t), 0xffffff, 255);
    if (gex_gl_swap()) {
        gex_gl_leave(error, size);
        return fail(error, size, "fixed-function GL presentation wait failed");
    }
    queued_commands = 0;
#if GEX_PROFILE_RENDERER
    gex_packet_gpu_profile("opengl12", profile_draws, profile_uploads, profile_resolves,
                           profile_bytes);
    profile_draws = profile_uploads = profile_resolves = 0;
    profile_bytes = 0;
#endif
    static int shown;
    if (!shown) {
        fprintf(stderr, "[gldc] presented first GPU framebuffer; readbacks forbidden\n");
        shown = 1;
    }
    return check(error, size);
}
static void bind(void *data, const uint8_t *atlas, unsigned plane) {
    target *t = data;
    if (plane > 1) {
        fprintf(stderr, "[gldc] invalid picture plane\n");
        abort();
    }
    t->atlas = atlas;
    t->owned = 1;
    pictures[plane] = t;
}
static int overlap(unsigned begin, unsigned bytes, unsigned offset, size_t pitch, size_t row_bytes,
                   unsigned rows) {
    if (rows == 1)
        return row_bytes >= 0x100000 || ((begin - offset) & 0xfffff) < row_bytes ||
               ((offset - begin) & 0xfffff) < bytes;
    if (pitch == 2048 && row_bytes <= 2048 && rows <= 512) {
        unsigned delta = (begin - offset) & 0xfffff, row = delta >> 11, column = delta & 2047;
        return (row < rows && column < row_bytes) ||
               (row + 1 < rows && bytes > 2048 - column) || bytes > 0x100000 - delta;
    }
    for (unsigned row = 0; row < rows; ++row) {
        unsigned at = (offset + (uint64_t)row * pitch) & 0xfffff;
        for (unsigned wrap = 0; wrap < 2; ++wrap) {
            uint64_t a = (uint64_t)at + wrap * 0x100000;
            if ((uint64_t)begin < a + row_bytes && (uint64_t)begin + bytes > a)
                return 1;
            if ((uint64_t)begin + 0x100000 < a + row_bytes &&
                (uint64_t)begin + 0x100000 + bytes > a)
                return 1;
        }
    }
    return 0;
}
static void atlas_write_rect(const uint8_t *atlas, unsigned offset, size_t pitch, size_t row_bytes,
                             unsigned rows) {
    if (tracked_atlas != atlas) {
        tracked_atlas = atlas;
        for (unsigned i = 0; i < CACHE_COUNT; ++i)
            cache[i].valid = 0;
    }
    if (!row_bytes || !rows)
        return;
    for (unsigned i = 0; i < CACHE_COUNT; ++i) {
        texture *t = &cache[i];
        if (!t->valid || t->atlas != atlas)
            continue;
        unsigned format = t->page >> 7 & 3;
        if (format < 2 &&
            overlap(t->clut * 32 & 0xfffff, format ? 512 : 32, offset, pitch, row_bytes, rows))
            t->valid = 0;
        unsigned base = (t->page & 15) * 128 + (t->page & 16 ? 0x80000 : 0);
        unsigned start = (base + t->v * 2048 +
                          (format == 0 ? t->u / 2 : format == 1 ? t->u : t->u * 2)) & 0xfffff;
        unsigned bytes = format == 0 ? t->dimension / 2 :
                         format == 1 ? t->dimension : t->dimension * 2;
        if (!overlap(start, (t->dimension - 1) * 2048 + bytes, offset, pitch, row_bytes, rows))
            continue;
        for (unsigned y = 0; t->valid && y < t->dimension; ++y) {
            if (overlap((start + y * 2048) & 0xfffff, bytes, offset, pitch, row_bytes, rows))
                t->valid = 0;
        }
    }
}
static void atlas_write(const uint8_t *atlas, unsigned offset, size_t bytes) {
    atlas_write_rect(atlas, offset, 0, bytes, 1);
}
static texture *region(const gex_gpu_material *m, unsigned u0, unsigned u1, unsigned v0,
                       unsigned v1, char *error, size_t size) {
    unsigned dimension = power2(u1 - u0 > v1 - v0 ? u1 - u0 : v1 - v0);
    if (u0 + dimension > 256)
        u0 = 256 - dimension;
    if (v0 + dimension > 256)
        v0 = 256 - dimension;
    unsigned format = m->page >> 7 & 3, alpha = m->opcode & 2;
    texture *victim = NULL;
    uint64_t used = ++cache_clock;
    for (unsigned i = 0; i < CACHE_COUNT; ++i) {
        texture *t = &cache[i];
        if (m->atlas == tracked_atlas && t->valid && t->atlas == m->atlas && t->page == m->page &&
            (format == 2 || t->clut == m->clut) && t->alpha == alpha && t->u <= u0 && t->v <= v0 &&
            t->u + t->dimension >= u1 && t->v + t->dimension >= v1) {
            t->used = used;
            return t;
        }
        if (!victim || t->used < victim->used)
            victim = t;
    }
    size_t bytes = dimension * dimension * 2 * (alpha ? 2 : 1);
    if (victim->image[0] || cache_bytes + bytes > CACHE_BYTES) {
        if (finish(error, size))
            return NULL;
        glDeleteTextures(2, victim->image);
        cache_bytes -= victim->bytes;
        memset(victim, 0, sizeof *victim);
        while (cache_bytes + bytes > CACHE_BYTES) {
            texture *oldest = NULL;
            for (unsigned i = 0; i < CACHE_COUNT; ++i)
                if (cache[i].bytes && (!oldest || cache[i].used < oldest->used))
                    oldest = &cache[i];
            if (!oldest) {
                fail(error, size, "GLdc texture exceeds cache budget");
                return NULL;
            }
            glDeleteTextures(2, oldest->image);
            cache_bytes -= oldest->bytes;
            memset(oldest, 0, sizeof *oldest);
        }
    }
    uint16_t *pixels = conversion;
    unsigned base = (m->page & 15) * 128 + (m->page & 16 ? 0x80000 : 0);
    for (unsigned pass = 0; pass < (alpha ? 2u : 1u); ++pass) {
        for (unsigned y = 0; y < dimension; ++y)
            for (unsigned x = 0; x < dimension; ++x) {
                unsigned u = u0 + x, at = (base + (v0 + y) * 2048) & 0xfffff, c;
                if (format < 2) {
                    unsigned index = m->atlas[(at + (format ? u : u / 2)) & 0xfffff];
                    if (!format)
                        index = index >> ((u & 1) * 4) & 15;
                    c = word(m->atlas + ((m->clut * 32 + index * 2) & 0xffffe));
                } else
                    c = word(m->atlas + ((at + u * 2) & 0xffffe));
                int masked = alpha && (c & 0x8000);
                pixels[y * dimension + x] =
                    !c || (alpha && masked != (int)pass)
                        ? 0
                        : 0x8000 | (c & 31) << 10 | (c & 0x3e0) | (c >> 10 & 31);
            }
        allocate_image(&victim->image[pass], dimension, dimension, 0, pixels);
    }
    victim->page = m->page;
    victim->clut = m->clut;
    victim->alpha = alpha;
    victim->atlas = m->atlas;
    victim->u = u0;
    victim->v = v0;
    victim->dimension = dimension;
    victim->used = used;
    victim->bytes = bytes;
    victim->valid = 1;
    cache_bytes += bytes;
    return victim;
}
static unsigned clip_polygon(const gex_gpu_vertex *in, unsigned count, gex_gpu_vertex *out,
                             unsigned axis, float edge, int greater) {
    unsigned used = 0;
    for (unsigned i = 0; i < count; ++i) {
        gex_gpu_vertex a = in[i], b = in[(i + 1) % count];
        float av = axis == 0 ? a.x : axis == 1 ? a.y : axis == 2 ? a.u : a.v;
        float bv = axis == 0 ? b.x : axis == 1 ? b.y : axis == 2 ? b.u : b.v;
        int ai = greater ? av >= edge : av <= edge, bi = greater ? bv >= edge : bv <= edge;
        if (ai)
            out[used++] = a;
        if (ai != bi) {
            float t = (edge - av) / (bv - av);
            out[used++] = (gex_gpu_vertex){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                           a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
        }
    }
    return used;
}
static unsigned tint(unsigned rgb) {
    unsigned result = 0;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned c = (rgb >> (i * 8) & 255) * 2;
        result |= (c > 255 ? 255 : c) << (i * 8);
    }
    return result;
}
static int piece(target *t, gex_gpu_vertex *v, unsigned count, const gex_gpu_material *m,
                 int picture, int source_x, int source_y, char *error, size_t size) {
    unsigned code = m->opcode & ~2u, format = m->page >> 7 & 3;
    int textured = code == 0x2c || code == 0x64;
    texture *tx = NULL;
    if (textured && picture < 0) {
        float u0 = 256, u1 = 0, v0 = 256, v1 = 0;
        for (unsigned i = 0; i < count; ++i) {
            u0 = fminf(u0, v[i].u);
            u1 = fmaxf(u1, v[i].u);
            v0 = fminf(v0, v[i].v);
            v1 = fmaxf(v1, v[i].v);
        }
        tx = region(m, (unsigned)fmaxf(0, floorf(u0)), (unsigned)fminf(256, floorf(u1) + 1),
                    (unsigned)fmaxf(0, floorf(v0)), (unsigned)fminf(256, floorf(v1) + 1), error,
                    size);
        if (!tx)
            return -1;
    }
    if (select_target(t, 1, error, size))
        return -1;
    unsigned passes = textured && picture < 0 && (m->opcode & 2) ? 2 : 1;
    for (unsigned pass = 0; pass < passes; ++pass) {
        unsigned commands = (count == 4 ? 4 : 3 * (count - 2)) + 1;
        if (queued_commands + commands >= GEX_GL_BATCH_COMMANDS) {
            if (finish(error, size) || select_target(t, 1, error, size))
                return -1;
        }
        unsigned rgb = m->rgb;
        unsigned alpha = pass || ((!textured || picture >= 0) && (m->opcode & 2)) ? 128 : 255;
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, picture >= 0 ? pictures[picture]->image[2]
                                     : tx         ? tx->image[pass]
                                                  : white);
        if (textured) {
            int modulated = picture >= 0 || format < 2 ||
                            (code == 0x64 && !(m->opcode & 2) && (m->rgb & 255) != 128 &&
                             (m->rgb >> 8 & 255) != 128 && (m->rgb >> 16 & 255) != 128);
            rgb = modulated ? tint(m->rgb) : 0xffffff;
        }
        glColor4ub(rgb & 255, rgb >> 8 & 255, rgb >> 16 & 255, alpha);
        glBegin(count == 4 ? GL_TRIANGLE_STRIP : GL_TRIANGLE_FAN);
        for (unsigned i = 0; i < count; ++i) {
            /* Preserve the fan's diagonal when submitting four vertices as a strip. */
            const unsigned strip[] = {1, 2, 0, 3};
            const gex_gpu_vertex *p = &v[count == 4 ? strip[i] : i];
            float u = 0, w = 0;
            if (picture >= 0) {
                target *src = pictures[picture];
                u = (p->u + source_x) / src->tw;
                float row = p->v + source_y - picture * 240;
                w = image_top(src) - row / src->th;
            } else if (tx) {
                u = (p->u - tx->u) / tx->dimension;
                w = (p->v - tx->v) / tx->dimension;
            }
            glTexCoord2f(u, w);
            glVertex2f(p->x, p->y);
        }
        glEnd();
        queued_commands += commands;
    }
    return 0;
}
static int polygon(void *data, const gex_gpu_vertex input[4], int left, int top, int right,
                   int bottom, const gex_gpu_material *m, char *error, size_t size) {
    unsigned code = m->opcode & ~2u, format = m->page >> 7 & 3;
    int textured = code == 0x2c || code == 0x64;
    if (left >= right || top >= bottom)
        return 0;
    PROFILE(draws, 1);
    if (textured && (!m->atlas || format == 3))
        return fail(error, size, "invalid GLdc material");
    if (gex_gl_enter(error, size))
        return -1;
    gex_gpu_vertex q[4];
    memcpy(q, input, sizeof q);
    for (unsigned i = 0; i < 4; ++i)
        if (!isfinite(q[i].x) || !isfinite(q[i].y) || !isfinite(q[i].u) || !isfinite(q[i].v) ||
            fabsf(q[i].u) > 1024 || fabsf(q[i].v) > 1024) {
            gex_gl_leave(error, size);
            return fail(error, size, "invalid GLdc polygon vertex");
        }
    if (q[0].x == q[2].x && q[1].x == q[3].x && q[0].y == q[1].y && q[2].y == q[3].y) {
        float w = fabsf(q[3].x - q[0].x), h = fabsf(q[3].y - q[0].y);
        if (!w || !h)
            return check(error, size);
        float du = (q[3].u - q[0].u) / w, dv = (q[3].v - q[0].v) / h;
        for (unsigned i = 0; i < 4; ++i) {
            /* Keep wrap cuts off pixel centers after fixed-subpixel rounding. */
            q[i].u += (q[0].x > q[3].x ? 1 : 0) - du * 0.5f + 1.0f / 128;
            q[i].v += (q[0].y > q[3].y ? 1 : 0) - dv * 0.5f + 1.0f / 128;
        }
    }
    float min_u = q[0].u, max_u = min_u, min_v = q[0].v, max_v = min_v;
    for (unsigned i = 1; i < 4; ++i) {
        min_u = fminf(min_u, q[i].u);
        max_u = fmaxf(max_u, q[i].u);
        min_v = fminf(min_v, q[i].v);
        max_v = fmaxf(max_v, q[i].v);
    }
    int first_u = textured ? (int)floorf(min_u / 256) : 0;
    int last_u = textured ? (int)floorf(max_u / 256) : 0;
    int first_v = textured ? (int)floorf(min_v / 256) : 0;
    int last_v = textured ? (int)floorf(max_v / 256) : 0;
    target *t = data;
    int sampled[2] = {0, 0};
    int source_x = (m->page & 15) * 64, source_y = m->page & 16 ? 256 : 0;
    if (textured)
        for (unsigned plane = 0; plane < 2; ++plane) {
            if (!pictures[plane] || pictures[plane]->atlas != m->atlas)
                continue;
            for (int py = first_v; py <= last_v; ++py)
                for (int px = first_u; px <= last_u; ++px)
                    for (unsigned carry = 0; carry < 2; ++carry) {
                        unsigned divisor = format == 0 ? 4 : format == 1 ? 2 : 1;
                        float u0 = fmaxf(0, min_u - px * 256), u1 = fminf(256, max_u - px * 256);
                        float v0 = fmaxf(0, min_v - py * 256), v1 = fminf(256, max_v - py * 256);
                        if (source_x + u0 / divisor < 320 + carry * 1024 &&
                            source_x + u1 / divisor > carry * 1024 &&
                            source_y + carry + v0 < 232 + plane * 240 &&
                            source_y + carry + v1 > 8 + plane * 240)
                            sampled[plane] = 1;
                    }
            if (sampled[plane] && format < 2) {
                gex_gl_leave(error, size);
                return fail(error, size,
                            "indexed sampling of a GPU-owned framebuffer is unsupported");
            }
            if (sampled[plane] && snapshot(pictures[plane], error, size)) {
                gex_gl_leave(error, size);
                return -1;
            }
        }
    for (int py = first_v; py <= last_v; ++py)
        for (int px = first_u; px <= last_u; ++px) {
            float ue[5] = {0}, ve[6];
            unsigned nu = 1;
            const int x_edges[] = {320, 1024, 1344};
            if (sampled[0] || sampled[1])
                for (unsigned i = 0; i < 3; ++i)
                    if (x_edges[i] > source_x && x_edges[i] < source_x + 256)
                        ue[nu++] = x_edges[i] - source_x;
            ue[nu++] = 256;
            for (unsigned ui = 0; ui + 1 < nu; ++ui) {
                int carry = source_x + (ue[ui] + ue[ui + 1]) * 0.5f >= 1024;
                int sx = source_x - carry * 1024, sy = source_y + carry;
                unsigned nv = 1;
                ve[0] = 0;
                const int y_edges[] = {8, 232, 248, 472};
                if (sampled[0] || sampled[1])
                    for (unsigned i = 0; i < 4; ++i)
                        if (y_edges[i] > sy && y_edges[i] < sy + 256)
                            ve[nv++] = y_edges[i] - sy;
                ve[nv++] = 256;
                for (unsigned vi = 0; vi + 1 < nv; ++vi) {
                    const unsigned order[] = {0, 1, 3, 2};
                    gex_gpu_vertex a[16], b[16];
                    for (unsigned i = 0; i < 4; ++i)
                        a[i] = q[order[i]];
                    unsigned count = 4;
                    float edges[] = {left,
                                     right,
                                     top,
                                     bottom,
                                     px * 256 + ue[ui],
                                     px * 256 + ue[ui + 1],
                                     py * 256 + ve[vi],
                                     py * 256 + ve[vi + 1]};
                    for (unsigned edge = 0; edge < (textured ? 8u : 4u) && count; ++edge) {
                        count = clip_polygon(a, count, b, edge / 2, edges[edge], !(edge & 1));
                        memcpy(a, b, count * sizeof *a);
                    }
                    if (count < 3)
                        continue;
                    for (unsigned i = 0; i < count; ++i) {
                        a[i].u -= px * 256;
                        a[i].v -= py * 256;
                    }
                    int picture = -1;
                    if (textured && format == 2 && sx + ue[ui] < 320)
                        for (unsigned plane = 0; plane < 2; ++plane)
                            if (sampled[plane] &&
                                sy + (ve[vi] + ve[vi + 1]) * 0.5f >= 8 + plane * 240 &&
                                sy + (ve[vi] + ve[vi + 1]) * 0.5f < 232 + plane * 240)
                                picture = (int)plane;
                    if (piece(t, a, count, m, picture, sx, sy, error, size)) {
                        gex_gl_leave(error, size);
                        return -1;
                    }
                }
            }
        }
    if ((sampled[0] || sampled[1]) && finish(error, size)) {
        gex_gl_leave(error, size);
        return -1;
    }
    return check(error, size);
}
static int blit(void *data, int x, int y, int width, int height, const uint8_t *pixels, int pitch,
                int key, char *error, size_t size) {
    target *t = data;
    if (!pixels || width < 1 || height < 1 || x < 0 || y < 0 || x + width > t->width ||
        y + height > t->height || pitch < width * 2)
        return fail(error, size, "invalid GLdc asset blit");
    if (gex_gl_enter(error, size))
        return -1;
    unsigned tw = power2(width), th = power2(height);
    uint16_t *converted = calloc(tw * th, 2);
    if (!converted) {
        gex_gl_leave(error, size);
        return fail(error, size, "cannot allocate GLdc blit");
    }
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col) {
            unsigned c = word(pixels + row * pitch + col * 2);
            converted[row * tw + col] =
                (int)c == key ? 0 : 0x8000 | (c & 31) << 10 | (c & 0x3e0) | (c >> 10 & 31);
        }
    GLuint image;
    allocate_image(&image, tw, th, 0, converted);
    free(converted);
    if (select_target(t, 1, error, size)) {
        glDeleteTextures(1, &image);
        gex_gl_leave(error, size);
        return -1;
    }
    quad(image, x, y, x + width, y + height, 0, 0, (float)width / tw, (float)height / th, 0xffffff,
         255);
    int result = finish(error, size);
    glDeleteTextures(1, &image);
    if (result) {
        gex_gl_leave(error, size);
        return -1;
    }
    return check(error, size);
}
static int movie(void *data, const uint8_t *pixels, int width, int height, int pitch, char *error,
                 size_t size) {
    if (!pixels || width < 1 || height < 1 || width > 640 || height > 480 || pitch < width * 2)
        return fail(error, size, "invalid GLdc movie frame");
    if (gex_gl_enter(error, size))
        return -1;
    target *t = data;
    unsigned tw = power2(width), th = power2(height);
    uint16_t *packed = calloc(tw * th, 2);
    if (!packed) {
        gex_gl_leave(error, size);
        return fail(error, size, "cannot allocate GLdc movie upload");
    }
    for (int row = 0; row < height; ++row)
        memcpy(packed + row * tw, pixels + row * pitch, width * 2);
    GLuint image;
    allocate_image(&image, tw, th, 1, packed);
    free(packed);
    if (finish(error, size) || select_target(t, 0, error, size)) {
        glDeleteTextures(1, &image);
        gex_gl_leave(error, size);
        return -1;
    }
    glBlendFunc(GL_ONE, GL_ZERO);
    quad(0, 0, 0, t->width, t->height, 0, 0, 0, 0, 0, 255);
    float w = t->width, h = (float)height * w / width;
    if (h > t->height) {
        h = t->height;
        w = (float)width * h / height;
    }
    quad(image, (t->width - w) / 2, (t->height - h) / 2, (t->width + w) / 2, (t->height + h) / 2, 0,
         0, (float)width / tw, (float)height / th, 0xffffff, 255);
    int result = finish(error, size);
    glDeleteTextures(1, &image);
    if (result) {
        gex_gl_leave(error, size);
        return -1;
    }
    if (check(error, size))
        return -1;
    return present(t, error, size);
}
static int upload(void *data, const uint16_t *pixels, char *error, size_t size) {
    target *t = data;
    if (t->owned)
        return fail(error, size, "GLdc live framebuffer CPU replacement is forbidden");
    return blit(t, 0, 0, t->width, t->height, (const uint8_t *)pixels, t->width * 2, -1, error,
                size);
}
static int download(void *data, uint16_t *pixels, char *error, size_t size) {
    target *t = data;
    if (t->owned)
        return fail(error, size, "GLdc live framebuffer readback is forbidden");
#ifdef GEX_GLDC_DREAMCAST
    (void)pixels;
    return fail(error, size, "GLdc has no diagnostic readback; inspect scanout externally");
#else
    if (gex_gl_enter(error, size))
        return -1;
    if (finish(error, size)) {
        gex_gl_leave(error, size);
        return -1;
    }
    gex_gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, t->framebuffer[t->current]);
    uint8_t *rgba = malloc((size_t)t->width * t->height * 4);
    if (!rgba) {
        gex_gl_leave(error, size);
        return fail(error, size, "cannot allocate GLdc diagnostic pixels");
    }
    glReadPixels(0, gex_gl_fbo_top() ? t->th - t->height : 0, t->width, t->height, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    for (int y = 0; y < t->height; ++y)
        for (int x = 0; x < t->width; ++x) {
            uint8_t *p = rgba + ((t->height - 1 - y) * t->width + x) * 4;
            pixels[y * t->width + x] = p[0] >> 3 | (p[1] >> 3) << 5 | (p[2] >> 3) << 10;
        }
    free(rgba);
    gex_gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
    return check(error, size);
#endif
}
static int draw(void *data, const gex_gpu_span *spans, size_t count, const gex_gpu_material *m,
                char *error, size_t size) {
    target *t = data;
    for (size_t i = 0; i < count; ++i) {
        const gex_gpu_span *s = &spans[i];
        for (int x = 0; x < s->count; ++x) {
            int k = (x + s->phase) / s->scale;
            float u = s->mode == 1 ? floor((s->u + k * s->du) / 65536) : s->u + k * s->du;
            float v = s->mode == 1 ? floor((s->v + k * s->dv) / 65536) : s->v + k * s->dv;
            gex_gpu_vertex q[] = {{s->x + x, s->y, u, v},
                                  {s->x + x + 1, s->y, u, v},
                                  {s->x + x, s->y + 1, u, v},
                                  {s->x + x + 1, s->y + 1, u, v}};
            if (polygon(t, q, 0, 0, t->width, t->height, m, error, size))
                return -1;
        }
    }
    return 0;
}
const gex_packet_gpu *gex_packet_gpu_gldc(void) {
    static const gex_gpu_native_ops native = {.clear = clear,
                                              .copy = copy,
                                              .blit = blit,
                                              .present = present,
                                              .bind = bind,
                                              .polygon = polygon,
                                              .movie = movie,
                                              .atlas_write = atlas_write,
                                              .atlas_write_rect = atlas_write_rect};
    static const gex_packet_gpu gpu = {.create = create,
                                       .destroy = destroy,
                                       .upload = upload,
                                       .download = download,
                                       .draw = draw,
                                       .native = &native};
    return &gpu;
}
#ifdef GEX_PACKET_GPU_TEST_API
unsigned gex_opengl12_test_image(void *data) {
    target *t = data;
    return t->image[t->current];
}
#endif
