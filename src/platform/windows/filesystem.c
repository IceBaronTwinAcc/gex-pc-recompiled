#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "platform/filesystem.h"
#include "win32hle.h"

static int native_error(void) {
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: errno = EACCES; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: errno = EEXIST; break;
    case ERROR_DIRECTORY: errno = ENOTDIR; break;
    case ERROR_INVALID_HANDLE: errno = EBADF; break;
    case ERROR_DISK_FULL: errno = ENOSPC; break;
    default: errno = EIO; break;
    }
    return -1;
}
int gex_file_duplicate(int file) { return _dup(file); }
int64_t gex_file_write(int file, const void *data, size_t bytes) {
    return _write(file, data, (unsigned)(bytes > INT_MAX ? INT_MAX : bytes));
}
int64_t gex_file_seek(int file, int64_t offset, int origin) { return _lseeki64(file, offset, origin); }
int gex_file_truncate(int file, int64_t bytes) {
    errno_t error = _chsize_s(file, bytes);
    if (error) errno = error;
    return error ? -1 : 0;
}
int gex_file_is_terminal(int file) { return _isatty(file); }
static gex_file_time from_time(FILETIME time) {
    uint64_t ticks = ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
    return (gex_file_time){(int64_t)(ticks / 10000000) - 11644473600LL,
                           (long)(ticks % 10000000) * 100};
}
static int handle_stat(HANDLE handle, gex_file_info *info) {
    BY_HANDLE_FILE_INFORMATION value;
    if (!GetFileInformationByHandle(handle, &value)) return native_error();
    *info = (gex_file_info){
        .size = ((uint64_t)value.nFileSizeHigh << 32) | value.nFileSizeLow,
        .device = value.dwVolumeSerialNumber,
        .inode = ((uint64_t)value.nFileIndexHigh << 32) | value.nFileIndexLow,
        .links = value.nNumberOfLinks,
        .directory = !!(value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
        .writable = !(value.dwFileAttributes & FILE_ATTRIBUTE_READONLY),
        .accessed = from_time(value.ftLastAccessTime), .modified = from_time(value.ftLastWriteTime)
    };
    return 0;
}
int gex_file_stat(int file, gex_file_info *info) {
    return handle_stat((HANDLE)_get_osfhandle(file), info);
}
int gex_path_stat(const char *path, gex_file_info *info) {
    HANDLE handle = CreateFileA(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) return native_error();
    int result = handle_stat(handle, info);
    CloseHandle(handle);
    return result;
}
int gex_file_set_times(int file, const gex_file_time times[2]) {
    FILETIME native[2];
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t ticks = ((uint64_t)times[i].seconds + 11644473600ULL) * 10000000 +
            (times[i].nanoseconds < 0 ? 0 : times[i].nanoseconds / 100);
        native[i] = (FILETIME){(DWORD)ticks, (DWORD)(ticks >> 32)};
    }
    return SetFileTime((HANDLE)_get_osfhandle(file), NULL,
        times[0].nanoseconds < 0 ? NULL : &native[0],
        times[1].nanoseconds < 0 ? NULL : &native[1]) ? 0 : native_error();
}
int gex_directory_create(const char *path) { return _mkdir(path); }
int gex_directory_remove(const char *path) { return _rmdir(path); }
int gex_file_remove(const char *path) { return _unlink(path); }
int gex_file_rename(const char *from, const char *to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : native_error();
}
static char *slashes(char *path) {
    if (path) for (char *p = path; *p; ++p) if (*p == '\\') *p = '/';
    return path;
}
char *gex_fs_current_directory(char *path, size_t size) {
    if (size > INT_MAX) { errno = ERANGE; return NULL; }
    return slashes(_getcwd(path, (int)size));
}
int gex_fs_enter_directory(const char *path) { return _chdir(path); }
char *gex_fs_canonical_path(const char *path) {
    char *full = _fullpath(NULL, path, 0);
    if (full && _access(full, 0)) { free(full); return NULL; }
    return slashes(full);
}
int gex_fs_readable(const char *path) { return _access(path, 4) == 0; }
const char *gex_fs_device_path(int console) { return console ? "CON" : "NUL"; }
const char *gex_fs_default_drive_root(unsigned drive) {
    static const char roots[][4] = {
        "A:/", "B:/", "C:/", "D:/", "E:/", "F:/", "G:/", "H:/", "I:/",
        "J:/", "K:/", "L:/", "M:/", "N:/", "O:/", "P:/", "Q:/", "R:/",
        "S:/", "T:/", "U:/", "V:/", "W:/", "X:/", "Y:/", "Z:/"
    };
    return drive < 26 ? roots[drive] : NULL;
}
int gex_fs_disk_info(const char *path, gex_disk_info *info) {
    ULARGE_INTEGER available, total, free;
    if (!GetDiskFreeSpaceExA(path, &available, &total, &free)) return native_error();
    *info = (gex_disk_info){1, available.QuadPart, total.QuadPart, free.QuadPart};
    return 0;
}
struct gex_directory {
    HANDLE search;
    WIN32_FIND_DATAA data;
    int first;
    gex_directory_entry entry;
};
gex_directory *gex_directory_open(const char *path) {
    size_t bytes = strlen(path) + 3;
    char *pattern = malloc(bytes);
    if (!pattern) hle_fatal("cannot allocate directory pattern");
    snprintf(pattern, bytes, "%s/*", path);
    gex_directory *directory = calloc(1, sizeof *directory);
    if (!directory) hle_fatal("cannot allocate directory iterator");
    directory->search = FindFirstFileA(pattern, &directory->data);
    free(pattern);
    if (directory->search == INVALID_HANDLE_VALUE) {
        native_error();
        free(directory);
        return NULL;
    }
    directory->first = 1;
    return directory;
}
const gex_directory_entry *gex_directory_next(gex_directory *directory) {
    if (!directory->first && !FindNextFileA(directory->search, &directory->data)) {
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            native_error();
            fprintf(stderr, "[gex] read directory: %s\n", strerror(errno));
        }
        return NULL;
    }
    directory->first = 0;
    snprintf(directory->entry.name, sizeof directory->entry.name, "%s", directory->data.cFileName);
    directory->entry.directory = !!(directory->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    return &directory->entry;
}
void gex_directory_close(gex_directory *directory) {
    if (!FindClose(directory->search))
        fprintf(stderr, "[gex] close directory: Windows error %lu\n", GetLastError());
    free(directory);
}
FILE *gex_fs_replace_begin(const char *path, char **temporary) {
    size_t bytes = strlen(path) + 40;
    *temporary = malloc(bytes);
    if (!*temporary) hle_fatal("cannot allocate atomic replacement path");
    static atomic_uint sequence;
    for (unsigned i = 0; i < 100; ++i) {
        snprintf(*temporary, bytes, "%s.%lu.%u", path, GetCurrentProcessId(),
                 atomic_fetch_add(&sequence, 1));
        int file = _open(*temporary, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                         _S_IREAD | _S_IWRITE);
        if (file >= 0) {
            FILE *stream = _fdopen(file, "wb");
            if (stream) return stream;
            int error = errno;
            _close(file);
            _unlink(*temporary);
            errno = error;
            break;
        }
        if (errno != EEXIST) break;
    }
    free(*temporary);
    *temporary = NULL;
    return NULL;
}
int gex_fs_replace_commit(FILE *file, const char *temporary, const char *path) {
    int error = ferror(file) ? EIO : 0;
    if (fflush(file) && !error) error = errno;
    if (!error && _commit(_fileno(file))) error = errno;
    if (fclose(file) && !error) error = errno;
    if (!error && gex_file_rename(temporary, path)) error = errno;
    if (error) { _unlink(temporary); errno = error; }
    return error ? -1 : 0;
}
