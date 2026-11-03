#ifndef GEX_FFMPEG_IO_H
#define GEX_FFMPEG_IO_H

#include <stddef.h>

struct AVFormatContext;
typedef struct gex_ffmpeg_io gex_ffmpeg_io;

gex_ffmpeg_io *gex_ffmpeg_io_open(const char *path, struct AVFormatContext *format,
                                 char *error, size_t error_size);
void gex_ffmpeg_io_close(gex_ffmpeg_io *io);

#endif
