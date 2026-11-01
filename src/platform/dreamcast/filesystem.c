#include <kos.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform/filesystem.h"
#include "platform/runtime.h"
#include "win32hle.h"

static uint8_t read_buffer[65536] __attribute__((aligned(32)));
static mutex_t read_lock = MUTEX_INITIALIZER;
static void *cached_handle;
static int64_t cached_position;
static size_t cached_bytes;

enum gex_io_error gex_fs_error(void) {
    switch (errno) {
    case ENOENT: return GEX_IO_MISSING;
    case ENOTDIR: return GEX_IO_NOT_DIRECTORY;
    case EEXIST: return GEX_IO_EXISTS;
    case EACCES: case EPERM: case EROFS: return GEX_IO_DENIED;
    case EINTR: return GEX_IO_INTERRUPTED;
    default: return GEX_IO_OTHER;
    }
}
const char *gex_fs_error_text(void) { return strerror(errno); }
int gex_file_open(const char *path, unsigned flags) {
    int mode = flags & GEX_FILE_WRITE ?
        (flags & GEX_FILE_READ ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (flags & GEX_FILE_CREATE) mode |= O_CREAT;
    if (flags & GEX_FILE_EXCLUSIVE) mode |= O_EXCL;
    if (flags & GEX_FILE_TRUNCATE) mode |= O_TRUNC;
    int file = open(path, mode, 0644);
    if (file >= 0) {
        if (mutex_lock(&read_lock)) hle_fatal("cannot lock disc transfer buffer");
        if (fs_get_handle(file) == cached_handle) cached_handle = NULL;
        if (mutex_unlock(&read_lock)) hle_fatal("cannot unlock disc transfer buffer");
    }
    return file;
}
int gex_file_close(int file) {
    if (mutex_lock(&read_lock)) hle_fatal("cannot lock disc transfer buffer");
    if (fs_get_handler(file) && fs_get_handle(file) == cached_handle) cached_handle = NULL;
    if (mutex_unlock(&read_lock)) hle_fatal("cannot unlock disc transfer buffer");
    return close(file);
}
int gex_file_duplicate(int file) { return dup(file); }

static int64_t file_read(int file, void *data, size_t bytes) {
    if (bytes < 2048 || !((uintptr_t)data & 31)) return read(file, data, bytes);
    int saved_errno = errno;
    int64_t position = fs_seek64(file, 0, SEEK_CUR);
    if (position < 0) {
        errno = saved_errno;
        return read(file, data, bytes);
    }
    if (mutex_lock(&read_lock)) hle_fatal("cannot lock disc transfer buffer");
    cached_handle = NULL;
    size_t done = 0;
    int64_t result = 0;
    while (done < bytes) {
        size_t count = bytes - done;
        unsigned prefix = (unsigned)position & 2047;
        int buffered = !prefix && count >= 2048;
        if (prefix) {
            size_t remaining = 2048 - prefix;
            if (count > remaining) count = remaining;
        } else if (buffered) {
            if (count > sizeof read_buffer) count = sizeof read_buffer;
            count &= ~(size_t)2047;
        }
        /* KOS ISO9660 only uses multi-sector DMA for sector-aligned reads
         * into 32-byte-aligned buffers. Guest heap pointers need not be aligned. */
        result = read(file, buffered ? read_buffer : (uint8_t *)data + done, count);
        if (result <= 0) break;
        if (buffered) memcpy((uint8_t *)data + done, read_buffer, (size_t)result);
        done += (size_t)result;
        position += result;
        if ((size_t)result < count) break;
    }
    int read_errno = errno;
    if (mutex_unlock(&read_lock)) hle_fatal("cannot unlock disc transfer buffer");
    errno = read_errno;
    return done ? (int64_t)done : result;
}

static int64_t disc_read(int file, void *data, size_t bytes) {
    if (!bytes) return 0;
    int64_t position = fs_seek64(file, 0, SEEK_CUR);
    if (position < 0) return -1;
    if (mutex_lock(&read_lock)) hle_fatal("cannot lock disc read-ahead buffer");
    void *handle = fs_get_handle(file);
    size_t done = 0;
    int64_t result = 0;
    while (done < bytes) {
        if (cached_handle != handle || position < cached_position ||
            (uint64_t)(position - cached_position) >= cached_bytes) {
            cached_handle = NULL;
            cached_position = position & ~(int64_t)2047;
            if (fs_seek64(file, cached_position, SEEK_SET) < 0) {
                result = -1;
                break;
            }
            result = read(file, read_buffer, sizeof read_buffer);
            if (result <= 0) break;
            cached_bytes = (size_t)result;
            cached_handle = handle;
        }
        size_t offset = (size_t)(position - cached_position);
        if (offset >= cached_bytes) break;
        size_t count = cached_bytes - offset;
        if (count > bytes - done) count = bytes - done;
        memcpy((uint8_t *)data + done, read_buffer + offset, count);
        done += count;
        position += count;
        if (cached_bytes < sizeof read_buffer && offset + count == cached_bytes) break;
    }
    int read_errno = errno;
    /* Prefetch must not change the descriptor's logical offset, including
     * for duplicate handles, short reads, EOF and failed transfers. */
    if (fs_seek64(file, position, SEEK_SET) < 0) {
        result = -1;
        done = 0;
        read_errno = errno;
        cached_handle = NULL;
    }
    if (mutex_unlock(&read_lock)) hle_fatal("cannot unlock disc read-ahead buffer");
    errno = read_errno;
    return done ? (int64_t)done : result < 0 ? result : 0;
}

int64_t gex_file_read(int file, void *data, size_t bytes) {
    vfs_handler_t *handler = fs_get_handler(file);
    /* Only immutable CD assets are prefetched; RAM files retain ordinary
     * read/write visibility. Reuse the existing aligned DMA transfer buffer. */
    if (!handler || strcmp(handler->nmmgr.pathname, "/cd")) return file_read(file, data, bytes);
    if (!bytes) return 0;
    size_t capacity = bytes < sizeof read_buffer ? bytes : sizeof read_buffer;
    uint8_t *buffer = malloc(capacity);
    if (!buffer) { errno = ENOMEM; return -1; }
    int pinned = gex_file_duplicate(file);
    if (pinned < 0) {
        int error = errno;
        free(buffer);
        errno = error;
        return -1;
    }
    size_t done = 0;
    int64_t result = 0;
    int error = errno;
    while (done < bytes) {
        size_t count = bytes - done;
        if (count > capacity) count = capacity;
        /* No guest registers or memory are accessed while blocked. Retire
         * the disc-buffer lock before reacquiring the machine to avoid
         * lock inversion with another guest performing a file operation. */
        hle_block_begin();
        result = disc_read(pinned, buffer, count);
        error = errno;
        hle_block_end();
        if (result <= 0) break;
        memcpy((uint8_t *)data + done, buffer, (size_t)result);
        done += (size_t)result;
        if ((size_t)result < count) break;
    }
    /* This pin shares the original KOS open-file identity. Closing it must
     * not discard read-ahead data while the caller's descriptor remains open. */
    hle_block_begin();
    int closed = close(pinned);
    int close_error = errno;
    hle_block_end();
    free(buffer);
    if (closed < 0) { errno = close_error; return -1; }
    errno = error;
    return done ? (int64_t)done : result;
}
int64_t gex_file_write(int file, const void *data, size_t bytes) { return write(file, data, bytes); }
int64_t gex_file_seek(int file, int64_t offset, int origin) {
    return fs_seek64(file, offset, origin);
}
int gex_file_truncate(int file, int64_t bytes) {
    (void)file; (void)bytes;
    fprintf(stderr, "[dreamcast] descriptor truncation is unsupported\n");
    errno = ENOTSUP;
    return -1;
}
int gex_file_is_terminal(int file) { return isatty(file); }
static void translate(const struct stat *native, gex_file_info *info) {
    *info = (gex_file_info){.size = native->st_size, .device = native->st_dev,
        .inode = native->st_ino, .links = native->st_nlink,
        .directory = S_ISDIR(native->st_mode), .writable = !!(native->st_mode & S_IWUSR),
        .accessed = {native->st_atime, 0}, .modified = {native->st_mtime, 0}};
}
int gex_file_stat(int file, gex_file_info *info) {
    struct stat value;
    if (fstat(file, &value)) return -1;
    translate(&value, info);
    return 0;
}
int gex_path_stat(const char *path, gex_file_info *info) {
    struct stat value;
    if (stat(path, &value)) return -1;
    translate(&value, info);
    return 0;
}
int gex_file_set_times(int file, const gex_file_time times[2]) {
    (void)file; (void)times;
    fprintf(stderr, "[dreamcast] changing file timestamps is unsupported\n");
    errno = ENOTSUP;
    return -1;
}
int gex_directory_create(const char *path) { return mkdir(path, 0755); }
int gex_directory_remove(const char *path) { return rmdir(path); }
int gex_file_remove(const char *path) { return unlink(path); }
int gex_file_rename(const char *from, const char *to) { return rename(from, to); }
char *gex_fs_current_directory(char *path, size_t size) { return getcwd(path, size); }
int gex_fs_enter_directory(const char *path) { return chdir(path); }
char *gex_fs_canonical_path(const char *path) {
    char buffer[PATH_MAX];
    if (!realpath(path, buffer)) return NULL;
    return gex_string_duplicate(buffer);
}
int gex_fs_readable(const char *path) {
    struct stat info;
    return !stat(path, &info) && (info.st_mode & S_IRUSR);
}
const char *gex_fs_device_path(int console) { return console ? "/dev/tty" : "/dev/null"; }
const char *gex_fs_default_drive_root(unsigned drive) {
    return drive == 'z' - 'a' ? "/" : NULL;
}
int gex_fs_disk_info(const char *path, gex_disk_info *info) {
    (void)path; (void)info;
    fprintf(stderr, "[dreamcast] disk capacity query is unsupported\n");
    errno = ENOTSUP;
    return -1;
}

struct gex_directory { file_t file; gex_directory_entry entry; };
gex_directory *gex_directory_open(const char *path) {
    file_t file = fs_open(path, O_RDONLY | O_DIR);
    if (file < 0) return NULL;
    gex_directory *directory = malloc(sizeof *directory);
    if (!directory) { fs_close(file); hle_fatal("no memory for directory iterator"); }
    directory->file = file;
    return directory;
}
const gex_directory_entry *gex_directory_next(gex_directory *directory) {
    errno = 0;
    const dirent_t *entry = fs_readdir(directory->file);
    if (!entry) {
        if (errno) fprintf(stderr, "[dreamcast] readdir failed: %s\n", strerror(errno));
        return NULL;
    }
    if (strlen(entry->name) >= sizeof directory->entry.name)
        hle_fatal("directory entry exceeds portable path limit");
    strcpy(directory->entry.name, entry->name);
    directory->entry.directory = entry->size < 0;
    return &directory->entry;
}
void gex_directory_close(gex_directory *directory) {
    if (fs_close(directory->file)) fprintf(stderr, "[dreamcast] directory close failed\n");
    free(directory);
}
FILE *gex_fs_replace_begin(const char *path, char **temporary) {
    (void)path;
    *temporary = NULL;
    fprintf(stderr, "[dreamcast] atomic file replacement is unavailable on the RAM disk\n");
    errno = ENOTSUP;
    return NULL;
}
int gex_fs_replace_commit(FILE *file, const char *temporary, const char *path) {
    (void)path;
    if (file) fclose(file);
    if (temporary) unlink(temporary);
    fprintf(stderr, "[dreamcast] atomic file replacement is unavailable on the RAM disk\n");
    errno = ENOTSUP;
    return -1;
}
