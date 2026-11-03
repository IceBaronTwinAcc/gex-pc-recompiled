#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#ifdef __linux__
#include <fcntl.h>
#endif
#endif
#include "game_install.h"
#include "identity.h"
#include "sha256.h"
#include "game_assets.h"

#define PATH_SIZE 4096

static int fail(char *error, size_t size, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
    return -1;
}

static int join(char *path, const char *parent, const char *name) {
    int n = snprintf(path, PATH_SIZE, "%s/%s", parent, name);
    if (n < 0 || n >= PATH_SIZE) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

#ifdef _WIN32
static int wide(const char *path, wchar_t *out) {
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, PATH_SIZE))
        return 0;
    errno = EINVAL;
    return -1;
}
static int windows_error(void) {
    DWORD code = GetLastError();
    errno = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? ENOENT :
        code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS ? EEXIST :
        code == ERROR_ACCESS_DENIED ? EACCES : EIO;
    return -1;
}
#endif

/* 0 missing, 1 ordinary file, 2 directory, 3 link or special file, -1 error. */
static int kind(const char *path) {
#ifdef _WIN32
    wchar_t native[PATH_SIZE];
    if (wide(path, native)) return -1;
    DWORD attributes = GetFileAttributesW(native);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        windows_error();
        return errno == ENOENT ? 0 : -1;
    }
    if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return 3;
    return attributes & FILE_ATTRIBUTE_DIRECTORY ? 2 : 1;
#else
    struct stat value;
    if (lstat(path, &value)) return errno == ENOENT ? 0 : -1;
    return S_ISREG(value.st_mode) ? 1 : S_ISDIR(value.st_mode) ? 2 : 3;
#endif
}

static int make_directory(const char *path) {
#ifdef _WIN32
    wchar_t native[PATH_SIZE];
    if (wide(path, native)) return -1;
    return CreateDirectoryW(native, NULL) ? 0 : windows_error();
#else
    return mkdir(path, 0755);
#endif
}

static int remove_path(const char *path, int directory) {
#ifdef _WIN32
    wchar_t native[PATH_SIZE];
    if (wide(path, native)) return -1;
    return (directory ? RemoveDirectoryW(native) : DeleteFileW(native)) ? 0 : windows_error();
#else
    return directory ? rmdir(path) : unlink(path);
#endif
}

typedef int (*visit)(const char *, const char *, void *);
static int walk(const char *path, visit callback, void *context) {
    int result = 0;
#ifdef _WIN32
    char pattern[PATH_SIZE];
    wchar_t native[PATH_SIZE];
    if (join(pattern, path, "*") || wide(pattern, native)) return -1;
    WIN32_FIND_DATAW entry;
    HANDLE directory = FindFirstFileW(native, &entry);
    if (directory == INVALID_HANDLE_VALUE) return windows_error();
    do {
        char name[PATH_SIZE];
        if (!WideCharToMultiByte(CP_UTF8, 0, entry.cFileName, -1,
                                name, sizeof name, NULL, NULL)) { result = -1; break; }
        if (strcmp(name, ".") && strcmp(name, "..") && callback(path, name, context)) {
            result = -1;
            break;
        }
    } while (FindNextFileW(directory, &entry));
    if (!result && GetLastError() != ERROR_NO_MORE_FILES) result = windows_error();
    if (!FindClose(directory) && !result) result = windows_error();
#else
    DIR *directory = opendir(path);
    if (!directory) return -1;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) result = -1; break; }
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
            callback(path, entry->d_name, context)) { result = -1; break; }
    }
    int saved = errno;
    if (closedir(directory) && !result) { result = -1; saved = errno; }
    errno = saved;
#endif
    return result;
}

static int clean_entry(const char *parent, const char *name, void *context) {
    (void)context;
    char path[PATH_SIZE];
    if (join(path, parent, name)) return -1;
    int type = kind(path);
    if (type < 0) return -1;
    if (type == 2 && walk(path, clean_entry, NULL)) return -1;
    return remove_path(path, type == 2);
}

static FILE *open_file(const char *path, int write) {
#ifdef _WIN32
    wchar_t native[PATH_SIZE];
    if (wide(path, native)) return NULL;
    int fd = _wopen(native, _O_BINARY | _O_NOINHERIT |
        (write ? _O_WRONLY | _O_CREAT | _O_EXCL : _O_RDONLY), _S_IREAD | _S_IWRITE);
    if (fd < 0) return NULL;
    FILE *file = _fdopen(fd, write ? "wb" : "rb");
    if (!file) _close(fd);
    return file;
#else
    return fopen(path, write ? "wbx" : "rb");
#endif
}

static int copy_file(const char *source, const char *target) {
    FILE *input = open_file(source, 0);
    if (!input) return -1;
    FILE *output = open_file(target, 1);
    int result = output ? 0 : -1;
    if (output) {
        unsigned char buffer[65536];
        size_t count;
        while ((count = fread(buffer, 1, sizeof buffer, input)))
            if (fwrite(buffer, 1, count, output) != count) { result = -1; break; }
        if (ferror(input)) result = -1;
        if (fclose(output)) result = -1;
    }
    if (fclose(input)) result = -1;
    return result;
}

static int verify_executable(const char *path) {
    FILE *file = open_file(path, 0);
    if (!file) {
        fprintf(stderr, "[setup] cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }
    gex_sha256 hash;
    gex_sha256_init(&hash);
    unsigned char buffer[8192], digest[32];
    size_t count;
    while ((count = fread(buffer, 1, sizeof buffer, file)))
        gex_sha256_update(&hash, buffer, count);
    int failed = ferror(file);
    if (fclose(file)) failed = 1;
    if (failed) {
        fprintf(stderr, "[setup] cannot hash %s: %s\n", path, strerror(errno));
        return -1;
    }
    gex_sha256_final(&hash, digest);
    return gex_verify_executable_digest(digest);
}

static int verify_assets(const char *root, char *error, size_t size) {
    char path[PATH_SIZE];
    for (unsigned i = 0; i < sizeof gex_required_assets / sizeof *gex_required_assets; ++i) {
        const char *name = gex_required_assets[i].path;
        if (join(path, root, name) || kind(path) != 1)
            return fail(error, size, "Missing or invalid required game file: %s", name);
        FILE *file = open_file(path, 0);
        if (!file) return fail(error, size, "Cannot read required game file %s: %s", name, strerror(errno));
        gex_sha256 hash;
        gex_sha256_init(&hash);
        unsigned char buffer[65536], digest[32];
        uint64_t bytes = 0;
        size_t count;
        while ((count = fread(buffer, 1, sizeof buffer, file))) {
            bytes += count;
            if (bytes > gex_required_assets[i].size) break;
            gex_sha256_update(&hash, buffer, count);
        }
        int failed = ferror(file);
        if (fclose(file)) failed = 1;
        if (failed)
            return fail(error, size, "Cannot read required game file %s: %s", name, strerror(errno));
        if (bytes != gex_required_assets[i].size)
            return fail(error, size, "Required game file has an incorrect size: %s. "
                        "Reinstall the game from GOG.", name);
        gex_sha256_final(&hash, digest);
        char actual[65];
        for (unsigned j = 0; j < sizeof digest; ++j)
            snprintf(actual + j * 2, 3, "%02x", digest[j]);
        if (strcmp(actual, gex_required_assets[i].sha256))
            return fail(error, size, "Required game file failed integrity check: %s. "
                        "Reinstall the game from GOG.", name);
    }
    return 0;
}

struct copy_context { const char *target; unsigned depth; };
static int copy_entry(const char *parent, const char *name, void *opaque) {
    struct copy_context *context = opaque;
    char source[PATH_SIZE], target[PATH_SIZE];
    if (join(source, parent, name) || join(target, context->target, name)) return -1;
    int type = kind(source);
    if (type == 2) {
        if (context->depth >= 32) { errno = ELOOP; return -1; }
        if (make_directory(target)) return -1;
        struct copy_context child = {target, context->depth + 1};
        return walk(source, copy_entry, &child);
    }
    if (type != 1) { errno = EINVAL; return -1; }
    return copy_file(source, target);
}

static int absolute(const char *path, char *output) {
#ifdef _WIN32
    wchar_t native[PATH_SIZE], full[PATH_SIZE];
    if (wide(path, native)) return -1;
    DWORD n = GetFullPathNameW(native, PATH_SIZE, full, NULL);
    if (!n || n >= PATH_SIZE ||
        !WideCharToMultiByte(CP_UTF8, 0, full, -1, output, PATH_SIZE, NULL, NULL))
        return windows_error();
    for (char *p = output; *p; ++p) if (*p == '\\') *p = '/';
    return 0;
#else
    char *full = realpath(path, NULL);
    if (!full) return -1;
    int n = snprintf(output, PATH_SIZE, "%s", full);
    free(full);
    if (n < 0 || n >= PATH_SIZE) { errno = ENAMETOOLONG; return -1; }
    return 0;
#endif
}

static int publish(const char *stage, const char *destination) {
#ifdef _WIN32
    wchar_t from[PATH_SIZE], to[PATH_SIZE];
    if (wide(stage, from) || wide(destination, to)) return -1;
    return MoveFileW(from, to) ? 0 : windows_error();
#elif defined(__APPLE__)
    return renamex_np(stage, destination, RENAME_EXCL);
#else
    return renameat2(AT_FDCWD, stage, AT_FDCWD, destination, RENAME_NOREPLACE);
#endif
}

static int validate_layout(const char *root, char *error, size_t size) {
    char path[PATH_SIZE];
    const char *required[] = {"GEX.exe", "AVI/GEX000.AVI", "AVI/GEX001.AVI",
        "IDL/GEX000.IDL", "LEV/GEX001.LEV", "LEV/GEX125.LEV"};
    for (unsigned i = 0; i < sizeof required / sizeof *required; ++i) {
        if (join(path, root, required[i]) || kind(path) != 1)
            return fail(error, size, "Choose the original Gex installation folder. Missing or invalid: %s",
                        required[i]);
    }
    const char *directories[] = {"AVI", "IDL", "LEV", "MUS", "SFX", "VFX"};
    for (unsigned i = 0; i < sizeof directories / sizeof *directories; ++i) {
        if (join(path, root, directories[i]) || kind(path) != 2)
            return fail(error, size, "The selected game folder is missing %s", directories[i]);
    }
    return 0;
}

int gex_install_game(const char *source, const char *destination, char *error, size_t size) {
    char path[PATH_SIZE], root[PATH_SIZE], parent[PATH_SIZE], stage[PATH_SIZE];
    if (kind(destination) != 0)
        return fail(error, size, "Cannot import into %s: the destination already exists or is inaccessible. "
                    "It will not be overwritten. Use --data to select an existing installation.", destination);
    if (absolute(source, root))
        return fail(error, size, "Cannot open the selected game folder: %s", strerror(errno));
    if (kind(root) != 2)
        return fail(error, size, "The selected path is not an ordinary game directory; links are not supported");
    if (validate_layout(root, error, size)) return -1;
    int n = snprintf(path, sizeof path, "%s", destination);
    if (n < 0 || n >= (int)sizeof path)
        return fail(error, size, "The destination path is too long");
    char *separator = strrchr(path, '/');
    if (separator) {
        if (separator == path) separator[1] = '\0';
        else *separator = '\0';
    } else strcpy(path, ".");
    if (absolute(path, parent))
        return fail(error, size, "Cannot resolve the import destination: %s", strerror(errno));
    size_t length = strlen(root);
#ifdef _WIN32
    int inside = !_strnicmp(root, parent, length);
#else
    int inside = !strncmp(root, parent, length);
#endif
    if (inside && (root[length - 1] == '/' || !parent[length] || parent[length] == '/'))
        return fail(error, size, "The import destination must not be inside the selected game folder");
    if (verify_assets(root, error, size)) return -1;
    if (join(stage, parent, ".gex-import-XXXXXX"))
        return fail(error, size, "The import destination path is too long");
#ifdef _WIN32
    static unsigned sequence;
    int created = 0;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        int written = snprintf(stage, sizeof stage, "%s/.gex-import-%lu-%u", parent,
                               GetCurrentProcessId(), sequence++);
        if (written < 0 || written >= (int)sizeof stage)
            return fail(error, size, "The import staging path is too long");
        if (!make_directory(stage)) { created = 1; break; }
        if (errno != EEXIST) break;
    }
    if (!created)
#else
    if (!mkdtemp(stage))
#endif
        return fail(error, size, "Cannot create an import directory: %s. "
                    "Move the application to a writable folder and try again.", strerror(errno));
    int result = -1;
    struct copy_context context = {stage, 0};
    if (walk(root, copy_entry, &context))
        fail(error, size, "Cannot copy the game folder: %s. Links and special files are not supported.",
             strerror(errno));
    else if (!verify_assets(stage, error, size)) {
        if (publish(stage, destination))
            fail(error, size, "Cannot finish the import without replacing existing files: %s", strerror(errno));
        else result = 0;
    }
    if (result && (walk(stage, clean_entry, NULL) || remove_path(stage, 1)))
        fprintf(stderr, "[setup] cannot remove incomplete import %s: %s\n", stage, strerror(errno));
    return result;
}

int gex_prepare_game(const char *destination, int interactive) {
    char path[PATH_SIZE], error[1024];
    int existing = kind(destination);
    if (existing == 2) {
        if (!validate_layout(destination, error, sizeof error)) {
            if (!join(path, destination, "GEX.exe") && !verify_executable(path)) return 0;
            snprintf(error, sizeof error, "The installed GEX.exe is unreadable or is not the supported GOG executable");
        }
        fprintf(stderr, "[setup] %s; existing files in %s will not be overwritten\n", error, destination);
        if (interactive) gex_setup_error(error);
        return -1;
    }
    if (existing != 0) {
        snprintf(error, sizeof error, "The game destination %s is inaccessible or is not a directory. "
                 "It will not be overwritten.", destination);
        fprintf(stderr, "[setup] %s\n", error);
        if (interactive) gex_setup_error(error);
        return -1;
    }
    if (!interactive) {
        fprintf(stderr, "[setup] game data not found in %s; use --data PATH or launch interactively to import it\n",
                destination);
        return -1;
    }
    for (;;) {
        int selected = gex_pick_game_folder(path, sizeof path, error, sizeof error);
        if (selected == 1) { fprintf(stderr, "[setup] game import cancelled\n"); return 1; }
        if (selected < 0) {
            fprintf(stderr, "[setup] %s\n", error);
            gex_setup_error(error);
            return -1;
        }
        if (!gex_install_game(path, destination, error, sizeof error)) {
            fprintf(stderr, "[setup] copied original game to %s\n", destination);
            return 0;
        }
        fprintf(stderr, "[setup] %s\n", error);
        gex_setup_error(error);
        if (kind(destination) != 0) return -1;
    }
}
