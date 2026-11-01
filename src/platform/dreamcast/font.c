#include <kos.h>
#include <stdlib.h>
#include <string.h>
#include "platform/font.h"

struct gex_font { int height, width, underline; };

const char *gex_font_locate(const char *family, int bold, int italic) {
    (void)family; (void)bold; (void)italic;
    return "dreamcast-bios";
}
int gex_font_initialize(void) { return 0; }
const char *gex_font_error(void) { return "Dreamcast BIOS font allocation or input failure"; }
gex_font *gex_font_open(const char *path, const void *data, size_t bytes, int size) {
    (void)path; (void)bytes;
    if (data || size < 1 || size > 256) {
        fprintf(stderr, "[dreamcast] unsupported font data or size\n");
        return NULL;
    }
    gex_font *font = malloc(sizeof *font);
    if (font) *font = (gex_font){.height = size, .width = (size + 1) / 2};
    return font;
}
void gex_font_close(gex_font *font) { free(font); }
const char *gex_font_family(gex_font *font) { (void)font; return "Dreamcast BIOS"; }
void gex_font_configure(gex_font *font, int crisp, int italic, int underline) {
    (void)crisp; (void)italic;
    font->underline = underline;
}
int gex_font_measure(gex_font *font, const char *text, int *width, int *height) {
    size_t count = strlen(text);
    if (count > INT32_MAX / font->width) return -1;
    *width = (int)count * font->width;
    *height = font->height;
    return 0;
}
int gex_font_height(gex_font *font) { return font->height; }
int gex_font_ascent(gex_font *font) { return font->height * 3 / 4; }
int gex_font_descent(gex_font *font) { return font->height - gex_font_ascent(font); }
uint8_t *gex_font_coverage(gex_font *font, const char *text, int count,
                          int crisp, int bold, int *width, int *height) {
    (void)crisp;
    if (count < 0 || count > 4096) return NULL;
    *width = count * font->width;
    *height = font->height;
    uint8_t *pixels = calloc((size_t)*width * *height + 1, 1);
    if (!pixels) return NULL;
    for (int i = 0; i < count; ++i) {
        uint8_t glyph[12 * 24] = {0};
        bfont_draw_ex(glyph, 12, 255, 0, 8, true, (unsigned char)text[i], false, false);
        for (int y = 0; y < *height; ++y)
            for (int x = 0; x < font->width; ++x) {
                uint8_t value = glyph[(y * 24 / *height) * 12 + x * 12 / font->width];
                if (font->underline && y == *height - 1) value = 255;
                size_t at = (size_t)y * *width + i * font->width + x;
                pixels[at] |= value;
                if (bold && x + 1 < font->width) pixels[at + 1] |= value;
            }
    }
    return pixels;
}
