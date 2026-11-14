#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "movie_decoder.h"

#define READ_AHEAD 65536

typedef struct {
    int fd, error;
    int64_t position, size, cached_position;
    int cached_bytes;
    uint8_t *cache;
} movie_file;

static movie_file *movie_file_open(const char *path);
static int movie_file_close(movie_file *file);
static int movie_file_seek(movie_file *file, int64_t offset, int origin);
int gex_dreamcast_mpeg_file_read(movie_file *file, void *buffer, size_t bytes);
static int64_t movie_file_tell(movie_file *file);

#define PLM_FILE_TYPE movie_file *
#define PLM_FILE_INVALID_HANDLE NULL
#define PLM_FILE_OPEN(path) movie_file_open(path)
#define PLM_FILE_CLOSE(file) movie_file_close(file)
#define PLM_FILE_SEEK(file, offset, origin) movie_file_seek(file, offset, origin)
#define PLM_FILE_READ(file, buffer, bytes) gex_dreamcast_mpeg_file_read(file, buffer, bytes)
#define PLM_FILE_TELL(file) movie_file_tell(file)
#define PLM_MALLOC(size) malloc(size)
#define PLM_FREE(ptr) free(ptr)
#define PLM_REALLOC(ptr, size) realloc(ptr, size)
#define PLM_MEMZERO(ptr, size) memset(ptr, 0, size)
#define PLM_MEMALIGN(alignment, size) memalign(alignment, size)
#define PL_MPEG_IMPLEMENTATION
#include <pl_mpeg.h>

struct gex_movie_decoder {
    movie_file *file;
    plm_t *mpeg;
    plm_frame_t *frame;
    gex_movie_event event;
    double video_time, audio_time, frame_time;
    int video_end, audio_end, outstanding, audio_rate;
    uint8_t luma[1024];
    uint16_t rgb[3][1024];
    int16_t chroma[4][256];
    char error[256];
};

static void initialize_pixels(gex_movie_decoder *d) {
    const int64_t gain = 65536 * 255 / 219;
    const int coefficients[] = {104597, 132201, -25675, -53279};
    for (int i = 0; i < 1024; ++i) {
        int value = (int)((i * gain - (400LL << 16) + 32768) >> 16);
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        d->luma[i] = (uint8_t)value;
        d->rgb[0][i] = (uint16_t)((value >> 3) << 11);
        d->rgb[1][i] = (uint16_t)((value >> 2) << 5);
        d->rgb[2][i] = (uint16_t)(value >> 3);
    }
    for (int c = 0; c < 4; ++c) {
        int64_t coefficient = ((int64_t)coefficients[c] * 65536 + 32768) / gain;
        for (int i = 0; i < 256; ++i)
            d->chroma[c][i] = (int16_t)((i * coefficient >> 16) - (coefficient >> 9));
    }
}

static movie_file *movie_file_open(const char *path) {
    movie_file *file = calloc(1, sizeof *file);
    if (!file) return NULL;
    file->fd = open(path, O_RDONLY);
    struct stat status;
    if (file->fd < 0 || fstat(file->fd, &status) || status.st_size <= 0 ||
        !(file->cache = memalign(32, READ_AHEAD))) {
        int saved = errno;
        if (file->fd >= 0) close(file->fd);
        free(file);
        errno = saved ? saved : EINVAL;
        return NULL;
    }
    file->size = status.st_size;
    return file;
}

static int movie_file_close(movie_file *file) {
    int result = close(file->fd);
    free(file->cache);
    free(file);
    return result;
}

static int movie_file_seek(movie_file *file, int64_t offset, int origin) {
    int64_t base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? file->position :
                   origin == SEEK_END ? file->size : -1;
    if (base < 0 || offset < -base || offset > file->size - base) {
        file->error = EINVAL;
        return -1;
    }
    file->position = base + offset;
    return 0;
}

static int64_t movie_file_tell(movie_file *file) { return file->position; }

int gex_dreamcast_mpeg_file_read(movie_file *file, void *buffer, size_t bytes) {
    size_t done = 0;
    while (done < bytes && file->position < file->size) {
        int64_t offset = file->position - file->cached_position;
        if (offset < 0 || offset >= file->cached_bytes) {
            file->cached_position = file->position & ~INT64_C(2047);
            file->cached_bytes = 0;
            if (lseek(file->fd, (off_t)file->cached_position, SEEK_SET) < 0) {
                file->error = errno;
                return done ? (int)done : -1;
            }
            ssize_t count = read(file->fd, file->cache, READ_AHEAD);
            if (count <= 0) {
                file->error = count < 0 ? errno : EIO;
                return done ? (int)done : -1;
            }
            file->cached_bytes = (int)count;
            offset = file->position - file->cached_position;
            if (offset >= count) { file->error = EIO; return done ? (int)done : -1; }
        }
        size_t count = (size_t)(file->cached_bytes - offset);
        if (count > bytes - done) count = bytes - done;
        memcpy((uint8_t *)buffer + done, file->cache + offset, count);
        file->position += (int64_t)count;
        done += count;
    }
    return (int)done;
}

static int fail(gex_movie_decoder *d, const char *message) {
    snprintf(d->error, sizeof d->error, "%s", message);
    return -1;
}

void gex_movie_decoder_close(gex_movie_decoder *d) {
    if (!d) return;
    if (d->mpeg) plm_destroy(d->mpeg);
    if (d->file && movie_file_close(d->file))
        fprintf(stderr, "[movie] cannot close disc file: %s\n", strerror(errno));
    free(d);
}

gex_movie_decoder *gex_movie_decoder_open(const char *path, gex_movie_info *info,
                                          char *error, size_t error_size) {
    gex_movie_decoder *d = calloc(1, sizeof *d);
    if (!d) {
        if (error_size) snprintf(error, error_size, "cannot allocate movie decoder");
        return NULL;
    }
    d->file = movie_file_open(path);
    if (!d->file) {
        snprintf(d->error, sizeof d->error, "cannot open MPEG movie: %s", strerror(errno));
        goto failed;
    }
    d->mpeg = plm_create_with_file(d->file, 0);
    if (!d->mpeg || !plm_has_headers(d->mpeg)) {
        fail(d, "invalid MPEG movie headers");
        goto failed;
    }
    int width = plm_get_width(d->mpeg), height = plm_get_height(d->mpeg);
    int rate = plm_get_samplerate(d->mpeg);
    double fps = plm_get_framerate(d->mpeg), duration = plm_get_duration(d->mpeg);
    if (d->file->error || width != 320 || height != 240 ||
        plm_get_num_video_streams(d->mpeg) != 1 || plm_get_num_audio_streams(d->mpeg) != 1 ||
        rate != 32000 || fps != 15.0 || duration <= 0 || duration > UINT32_MAX / 1000.0) {
        fail(d, "unsupported MPEG movie format");
        goto failed;
    }
    d->frame_time = 1.0 / fps;
    d->audio_rate = rate;
    initialize_pixels(d);
    *info = (gex_movie_info){width, height, rate, 2, (uint32_t)(duration * 1000)};
    if (gex_movie_decoder_rewind(d)) goto failed;
    return d;
failed:
    if (error_size) snprintf(error, error_size, "%s", d->error);
    gex_movie_decoder_close(d);
    return NULL;
}

const char *gex_movie_decoder_error(const gex_movie_decoder *d) { return d->error; }

void gex_movie_decoder_release(gex_movie_decoder *d) {
    d->outstanding = 0;
    d->frame = NULL;
}

void gex_movie_decoder_discard(gex_movie_decoder *d) { gex_movie_decoder_release(d); }

int gex_movie_decoder_rewind(gex_movie_decoder *d) {
    gex_movie_decoder_discard(d);
    plm_destroy(d->mpeg);
    d->mpeg = NULL;
    if (movie_file_seek(d->file, 0, SEEK_SET) || d->file->error)
        return fail(d, "cannot rewind MPEG movie");
    d->mpeg = plm_create_with_file(d->file, 0);
    if (!d->mpeg || !plm_has_headers(d->mpeg))
        return fail(d, "cannot reopen MPEG movie");
    d->video_time = d->audio_time = 0;
    d->video_end = d->audio_end = 0;
    return 0;
}

int gex_movie_decoder_next(gex_movie_decoder *d, gex_movie_event *event) {
    if (d->outstanding) return fail(d, "release the previous movie event first");
    while (!d->video_end || !d->audio_end) {
        if (!d->audio_end && (d->video_end || d->audio_time < d->video_time + 0.25)) {
            plm_samples_t *samples = plm_decode_audio(d->mpeg);
            if (d->file->error) return fail(d, "cannot read MPEG movie audio");
            if (!samples) { d->audio_end = 1; continue; }
            d->audio_time += (double)samples->count / d->audio_rate;
            d->event = (gex_movie_event){.type = GEX_MOVIE_AUDIO, .samples = samples->pcm,
                                         .audio_frames = (int)samples->count};
        } else {
            d->frame = plm_decode_video(d->mpeg);
            if (d->file->error) return fail(d, "cannot read MPEG movie video");
            if (!d->frame) { d->video_end = 1; continue; }
            d->event = (gex_movie_event){.type = GEX_MOVIE_VIDEO,
                                         .timestamp_ms = (int64_t)(d->video_time * 1000)};
            d->video_time += d->frame_time;
        }
        d->outstanding = 1;
        *event = d->event;
        return 1;
    }
    return 0;
}

int gex_movie_decoder_render(gex_movie_decoder *d, uint8_t *pixels,
                             int width, int height, int pitch, gex_movie_pixel_format format) {
    int bytes = format == GEX_MOVIE_RGB565_LE ? 2 : format == GEX_MOVIE_BGRA32 ? 4 : 0;
    if (!d->outstanding || d->event.type != GEX_MOVIE_VIDEO || !pixels || !bytes ||
        width <= 0 || height <= 0 || width > INT_MAX / bytes || pitch < width * bytes)
        return fail(d, "invalid movie render target or event");
    plm_frame_t *frame = d->frame;
    if (width == (int)frame->width && height == (int)frame->height &&
        format == GEX_MOVIE_RGB565_LE && !((uintptr_t)pixels & 1) && !(pitch & 1)) {
        for (int y = 0; y < height; ++y) {
            const uint8_t *luma = frame->y.data + (size_t)y * frame->y.width;
            const uint8_t *cb = frame->cb.data + (size_t)(y / 2) * frame->cb.width;
            const uint8_t *cr = frame->cr.data + (size_t)(y / 2) * frame->cr.width;
            uint16_t *out = (uint16_t *)(pixels + (size_t)y * pitch);
            for (int x = 0; x < width; x += 2) {
                int u = cb[x / 2], v = cr[x / 2];
                const uint16_t *red = d->rgb[0] + 326 + d->chroma[0][v];
                const uint16_t *green = d->rgb[1] + 326 + d->chroma[2][u] + d->chroma[3][v];
                const uint16_t *blue = d->rgb[2] + 326 + d->chroma[1][u];
                int a = luma[x], b = luma[x + 1];
                out[x] = red[a] | green[a] | blue[a];
                out[x + 1] = red[b] | green[b] | blue[b];
            }
        }
        return 0;
    }
    unsigned sy = 0, y_error = 0;
    unsigned y_step = frame->height / (unsigned)height;
    unsigned y_remainder = frame->height % (unsigned)height;
    unsigned x_step = frame->width / (unsigned)width;
    unsigned x_remainder = frame->width % (unsigned)width;
    for (int y = 0; y < height; ++y) {
        uint8_t *out = pixels + (size_t)y * pitch;
        unsigned sx = 0, x_error = 0;
        for (int x = 0; x < width; ++x) {
            int luma = frame->y.data[sy * frame->y.width + sx];
            int cb = frame->cb.data[(sy / 2) * frame->cb.width + sx / 2];
            int cr = frame->cr.data[(sy / 2) * frame->cr.width + sx / 2];
            int r = d->luma[326 + d->chroma[0][cr] + luma];
            int g = d->luma[326 + d->chroma[2][cb] + d->chroma[3][cr] + luma];
            int b = d->luma[326 + d->chroma[1][cb] + luma];
            if (bytes == 2) {
                uint16_t value = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
                *out++ = (uint8_t)value;
                *out++ = (uint8_t)(value >> 8);
            } else {
                *out++ = (uint8_t)b; *out++ = (uint8_t)g;
                *out++ = (uint8_t)r; *out++ = 255;
            }
            sx += x_step;
            x_error += x_remainder;
            if (x_error >= (unsigned)width) { x_error -= (unsigned)width; ++sx; }
        }
        sy += y_step;
        y_error += y_remainder;
        if (y_error >= (unsigned)height) { y_error -= (unsigned)height; ++sy; }
    }
    return 0;
}
