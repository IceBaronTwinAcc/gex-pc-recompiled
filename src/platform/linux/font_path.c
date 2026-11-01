#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "platform/font.h"

const char *gex_font_locate(const char *family, int bold, int italic) {
    static char path[512];
    const char *style = bold && italic ? "Bold Italic" : bold ? "Bold" : italic ? "Italic" : "Regular";
    char cmd[256];
    snprintf(cmd, sizeof cmd, "fc-match -f '%%{file}' '%s:style=%s' 2>/dev/null", family, style);
    FILE *p = popen(cmd, "r");
    if (p) {
        size_t n = fread(path, 1, sizeof path - 1, p);
        pclose(p);
        path[n] = 0;
        if (n && access(path, R_OK) == 0) return path;
    }
    static const char *const dirs[] = { "/usr/share/fonts/truetype/liberation/", "/usr/share/fonts/truetype/liberation2/",
                                        "/usr/share/fonts/liberation/", "/usr/share/fonts/liberation-sans/",
                                        "/usr/share/fonts/TTF/", "/usr/share/fonts/truetype/dejavu/" };
    char file[64];
    snprintf(file, sizeof file, "%s-%s.ttf", strstr(family, "Serif") ? "LiberationSerif" : strstr(family, "Mono") ? "LiberationMono" : "LiberationSans",
             bold && italic ? "BoldItalic" : bold ? "Bold" : italic ? "Italic" : "Regular");
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, sizeof path, "%s%s", dirs[i], file);
        if (access(path, R_OK) == 0) return path;
    }
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, sizeof path, "%s%s", dirs[i], bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf");
        if (access(path, R_OK) == 0) return path;
    }
    const char *mac = strstr(family, "Serif") ? "Times New Roman" : strstr(family, "Mono") ? "Courier New" : "Arial";
    snprintf(path, sizeof path, "/System/Library/Fonts/Supplemental/%s%s.ttf", mac,
             bold && italic ? " Bold Italic" : bold ? " Bold" : italic ? " Italic" : "");
    if (access(path, R_OK) == 0) return path;
    return NULL;
}
