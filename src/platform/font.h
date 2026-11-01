#ifndef GEX_PLATFORM_FONT_H
#define GEX_PLATFORM_FONT_H

#include <stddef.h>
#include <stdint.h>

typedef struct gex_font gex_font;
const char *gex_font_locate(const char *family, int bold, int italic);
int gex_font_initialize(void);
const char *gex_font_error(void);
gex_font *gex_font_open(const char *path, const void *data, size_t bytes, int size);
void gex_font_close(gex_font *font);
const char *gex_font_family(gex_font *font);
void gex_font_configure(gex_font *font, int crisp, int italic, int underline);
int gex_font_measure(gex_font *font, const char *text, int *width, int *height);
int gex_font_height(gex_font *font);
int gex_font_ascent(gex_font *font);
int gex_font_descent(gex_font *font);
/* Returns malloc-owned 0..255 coverage, tightly packed with pitch == width. */
uint8_t *gex_font_coverage(gex_font *font, const char *text, int count,
                          int crisp, int synthetic_bold, int *width, int *height);

#endif
