#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#ifdef GEX_MOVIE_CUSTOM_PIXELS
#include "platform/ffmpeg_pixels.h"
#else
#include <libswscale/swscale.h>
#endif
#include "movie_decoder.h"
#ifdef GEX_MOVIE_CUSTOM_IO
#include "platform/ffmpeg_io.h"
#endif

struct gex_movie_decoder {
    AVFormatContext *format;
    AVCodecContext *video;
    AVPacket *packet;
    AVFrame *frame;
#ifdef GEX_MOVIE_CUSTOM_PIXELS
    gex_ffmpeg_pixels pixels;
#else
    struct SwsContext *scale;
    AVFrame *converted;
#endif
    int video_index, audio_index, channels;
    int draining, outstanding, audio_offset;
    gex_movie_event event;
    int16_t pcm[GEX_MOVIE_AUDIO_CHUNK_FRAMES * 2];
    char error[256];
#ifdef GEX_MOVIE_CUSTOM_IO
    gex_ffmpeg_io *io;
#endif
};

static int fail(gex_movie_decoder *d, const char *operation, int code) {
    char detail[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, detail, sizeof detail);
    snprintf(d->error, sizeof d->error, "%s: %s (%d)", operation, detail, code);
    return -1;
}

void gex_movie_decoder_close(gex_movie_decoder *d) {
    if (!d) return;
#ifndef GEX_MOVIE_CUSTOM_PIXELS
    sws_freeContext(d->scale);
    av_frame_free(&d->converted);
#endif
    av_packet_free(&d->packet);
    av_frame_free(&d->frame);
    avcodec_free_context(&d->video);
    avformat_close_input(&d->format);
#ifdef GEX_MOVIE_CUSTOM_IO
    gex_ffmpeg_io_close(d->io);
#endif
    free(d);
}

gex_movie_decoder *gex_movie_decoder_open(const char *path, gex_movie_info *info,
                                         char *error, size_t error_size) {
    gex_movie_decoder *d = calloc(1, sizeof *d);
    if (!d) {
        if (error_size) snprintf(error, error_size, "cannot allocate movie decoder");
        return NULL;
    }
#ifdef GEX_MOVIE_CUSTOM_IO
    d->format = avformat_alloc_context();
    if (!d->format) { fail(d, "allocate movie demuxer", AVERROR(ENOMEM)); goto failed; }
    d->io = gex_ffmpeg_io_open(path, d->format, d->error, sizeof d->error);
    if (!d->io) goto failed;
#endif
    int rc = avformat_open_input(&d->format, path, NULL, NULL);
    if (rc < 0) { fail(d, "open movie", rc); goto failed; }
    rc = avformat_find_stream_info(d->format, NULL);
    if (rc < 0) { fail(d, "read stream information", rc); goto failed; }
    d->video_index = av_find_best_stream(d->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (d->video_index < 0) { fail(d, "find video stream", d->video_index); goto failed; }
    AVCodecParameters *par = d->format->streams[d->video_index]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    d->video = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!d->video) { fail(d, "allocate video decoder", AVERROR_DECODER_NOT_FOUND); goto failed; }
    rc = avcodec_parameters_to_context(d->video, par);
    if (rc < 0) { fail(d, "configure video decoder", rc); goto failed; }
    d->video->thread_count = 1;
    rc = avcodec_open2(d->video, codec, NULL);
    if (rc < 0) { fail(d, "open video decoder", rc); goto failed; }
    if (d->video->width <= 0 || d->video->height <= 0) {
        fail(d, "invalid video dimensions", AVERROR_INVALIDDATA); goto failed;
    }
    d->audio_index = av_find_best_stream(d->format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (d->audio_index < 0 && d->audio_index != AVERROR_STREAM_NOT_FOUND) {
        fail(d, "find audio stream", d->audio_index); goto failed;
    }
    int rate = 0;
    if (d->audio_index >= 0) {
        AVCodecParameters *audio = d->format->streams[d->audio_index]->codecpar;
        d->channels = audio->ch_layout.nb_channels;
        rate = audio->sample_rate;
        if (audio->codec_id != AV_CODEC_ID_PCM_U8 ||
            (d->channels != 1 && d->channels != 2) || rate <= 0) {
            fail(d, "unsupported movie audio format", AVERROR_INVALIDDATA); goto failed;
        }
    }
    d->packet = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->packet || !d->frame) { fail(d, "allocate decode buffers", AVERROR(ENOMEM)); goto failed; }
    int64_t duration = d->format->duration;
    if (duration == AV_NOPTS_VALUE || duration < 0 || duration / 1000 > UINT32_MAX) {
        fail(d, "invalid movie duration", AVERROR_INVALIDDATA); goto failed;
    }
    *info = (gex_movie_info){d->video->width, d->video->height, rate, d->channels,
                            (uint32_t)(duration / 1000)};
    return d;
failed:
    if (error_size) snprintf(error, error_size, "%s", d->error);
    gex_movie_decoder_close(d);
    return NULL;
}

const char *gex_movie_decoder_error(const gex_movie_decoder *d) {
    return d->error;
}

void gex_movie_decoder_release(gex_movie_decoder *d) {
    if (!d->outstanding) return;
    if (d->event.type == GEX_MOVIE_VIDEO) av_frame_unref(d->frame);
    else {
        d->audio_offset += d->event.audio_frames;
        if (d->audio_offset == d->packet->size / d->channels) {
            av_packet_unref(d->packet);
            d->audio_offset = 0;
        }
    }
    d->outstanding = 0;
}

void gex_movie_decoder_discard(gex_movie_decoder *d) {
    d->outstanding = d->audio_offset = 0;
    av_packet_unref(d->packet);
    av_frame_unref(d->frame);
}

int gex_movie_decoder_rewind(gex_movie_decoder *d) {
    gex_movie_decoder_discard(d);
    int rc = av_seek_frame(d->format, -1, 0, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) return fail(d, "rewind movie", rc);
    avcodec_flush_buffers(d->video);
    d->draining = 0;
    return 0;
}

int gex_movie_decoder_next(gex_movie_decoder *d, gex_movie_event *event) {
    if (d->outstanding) return fail(d, "release the previous movie event first", AVERROR(EINVAL));
    for (;;) {
        if (d->packet->size && d->packet->stream_index == d->audio_index) {
            int count = d->packet->size / d->channels - d->audio_offset;
            if (count > GEX_MOVIE_AUDIO_CHUNK_FRAMES) count = GEX_MOVIE_AUDIO_CHUNK_FRAMES;
            for (int i = 0; i < count * d->channels; ++i)
                d->pcm[i] = ((int)d->packet->data[d->audio_offset * d->channels + i] - 128) * 256;
            d->event = (gex_movie_event){.type = GEX_MOVIE_AUDIO, .samples = d->pcm,
                                       .audio_frames = count};
            break;
        }
        int rc = avcodec_receive_frame(d->video, d->frame);
        if (!rc) {
            int64_t pts = d->frame->best_effort_timestamp;
            d->event = (gex_movie_event){.type = GEX_MOVIE_VIDEO, .timestamp_ms =
                pts == AV_NOPTS_VALUE ? 0 : av_rescale_q(pts,
                    d->format->streams[d->video_index]->time_base, (AVRational){1, 1000})};
            break;
        }
        if (rc == AVERROR_EOF) return 0;
        if (rc != AVERROR(EAGAIN) || d->draining) return fail(d, "decode movie frame", rc);
        rc = av_read_frame(d->format, d->packet);
        if (rc == AVERROR_EOF) {
            d->draining = 1;
            rc = avcodec_send_packet(d->video, NULL);
            if (rc < 0) return fail(d, "drain movie decoder", rc);
            continue;
        }
        if (rc < 0) return fail(d, "read movie packet", rc);
        if (d->packet->stream_index == d->audio_index) {
            if (d->packet->size % d->channels)
                return fail(d, "incomplete movie audio sample", AVERROR_INVALIDDATA);
            if (d->packet->size) continue;
        } else if (d->packet->stream_index == d->video_index) {
            rc = avcodec_send_packet(d->video, d->packet);
            if (rc < 0) return fail(d, "submit movie packet", rc);
        }
        av_packet_unref(d->packet);
    }
    d->outstanding = 1;
    *event = d->event;
    return 1;
}

int gex_movie_decoder_render(gex_movie_decoder *d, uint8_t *pixels,
                             int width, int height, int pitch, gex_movie_pixel_format format) {
    int bytes = format == GEX_MOVIE_RGB565_LE ? 2 : format == GEX_MOVIE_BGRA32 ? 4 : 0;
    if (!d->outstanding || d->event.type != GEX_MOVIE_VIDEO || !pixels || !bytes ||
        width <= 0 || width > INT_MAX / bytes || height <= 0 || pitch < width * bytes)
        return fail(d, "invalid movie render target or event", AVERROR(EINVAL));
#ifdef GEX_MOVIE_CUSTOM_PIXELS
    int rc = gex_ffmpeg_convert(&d->pixels, d->frame, pixels, width, height, pitch, format);
    if (rc) return fail(d, "convert native Indeo pixels", rc);
#else
    enum AVPixelFormat pixel_format = format == GEX_MOVIE_RGB565_LE ? AV_PIX_FMT_RGB565LE : AV_PIX_FMT_BGRA;
    if (!d->converted) d->converted = av_frame_alloc();
    if (!d->converted) return fail(d, "allocate movie conversion frame", AVERROR(ENOMEM));
    if (!d->converted->data[0] || d->converted->width != width || d->converted->height != height ||
        d->converted->format != pixel_format) {
        av_frame_unref(d->converted);
        d->converted->width = width;
        d->converted->height = height;
        d->converted->format = pixel_format;
        int rc = av_frame_get_buffer(d->converted, 32);
        if (rc < 0) return fail(d, "allocate padded movie pixels", rc);
    }
    d->scale = sws_getCachedContext(d->scale, d->frame->width, d->frame->height,
        d->frame->format, width, height, pixel_format, SWS_BILINEAR, NULL, NULL, NULL);
    if (!d->scale) return fail(d, "allocate movie pixel converter", AVERROR(ENOMEM));
    int rc = sws_scale(d->scale, (const uint8_t *const *)d->frame->data,
                       d->frame->linesize, 0, d->frame->height,
                       d->converted->data, d->converted->linesize);
    if (rc != height) return fail(d, "convert movie pixels", rc < 0 ? rc : AVERROR_INVALIDDATA);
    /* SIMD conversion may write row padding; the caller owns only the exact picture rectangle. */
    for (int y = 0; y < height; ++y)
        memcpy(pixels + (size_t)y * pitch,
               d->converted->data[0] + (size_t)y * d->converted->linesize[0],
               (size_t)width * bytes);
#endif
    return 0;
}
