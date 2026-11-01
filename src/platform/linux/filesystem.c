#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "platform/filesystem.h"
#include "win32hle.h"

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
    int native = (flags & GEX_FILE_WRITE) ?
        ((flags & GEX_FILE_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (flags & GEX_FILE_CREATE) native |= O_CREAT;
    if (flags & GEX_FILE_EXCLUSIVE) native |= O_EXCL;
    if (flags & GEX_FILE_TRUNCATE) native |= O_TRUNC;
    return open(path, native | O_CLOEXEC, 0644);
}
int gex_file_close(int file) { return close(file); }
int gex_file_duplicate(int file) { return dup(file); }
int64_t gex_file_read(int file, void *data, size_t bytes) { return read(file, data, bytes); }
int64_t gex_file_write(int file, const void *data, size_t bytes) { return write(file, data, bytes); }
int64_t gex_file_seek(int file, int64_t offset, int origin) { return lseek(file, (off_t)offset, origin); }
int gex_file_truncate(int file, int64_t bytes) { return ftruncate(file, (off_t)bytes); }
int gex_file_is_terminal(int file) { return isatty(file); }

static void translate_info(const struct stat *native, gex_file_info *info) {
    *info = (gex_file_info){
        .size = (uint64_t)native->st_size, .device = native->st_dev,
        .inode = native->st_ino, .links = native->st_nlink,
        .directory = S_ISDIR(native->st_mode), .writable = !!(native->st_mode & S_IWUSR),
#ifdef __APPLE__
        .accessed = {native->st_atimespec.tv_sec, native->st_atimespec.tv_nsec},
        .modified = {native->st_mtimespec.tv_sec, native->st_mtimespec.tv_nsec}
#else
        .accessed = {native->st_atim.tv_sec, native->st_atim.tv_nsec},
        .modified = {native->st_mtim.tv_sec, native->st_mtim.tv_nsec}
#endif
    };
}
int gex_file_stat(int file, gex_file_info *info) {
    struct stat native;
    if (fstat(file, &native)) return -1;
    translate_info(&native, info);
    return 0;
}
int gex_path_stat(const char *path, gex_file_info *info) {
    struct stat native;
    if (stat(path, &native)) return -1;
    translate_info(&native, info);
    return 0;
}
int gex_file_set_times(int file, const gex_file_time times[2]) {
    struct timespec native[2];
    for (unsigned i = 0; i < 2; ++i) {
        native[i].tv_sec = (time_t)times[i].seconds;
        native[i].tv_nsec = times[i].nanoseconds < 0 ? UTIME_OMIT : times[i].nanoseconds;
    }
    return futimens(file, native);
}
int gex_directory_create(const char *path) { return mkdir(path, 0755); }
int gex_directory_remove(const char *path) { return rmdir(path); }
int gex_file_remove(const char *path) { return unlink(path); }
int gex_file_rename(const char *from, const char *to) { return rename(from, to); }
char *gex_fs_current_directory(char *path, size_t size) { return getcwd(path, size); }
int gex_fs_enter_directory(const char *path) { return chdir(path); }
char *gex_fs_canonical_path(const char *path) { return realpath(path, NULL); }
int gex_fs_readable(const char *path) { return access(path, R_OK) == 0; }
const char *gex_fs_device_path(int console) { return console ? "/dev/tty" : "/dev/null"; }
const char *gex_fs_default_drive_root(unsigned drive) {
    return drive == 'z' - 'a' ? "/" : NULL;
}
int gex_fs_disk_info(const char *path, gex_disk_info *info) {
    struct statvfs native;
    if (statvfs(path, &native)) return -1;
    *info = (gex_disk_info){
        native.f_frsize ? native.f_frsize : native.f_bsize,
        native.f_bavail, native.f_blocks, native.f_bfree
    };
    return 0;
}

struct gex_directory { DIR *native; char *path; gex_directory_entry entry; };
gex_directory *gex_directory_open(const char *path) {
    DIR *native = opendir(path);
    if (!native) return NULL;
    gex_directory *directory = calloc(1, sizeof *directory);
    if (!directory) hle_fatal("cannot allocate directory iterator");
    directory->native = native;
    directory->path = strdup(path);
    if (!directory->path) hle_fatal("cannot allocate directory path");
    return directory;
}
const gex_directory_entry *gex_directory_next(gex_directory *directory) {
    errno = 0;
    struct dirent *entry = readdir(directory->native);
    if (!entry) {
        if (errno) fprintf(stderr, "[gex] read directory %s: %s\n", directory->path, strerror(errno));
        return NULL;
    }
    if (strlen(entry->d_name) >= sizeof directory->entry.name)
        hle_fatal("directory entry exceeds portable path limit");
    strcpy(directory->entry.name, entry->d_name);
    directory->entry.directory = entry->d_type == DT_DIR;
    if (entry->d_type == DT_UNKNOWN) {
        char path[2048];
        int length = snprintf(path, sizeof path, "%s/%s", directory->path, entry->d_name);
        gex_file_info info;
        directory->entry.directory = length >= 0 && (size_t)length < sizeof path &&
            !gex_path_stat(path, &info) && info.directory;
    }
    return &directory->entry;
}
void gex_directory_close(gex_directory *directory) {
    if (closedir(directory->native))
        fprintf(stderr, "[gex] close directory %s: %s\n", directory->path, strerror(errno));
    free(directory->path);
    free(directory);
}

FILE *gex_fs_replace_begin(const char *path, char **temporary) {
    size_t size = strlen(path) + sizeof ".XXXXXX";
    *temporary = malloc(size);
    if (!*temporary) hle_fatal("cannot allocate atomic replacement path");
    snprintf(*temporary, size, "%s.XXXXXX", path);
    int descriptor = mkstemp(*temporary);
    FILE *file = descriptor < 0 ? NULL : fdopen(descriptor, "w");
    if (!file) {
        int error = errno;
        if (descriptor >= 0) { close(descriptor); unlink(*temporary); }
        free(*temporary);
        *temporary = NULL;
        errno = error;
    }
    return file;
}
int gex_fs_replace_commit(FILE *file, const char *temporary, const char *path) {
    int error = ferror(file) ? EIO : 0;
    if (fflush(file) && !error) error = errno;
    if (!error && fsync(fileno(file))) error = errno;
    if (fclose(file) && !error) error = errno;
    if (!error && rename(temporary, path)) error = errno;
    if (error) {
        unlink(temporary);
        errno = error;
    }
    return error ? -1 : 0;
}
