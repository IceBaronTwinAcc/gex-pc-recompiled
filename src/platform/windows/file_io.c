#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <string.h>
#include "platform/filesystem.h"

int gex_file_open(const char *path, unsigned flags) {
    int native = (flags & GEX_FILE_WRITE) ?
        ((flags & GEX_FILE_READ) ? _O_RDWR : _O_WRONLY) : _O_RDONLY;
    if (flags & GEX_FILE_CREATE) native |= _O_CREAT;
    if (flags & GEX_FILE_EXCLUSIVE) native |= _O_EXCL;
    if (flags & GEX_FILE_TRUNCATE) native |= _O_TRUNC;
    return _open(path, native | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
}
int gex_file_close(int file) { return _close(file); }
int64_t gex_file_read(int file, void *data, size_t bytes) {
    return _read(file, data, (unsigned)(bytes > INT_MAX ? INT_MAX : bytes));
}
enum gex_io_error gex_fs_error(void) {
    switch (errno) {
    case ENOENT: return GEX_IO_MISSING;
    case ENOTDIR: return GEX_IO_NOT_DIRECTORY;
    case EEXIST: return GEX_IO_EXISTS;
    case EACCES: case EPERM: return GEX_IO_DENIED;
    case EINTR: return GEX_IO_INTERRUPTED;
    default: return GEX_IO_OTHER;
    }
}
const char *gex_fs_error_text(void) { return strerror(errno); }
