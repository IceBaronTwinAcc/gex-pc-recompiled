#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include "platform/ffmpeg_io.h"

#define READ_AHEAD 65536

struct gex_ffmpeg_io {
    int file;
    int64_t position, size, cached_position;
    int cached_bytes;
    uint8_t *cache;
    AVIOContext *context;
};

static int read_movie(void *opaque, uint8_t *buffer, int bytes) {
    gex_ffmpeg_io *io = opaque;
    int done = 0;
    while (done < bytes && io->position < io->size) {
        int64_t offset = io->position - io->cached_position;
        if (offset < 0 || offset >= io->cached_bytes) {
            io->cached_bytes = 0;
            io->cached_position = io->position & ~INT64_C(2047);
            if (lseek(io->file, (off_t)io->cached_position, SEEK_SET) < 0)
                return done ? done : AVERROR(errno);
            /* KOS needs both sector-aligned offsets and 32-byte-aligned buffers
             * for multi-sector DMA. Ordinary FFmpeg reads can satisfy neither. */
            ssize_t count = read(io->file, io->cache, READ_AHEAD);
            if (count < 0) return done ? done : AVERROR(errno);
            if (!count) return done ? done : AVERROR(EIO);
            io->cached_bytes = (int)count;
            offset = io->position - io->cached_position;
            if (offset >= count) return done ? done : AVERROR(EIO);
        }
        int count = io->cached_bytes - (int)offset;
        if (count > bytes - done) count = bytes - done;
        memcpy(buffer + done, io->cache + offset, (size_t)count);
        io->position += count;
        done += count;
    }
    return done ? done : AVERROR_EOF;
}

static int64_t seek_movie(void *opaque, int64_t offset, int origin) {
    gex_ffmpeg_io *io = opaque;
    origin &= ~AVSEEK_FORCE;
    if (origin == AVSEEK_SIZE) return io->size;
    int64_t base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? io->position :
                   origin == SEEK_END ? io->size : -1;
    if (base < 0 || offset < -base || offset > io->size - base) return AVERROR(EINVAL);
    io->position = base + offset;
    return io->position;
}

void gex_ffmpeg_io_close(gex_ffmpeg_io *io) {
    if (!io) return;
    if (io->context) {
        av_freep(&io->context->buffer);
        avio_context_free(&io->context);
    }
    if (io->file >= 0 && close(io->file))
        fprintf(stderr, "[movie] cannot close disc file: %s\n", strerror(errno));
    free(io->cache);
    free(io);
}

gex_ffmpeg_io *gex_ffmpeg_io_open(const char *path, AVFormatContext *format,
                                 char *error, size_t error_size) {
    gex_ffmpeg_io *io = calloc(1, sizeof *io);
    if (!io) {
        snprintf(error, error_size, "cannot allocate movie disc reader");
        return NULL;
    }
    io->file = open(path, O_RDONLY);
    struct stat status;
    if (io->file < 0 || fstat(io->file, &status)) {
        snprintf(error, error_size, "cannot open movie disc file: %s", strerror(errno));
        goto failed;
    }
    if (status.st_size <= 0) {
        snprintf(error, error_size, "movie disc file is empty");
        goto failed;
    }
    io->size = status.st_size;
    io->cache = memalign(32, READ_AHEAD);
    uint8_t *buffer = av_malloc(4096);
    if (!io->cache || !buffer) {
        av_free(buffer);
        snprintf(error, error_size, "cannot allocate movie disc buffers");
        goto failed;
    }
    io->context = avio_alloc_context(buffer, 4096, 0, io, read_movie, NULL, seek_movie);
    if (!io->context) {
        av_free(buffer);
        snprintf(error, error_size, "cannot allocate movie I/O context");
        goto failed;
    }
    format->pb = io->context;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    return io;
failed:
    gex_ffmpeg_io_close(io);
    return NULL;
}
