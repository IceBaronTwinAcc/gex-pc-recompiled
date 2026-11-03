#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "platform/font.h"

const char *gex_font_locate(const char *family, int bold, int italic) {
    static char path[512];
    const char *name = strstr(family, "Mono") ? "RobotoMono" :
                       strstr(family, "Serif") ? "NotoSerif" : "Roboto";
    const char *style = bold && italic ? "BoldItalic" : bold ? "Bold" :
                        italic ? "Italic" : "Regular";
    snprintf(path, sizeof path, "/system/fonts/%s-%s.ttf", name, style);
    if (access(path, R_OK) == 0) return path;
    static const char *const fallback[] = {
        "/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
        "/system/fonts/Roboto-VF.ttf", "/system/fonts/NotoSans-Regular.ttf"
    };
    for (size_t i = 0; i < sizeof fallback / sizeof *fallback; ++i)
        if (access(fallback[i], R_OK) == 0) return fallback[i];
    fprintf(stderr, "[font] no readable Android system font for %s\n", family);
    return NULL;
}
