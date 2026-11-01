#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_opengl_glext.h>
#include "packet_gpu.h"

#define GL_FUNCTIONS(X) \
    X(PFNGLGENBUFFERSPROC, GenBuffers) X(PFNGLDELETEBUFFERSPROC, DeleteBuffers) \
    X(PFNGLBINDBUFFERPROC, BindBuffer) X(PFNGLBUFFERDATAPROC, BufferData) \
    X(PFNGLBUFFERSUBDATAPROC, BufferSubData) X(PFNGLGETBUFFERSUBDATAPROC, GetBufferSubData) \
    X(PFNGLBINDBUFFERBASEPROC, BindBufferBase) X(PFNGLCREATESHADERPROC, CreateShader) \
    X(PFNGLSHADERSOURCEPROC, ShaderSource) X(PFNGLCOMPILESHADERPROC, CompileShader) \
    X(PFNGLGETSHADERIVPROC, GetShaderiv) X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC, DeleteShader) X(PFNGLCREATEPROGRAMPROC, CreateProgram) \
    X(PFNGLATTACHSHADERPROC, AttachShader) X(PFNGLLINKPROGRAMPROC, LinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog) \
    X(PFNGLDELETEPROGRAMPROC, DeleteProgram) X(PFNGLUSEPROGRAMPROC, UseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation) X(PFNGLUNIFORM1UIPROC, Uniform1ui) \
    X(PFNGLUNIFORM1IPROC, Uniform1i) X(PFNGLDISPATCHCOMPUTEPROC, DispatchCompute) \
    X(PFNGLMEMORYBARRIERPROC, MemoryBarrier)
#define DECLARE(type, name) static type gpu_##name;
GL_FUNCTIONS(DECLARE)
static GLenum (APIENTRY *gpu_GetError)(void);
static const GLubyte *(APIENTRY *gpu_GetString)(GLenum);

static SDL_Window *window;
static SDL_GLContext context;
static SDL_Window *caller_window;
static SDL_GLContext caller_context;
static unsigned users;
static GLuint program, atlas_buffer, table_buffer, coverage_buffer;
static GLint width_uniform, page_uniform, clut_uniform, opcode_uniform, rgb_uniform;
static uint8_t atlas_cache[0x100000], table_cache[1024];
static int atlas_valid, table_valid;
static int video_owned;

typedef struct {
    int width, height;
    GLuint buffer;
    uint32_t *transfer;
} target;

static const char shader[] =
    "#version 430\n"
    "layout(local_size_x=64) in;\n"
    "struct Span { int x; int y; int count; int mode;"
    "double u; double v; double du; double dv;"
    "int scale; int phase; int reserved0; int reserved1; };\n"
    "layout(std430,binding=0) buffer Picture { uint pixels[]; };\n"
    "layout(std430,binding=1) readonly buffer Atlas { uint atlas[]; };\n"
    "layout(std430,binding=2) readonly buffer Table { uint table[]; };\n"
    "layout(std430,binding=3) readonly buffer Coverage { Span spans[]; };\n"
    "uniform int width; uniform uint page,clut,opcode,rgb;\n"
    "uint byteAt(uint a) { a&=1048575u; return (atlas[a>>2]>>((a&3u)*8u))&255u; }\n"
    "uint wordAt(uint a) { a&=1048574u; return byteAt(a)|(byteAt(a+1u)<<8); }\n"
    "uint modulate(uint c,bool mask) { uint r=mask?(c&32768u):0u;"
    "for(uint i=0u;i<3u;i++) { uint n=(((rgb>>(i*8u))&255u)>>3u)*32u+((c>>(i*5u))&31u);"
    "r|=((table[n>>2u]>>((n&3u)*8u))&255u)<<(i*5u); } return r; }\n"
    "void main() { Span s=spans[gl_WorkGroupID.y]; uint j=gl_GlobalInvocationID.x;"
    "if(j>=uint(s.count)) return; int k=(int(j)+s.phase)/s.scale;"
    "precise double u=s.u+double(k)*s.du,v=s.v+double(k)*s.dv;"
    "int iu=s.mode==1?int(floor(u/65536.0)):int(u);"
    "int iv=s.mode==1?int(floor(v/65536.0)):int(v);"
    "uint code=opcode&253u,format=(page>>7u)&3u;"
    "bool textured=code==44u||code==100u; uint c;"
    "if(textured) { uint a=(page&15u)*128u+((page&16u)!=0u?524288u:0u)+uint(iv&255)*2048u;"
    "uint x=uint(iu&255);"
    "if(format==0u) c=wordAt(clut*32u+((byteAt(a+x/2u)>>((x&1u)*4u))&15u)*2u);"
    "else if(format==1u) c=wordAt(clut*32u+byteAt(a+x)*2u);"
    "else c=wordAt(a+x*2u);"
    "if(format<2u) c=modulate(c,true);"
    "if(c==0u) return;"
    "if(code==100u&&(opcode&2u)==0u&&format==2u&&"
    "(rgb&255u)!=128u&&((rgb>>8u)&255u)!=128u&&((rgb>>16u)&255u)!=128u)"
    "c=modulate(c,false);"
    "} else c=((rgb&255u)>>3u)|((((rgb>>8u)&255u)>>3u)<<5u)|((((rgb>>16u)&255u)>>3u)<<10u);"
    "uint dst=uint(s.y*width+s.x)+j;"
    "if((opcode&2u)!=0u&&(!textured||(c&32768u)!=0u))"
    "c=((c>>1u)&15855u)+((pixels[dst]>>1u)&15855u);"
    "pixels[dst]=c; }\n";

static int current(char *error, size_t size) {
    caller_window = SDL_GL_GetCurrentWindow();
    caller_context = SDL_GL_GetCurrentContext();
    if (SDL_GL_MakeCurrent(window, context)) {
        snprintf(error, size, "cannot acquire packet GPU context: %s", SDL_GetError());
        return -1;
    }
    return 0;
}

static int check(char *error, size_t size) {
    GLenum code = gpu_GetError();
    int failed = code != GL_NO_ERROR;
    if (failed) snprintf(error, size, "OpenGL packet operation failed (0x%X)", code);
    /* Guest workers and the launcher share a serialized GPU, not an SDL
     * context permanently bound to the thread that allocated it. */
    if (SDL_GL_MakeCurrent(caller_context ? caller_window : window, caller_context)) {
        snprintf(error, size, "cannot release packet GPU context: %s", SDL_GetError());
        return -1;
    }
    return failed ? -1 : 0;
}

static void close_device(void) {
    if (context) {
        SDL_GL_MakeCurrent(window, context);
        if (program) gpu_DeleteProgram(program);
        if (atlas_buffer) gpu_DeleteBuffers(1, &atlas_buffer);
        if (table_buffer) gpu_DeleteBuffers(1, &table_buffer);
        if (coverage_buffer) gpu_DeleteBuffers(1, &coverage_buffer);
        SDL_GL_DeleteContext(context);
        if (caller_context && SDL_GL_MakeCurrent(caller_window, caller_context)) {
            fprintf(stderr, "[packet-gpu] cannot restore caller context: %s\n", SDL_GetError());
            abort();
        }
    }
    if (window) SDL_DestroyWindow(window);
    window = NULL; context = NULL;
    program = atlas_buffer = table_buffer = coverage_buffer = 0;
    atlas_valid = table_valid = 0;
    if (video_owned) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    video_owned = 0;
}

static int open_device(char *error, size_t size) {
    caller_window = SDL_GL_GetCurrentWindow();
    caller_context = SDL_GL_GetCurrentContext();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        snprintf(error, size, "packet GPU video initialization failed: %s", SDL_GetError());
        return -1;
    }
    video_owned = 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    window = SDL_CreateWindow("Gex packet GPU", 0, 0, 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (window) context = SDL_GL_CreateContext(window);
    if (!context) {
        snprintf(error, size, "OpenGL 4.3 packet GPU unavailable: %s", SDL_GetError());
        close_device(); return -1;
    }
#define LOAD(type, name) \
    do { void *address = SDL_GL_GetProcAddress("gl" #name); \
        memcpy(&gpu_##name, &address, sizeof address); \
        if (!gpu_##name) { snprintf(error, size, "missing gl" #name); close_device(); return -1; } \
    } while (0);
    GL_FUNCTIONS(LOAD)
    void *address = SDL_GL_GetProcAddress("glGetError");
    memcpy(&gpu_GetError, &address, sizeof address);
    address = SDL_GL_GetProcAddress("glGetString");
    memcpy(&gpu_GetString, &address, sizeof address);
    if (!gpu_GetError || !gpu_GetString) {
        snprintf(error, size, "missing core OpenGL functions"); close_device(); return -1;
    }
    GLuint compiled = gpu_CreateShader(GL_COMPUTE_SHADER);
    const char *source = shader;
    gpu_ShaderSource(compiled, 1, &source, NULL);
    gpu_CompileShader(compiled);
    GLint ok;
    gpu_GetShaderiv(compiled, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        gpu_GetShaderInfoLog(compiled, (GLsizei)size, NULL, error);
        gpu_DeleteShader(compiled); close_device(); return -1;
    }
    program = gpu_CreateProgram();
    gpu_AttachShader(program, compiled);
    gpu_LinkProgram(program);
    gpu_DeleteShader(compiled);
    gpu_GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        gpu_GetProgramInfoLog(program, (GLsizei)size, NULL, error);
        close_device(); return -1;
    }
    width_uniform = gpu_GetUniformLocation(program, "width");
    page_uniform = gpu_GetUniformLocation(program, "page");
    clut_uniform = gpu_GetUniformLocation(program, "clut");
    opcode_uniform = gpu_GetUniformLocation(program, "opcode");
    rgb_uniform = gpu_GetUniformLocation(program, "rgb");
    gpu_GenBuffers(1, &atlas_buffer);
    gpu_BindBuffer(GL_SHADER_STORAGE_BUFFER, atlas_buffer);
    gpu_BufferData(GL_SHADER_STORAGE_BUFFER, sizeof atlas_cache, NULL, GL_DYNAMIC_DRAW);
    gpu_GenBuffers(1, &table_buffer);
    gpu_BindBuffer(GL_SHADER_STORAGE_BUFFER, table_buffer);
    gpu_BufferData(GL_SHADER_STORAGE_BUFFER, sizeof table_cache, NULL, GL_DYNAMIC_DRAW);
    gpu_GenBuffers(1, &coverage_buffer);
    fprintf(stderr, "[packet-gpu] OpenGL %s; device %s\n",
            gpu_GetString(GL_VERSION), gpu_GetString(GL_RENDERER));
    if (check(error, size)) { close_device(); return -1; }
    return 0;
}

static void *create(int width, int height, char *error, size_t size) {
    _Static_assert(sizeof(gex_gpu_span) == 64, "GPU span ABI");
    if (!context && open_device(error, size)) return NULL;
    if (current(error, size)) return NULL;
    target *t = calloc(1, sizeof *t);
    if (!t) {
        int status = check(error, size);
        if (!status) snprintf(error, size, "cannot allocate GPU target");
        if (!users) close_device();
        return NULL;
    }
    t->width = width; t->height = height;
    t->transfer = calloc((size_t)width * height, sizeof *t->transfer);
    if (t->transfer) {
        gpu_GenBuffers(1, &t->buffer);
        gpu_BindBuffer(GL_SHADER_STORAGE_BUFFER, t->buffer);
        gpu_BufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)((size_t)width * height * 4),
                       t->transfer, GL_DYNAMIC_DRAW);
    }
    int failed = check(error, size);
    if (!t->transfer || failed) {
        if (!t->transfer) snprintf(error, size, "cannot allocate GPU transfer storage");
        if (t->buffer) {
            char cleanup_error[512];
            if (current(cleanup_error, sizeof cleanup_error)) {
                fprintf(stderr, "[packet-gpu] %s\n", cleanup_error); abort();
            }
            gpu_DeleteBuffers(1, &t->buffer);
            if (check(cleanup_error, sizeof cleanup_error)) {
                fprintf(stderr, "[packet-gpu] %s\n", cleanup_error); abort();
            }
        }
        free(t->transfer); free(t); if (!users) close_device(); return NULL;
    }
    ++users;
    return t;
}

static void destroy(void *device) {
    target *t = device;
    char error[512];
    if (current(error, sizeof error)) { fprintf(stderr, "[packet-gpu] %s\n", error); abort(); }
    gpu_DeleteBuffers(1, &t->buffer);
    free(t->transfer); free(t);
    if (!--users) close_device();
    else if (check(error, sizeof error)) { fprintf(stderr, "[packet-gpu] %s\n", error); abort(); }
}

static int upload(void *device, const uint16_t *pixels, char *error, size_t size) {
    target *t = device;
    if (current(error, size)) return -1;
    for (size_t i = 0; i < (size_t)t->width * t->height; ++i) t->transfer[i] = pixels[i];
    gpu_BindBuffer(GL_SHADER_STORAGE_BUFFER, t->buffer);
    gpu_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)((size_t)t->width * t->height * 4), t->transfer);
    return check(error, size);
}

static int download(void *device, uint16_t *pixels, char *error, size_t size) {
    target *t = device;
    if (current(error, size)) return -1;
    gpu_MemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    gpu_BindBuffer(GL_SHADER_STORAGE_BUFFER, t->buffer);
    gpu_GetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                        (GLsizeiptr)((size_t)t->width * t->height * 4), t->transfer);
    if (check(error, size)) return -1;
    for (size_t i = 0; i < (size_t)t->width * t->height; ++i) pixels[i] = (uint16_t)t->transfer[i];
    return 0;
}

static void region(const uint8_t *atlas, unsigned offset, unsigned bytes) {
    offset &= 0xffffc;
    bytes = (bytes + 3) & ~3u;
    if (!atlas_valid || memcmp(atlas_cache + offset, atlas + offset, bytes)) {
        memcpy(atlas_cache + offset, atlas + offset, bytes);
        gpu_BufferSubData(GL_SHADER_STORAGE_BUFFER, offset, bytes, atlas + offset);
    }
}

static int draw(void *device, const gex_gpu_span *spans, size_t count,
                const gex_gpu_material *m, char *error, size_t size) {
    target *t = device;
    if (current(error, size)) return -1;
    unsigned code = m->opcode & ~2u, format = (m->page >> 7) & 3;
    if (code == 0x2c || code == 0x64) {
        gpu_BindBuffer(GL_SHADER_STORAGE_BUFFER, atlas_buffer);
        unsigned base = (m->page & 15) * 128 + ((m->page & 16) ? 0x80000 : 0);
        unsigned bytes = format == 0 ? 128 : format == 1 ? 256 : 512;
        unsigned char rows[256] = {0};
        for (size_t i = 0; i < count; ++i) {
            double v = spans[i].mode ? spans[i].v / 65536.0 : spans[i].v;
            if (spans[i].dv != 0) memset(rows, 1, sizeof rows);
            else rows[(spans[i].mode ? (int)floor(v) : (int)v) & 255] = 1;
        }
        for (unsigned row = 0; row < 256; ++row)
            if (rows[row]) {
                unsigned offset = (base + row * 2048) & 0xfffff;
                unsigned first = bytes < 0x100000 - offset ? bytes : 0x100000 - offset;
                region(m->atlas, offset, first);
                if (first < bytes) region(m->atlas, 0, bytes - first);
            }
        if (format < 2) {
            unsigned offset = (m->clut * 32) & 0xfffff;
            unsigned bytes = format == 0 ? 32 : 512;
            unsigned first = bytes < 0x100000 - offset ? bytes : 0x100000 - offset;
            region(m->atlas, offset, first);
            if (first < bytes) region(m->atlas, 0, bytes - first);
        }
        /* Each cached region is initialized independently; a zero-filled cache
         * alone cannot establish that GPU allocation bytes were initialized. */
        if (!atlas_valid) {
            gpu_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof atlas_cache, m->atlas);
            memcpy(atlas_cache, m->atlas, sizeof atlas_cache);
            atlas_valid = 1;
        }
        if (!table_valid || memcmp(table_cache, m->modulation, sizeof table_cache)) {
            memcpy(table_cache, m->modulation, sizeof table_cache);
            gpu_BindBuffer(GL_SHADER_STORAGE_BUFFER, table_buffer);
            gpu_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof table_cache, table_cache);
            table_valid = 1;
        }
    }
    gpu_UseProgram(program);
    gpu_Uniform1i(width_uniform, t->width);
    gpu_Uniform1ui(page_uniform, m->page);
    gpu_Uniform1ui(clut_uniform, m->clut);
    gpu_Uniform1ui(opcode_uniform, m->opcode);
    gpu_Uniform1ui(rgb_uniform, m->rgb);
    gpu_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, t->buffer);
    gpu_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, atlas_buffer);
    gpu_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, table_buffer);
    gpu_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, coverage_buffer);
    /* Tiled backdrop pieces can overlap; one barrier per contiguous row batch
     * preserves producer order even when a span revisits the same pixels. */
    for (size_t begin = 0; begin < count;) {
        size_t end = begin + 1;
        int longest = spans[begin].count;
        while (end < count && end - begin < 65535 &&
               (spans[end].y > spans[end - 1].y ||
                (spans[end].y == spans[end - 1].y &&
                 spans[end].x >= spans[end - 1].x + spans[end - 1].count))) {
            if (spans[end].count > longest) longest = spans[end].count;
            ++end;
        }
        gpu_BindBuffer(GL_SHADER_STORAGE_BUFFER, coverage_buffer);
        gpu_BufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)((end - begin) * sizeof *spans),
                       spans + begin, GL_STREAM_DRAW);
        gpu_DispatchCompute((GLuint)((longest + 63) / 64), (GLuint)(end - begin), 1);
        gpu_MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        begin = end;
    }
    return check(error, size);
}

const gex_packet_gpu *gex_packet_gpu_get(void) {
    static const gex_packet_gpu api = {create, destroy, upload, download, draw, NULL};
    return &api;
}
