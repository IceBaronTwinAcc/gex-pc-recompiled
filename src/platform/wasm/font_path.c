#include <stdio.h>
#include <unistd.h>
#include "platform/font.h"

const char *gex_font_locate(const char *family, int bold, int italic) {
    static char path[512];
    static const char *const roots[] = {
        "/Gex/Fonts", "/Gex/fonts", "/fonts", "/Gex"
    };
    const char *style = bold && italic ? "Bold Italic" :
                        bold ? "Bold" : italic ? "Italic" : "Regular";
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; ++i) {
        int length = snprintf(path, sizeof path, "%s/%s-%s.ttf", roots[i], family, style);
        if (length >= 0 && (size_t)length < sizeof path && access(path, R_OK) == 0)
            return path;
        length = snprintf(path, sizeof path, "%s/%s.ttf", roots[i], family);
        if (length >= 0 && (size_t)length < sizeof path && access(path, R_OK) == 0)
            return path;
    }
    return NULL;
}
