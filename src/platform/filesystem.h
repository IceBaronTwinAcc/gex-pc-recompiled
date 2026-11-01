#ifndef GEX_PLATFORM_FILESYSTEM_H
#define GEX_PLATFORM_FILESYSTEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum gex_io_error {
    GEX_IO_OTHER, GEX_IO_MISSING, GEX_IO_NOT_DIRECTORY, GEX_IO_EXISTS,
    GEX_IO_DENIED, GEX_IO_INTERRUPTED
};
enum gex_file_flags {
    GEX_FILE_READ = 1, GEX_FILE_WRITE = 2, GEX_FILE_CREATE = 4,
    GEX_FILE_EXCLUSIVE = 8, GEX_FILE_TRUNCATE = 16
};
typedef struct { int64_t seconds; long nanoseconds; } gex_file_time;
typedef struct {
    uint64_t size, device, inode, links;
    int directory, writable;
    gex_file_time accessed, modified;
} gex_file_info;
typedef struct {
    uint64_t unit, available, total, free;
} gex_disk_info;
typedef struct gex_directory gex_directory;
typedef struct { char name[1024]; int directory; } gex_directory_entry;

/* File tokens 0/1/2 denote standard streams; other tokens are backend-owned.
 * Failed operations return -1/NULL and retain a thread-local error. */
enum gex_io_error gex_fs_error(void);
const char *gex_fs_error_text(void);
int gex_file_open(const char *path, unsigned flags);
int gex_file_close(int file);
int gex_file_duplicate(int file);
int64_t gex_file_read(int file, void *data, size_t bytes);
int64_t gex_file_write(int file, const void *data, size_t bytes);
int64_t gex_file_seek(int file, int64_t offset, int origin);
int gex_file_truncate(int file, int64_t bytes);
int gex_file_is_terminal(int file);
int gex_file_stat(int file, gex_file_info *info);
int gex_path_stat(const char *path, gex_file_info *info);
int gex_file_set_times(int file, const gex_file_time times[2]);
int gex_directory_create(const char *path);
int gex_directory_remove(const char *path);
int gex_file_remove(const char *path);
int gex_file_rename(const char *from, const char *to);
char *gex_fs_current_directory(char *path, size_t size);
int gex_fs_enter_directory(const char *path);
char *gex_fs_canonical_path(const char *path);
int gex_fs_readable(const char *path);
int gex_fs_disk_info(const char *path, gex_disk_info *info);
gex_directory *gex_directory_open(const char *path);
const gex_directory_entry *gex_directory_next(gex_directory *directory);
void gex_directory_close(gex_directory *directory);
const char *gex_fs_device_path(int console);
/* Default host root for an unmapped guest drive; NULL means unavailable. */
const char *gex_fs_default_drive_root(unsigned drive);
static inline int gex_fs_path_absolute(const char *path) {
    return path[0] == '/' || path[0] == '\\' ||
        (path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
}

/* Same-directory exclusive temporary file. Commit flushes/syncs/closes before
 * atomic replacement, removes the temporary on failure, and consumes the stream.
 * The caller frees the temporary path returned by begin. */
FILE *gex_fs_replace_begin(const char *path, char **temporary);
int gex_fs_replace_commit(FILE *file, const char *temporary, const char *path);

#endif
