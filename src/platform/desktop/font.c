#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "platform/font.h"
#include "win32hle.h"
struct gex_font { TTF_Font *native; };
int gex_font_initialize(void) { return TTF_WasInit() || !TTF_Init(); }
const char *gex_font_error(void) { return TTF_GetError(); }
gex_font *gex_font_open(const char *path, const void *data, size_t bytes, int size) {
    if (bytes > INT32_MAX) { fprintf(stderr, "[font] font data exceeds supported size\n"); return NULL; }
    TTF_Font *native = data ? TTF_OpenFontRW(SDL_RWFromConstMem(data, (int)bytes), 1, size) : TTF_OpenFont(path, size);
    if (!native) { fprintf(stderr, "[font] cannot open %s: %s\n", path, TTF_GetError()); return NULL; }
    gex_font *font = malloc(sizeof *font);
    if (!font) hle_fatal("cannot allocate font handle");
    font->native = native;
    return font;
}
void gex_font_close(gex_font *font) { TTF_CloseFont(font->native); free(font); }
const char *gex_font_family(gex_font *font) { return TTF_FontFaceFamilyName(font->native); }
void gex_font_configure(gex_font *font, int crisp, int italic, int underline) {
    TTF_SetFontHinting(font->native, crisp ? TTF_HINTING_MONO : TTF_HINTING_NORMAL);
    TTF_SetFontKerning(font->native, 0);
    TTF_SetFontStyle(font->native, (italic ? TTF_STYLE_ITALIC : 0) | (underline ? TTF_STYLE_UNDERLINE : 0));
}
int gex_font_measure(gex_font *font, const char *text, int *width, int *height) {
    return TTF_SizeText(font->native, text, width, height);
}
int gex_font_height(gex_font *font) { return TTF_FontHeight(font->native); }
int gex_font_ascent(gex_font *font) { return TTF_FontAscent(font->native); }
int gex_font_descent(gex_font *font) { return TTF_FontDescent(font->native); }
static void ink_from(uint8_t *ink, int iw, int ih, int x0, SDL_Surface *s) {
    SDL_LockSurface(s);
    for (int j = 0; j < s->h && j < ih; j++)
        for (int i = 0; i < s->w; i++) {
            int x = x0 + i, a;
            if (x < 0 || x >= iw) continue;
            if (s->format->BytesPerPixel == 1) a = ((const uint8_t *)s->pixels)[j * s->pitch + i] ? 255 : 0;
            else a = (int)(((const uint32_t *)((const uint8_t *)s->pixels + j * s->pitch))[i] >> 24);
            if (a > ink[j * iw + x]) ink[j * iw + x] = (uint8_t)a;
        }
    SDL_UnlockSurface(s);
}
static SDL_Surface *render(int crisp, TTF_Font *t, const char *s) {
    SDL_Color white = { 255, 255, 255, 255 };
    return crisp ? TTF_RenderText_Solid(t, s, white) : TTF_RenderText_Blended(t, s, white);
}
uint8_t *gex_font_coverage(gex_font *font, const char *buf, int n, int crisp, int synth_bold, int *iw, int *ih) {
    TTF_Font *t = font->native;
    int w, h;
    if (TTF_SizeText(t, buf, &w, &h) != 0) {
        fprintf(stderr, "[font] cannot measure text: %s\n", TTF_GetError());
        return NULL;
    }
    if (w < 0 || h < 0 || n < 0 || (synth_bold && n >= INT_MAX - w))
        hle_fatal("invalid font coverage size");
    *iw = w + (synth_bold ? n + 1 : 0), *ih = h;
    if ((uint64_t)*iw * (unsigned)*ih >= SIZE_MAX)
        hle_fatal("font coverage exceeds address space");
    uint8_t *ink = (uint8_t *)calloc((size_t)*iw * *ih + 1, 1);
    if (!ink) hle_fatal("cannot allocate font coverage");
    if (!synth_bold) {
        SDL_Surface *s = render(crisp, t, buf);
        if (!s) {
            fprintf(stderr, "[font] cannot render text: %s\n", TTF_GetError());
            free(ink);
            return NULL;
        }
        ink_from(ink, *iw, *ih, 0, s);
        SDL_FreeSurface(s);
        return ink;
    }
    for (int k = 0, x = 0; k < n; k++) {
        char one[2] = { buf[k], 0 };
        int adv = 0;
        if (TTF_GlyphMetrics(t, (uint8_t)buf[k], NULL, NULL, NULL, NULL, &adv) != 0 &&
            TTF_SizeText(t, one, &adv, NULL) != 0) {
            fprintf(stderr, "[font] cannot measure glyph: %s\n", TTF_GetError());
            free(ink);
            return NULL;
        }
        SDL_Surface *s = render(crisp, t, one);
        if (!s) {
            fprintf(stderr, "[font] cannot render glyph: %s\n", TTF_GetError());
            free(ink);
            return NULL;
        }
        ink_from(ink, *iw, *ih, x, s);
        ink_from(ink, *iw, *ih, x + 1, s);
        SDL_FreeSurface(s);
        x += adv + 1;
    }
    return ink;
}
