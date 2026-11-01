#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <io.h>
#include "platform/font.h"
#include "platform/filesystem.h"

const char *gex_font_locate(const char *family, int bold, int italic) {
    static char path[1024];
    char directory[MAX_PATH];
    UINT length = GetWindowsDirectoryA(directory, sizeof directory);
    if (!length || length >= sizeof directory) return NULL;
    const char *fallback = strstr(family, "Serif") ? "Times New Roman" :
        strstr(family, "Mono") ? "Courier New" : "Arial";
    const char *faces[] = {family, fallback, "Tahoma"};
    for (unsigned i = 0; i < sizeof faces / sizeof *faces; ++i) {
        char name[256], raw[768], file[768];
        snprintf(name, sizeof name, "%s%s (TrueType)", faces[i],
                 bold && italic ? " Bold Italic" : bold ? " Bold" : italic ? " Italic" : "");
        DWORD bytes = sizeof raw;
        LSTATUS status = RegGetValueA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", name,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL, raw, &bytes);
        if (status != ERROR_SUCCESS) continue;
        DWORD expanded = ExpandEnvironmentStringsA(raw, file, sizeof file);
        if (!expanded || expanded > sizeof file) continue;
        int size = gex_fs_path_absolute(file) ?
            snprintf(path, sizeof path, "%s", file) :
            snprintf(path, sizeof path, "%s/Fonts/%s", directory, file);
        if (size >= 0 && (size_t)size < sizeof path && !_access(path, 4)) return path;
    }
    fprintf(stderr, "[font] cannot find an installed TrueType font for %s\n", family);
    return NULL;
}
