/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "export/export.h"
#include "internal.h"
#include "nopeaudio/nopeaudio.h"

struct nau_exporter {
    AVFormatContext *format;
    AVCodecContext *encoder;
    AVFrame *frame;
    AVPacket *packet;
    SwrContext *converter;
    int frame_size;
    int buffered;
    int64_t frames;
    int opened;
    int error;
    int empty_packet;
};

static enum AVSampleFormat get_sample_format(enum nau_sample_format format)
{
    switch (format) {
    case NAU_SAMPLE_FORMAT_U8:   return AV_SAMPLE_FMT_U8;
    case NAU_SAMPLE_FORMAT_S16:  return AV_SAMPLE_FMT_S16;
    case NAU_SAMPLE_FORMAT_S32:  return AV_SAMPLE_FMT_S32;
    case NAU_SAMPLE_FORMAT_S64:  return AV_SAMPLE_FMT_S64;
    case NAU_SAMPLE_FORMAT_F32:  return AV_SAMPLE_FMT_FLT;
    case NAU_SAMPLE_FORMAT_F64:  return AV_SAMPLE_FMT_DBL;
    case NAU_SAMPLE_FORMAT_U8P:  return AV_SAMPLE_FMT_U8P;
    case NAU_SAMPLE_FORMAT_S16P: return AV_SAMPLE_FMT_S16P;
    case NAU_SAMPLE_FORMAT_S32P: return AV_SAMPLE_FMT_S32P;
    case NAU_SAMPLE_FORMAT_S64P: return AV_SAMPLE_FMT_S64P;
    case NAU_SAMPLE_FORMAT_F32P: return AV_SAMPLE_FMT_FLTP;
    case NAU_SAMPLE_FORMAT_F64P: return AV_SAMPLE_FMT_DBLP;
    default:                     return AV_SAMPLE_FMT_NONE;
    }
}

static int export_error(int ret)
{
    if (ret >= 0)
        return NAU_OK;
    if (ret == AVERROR(ENOMEM))
        return NAU_ERROR_MEMORY;
    if (ret == AVERROR(EINVAL))
        return NAU_ERROR_INVALID;
    if (ret == AVERROR(ENOSYS) || ret == AVERROR_ENCODER_NOT_FOUND ||
        ret == AVERROR_MUXER_NOT_FOUND || ret == AVERROR_OPTION_NOT_FOUND)
        return NAU_ERROR_UNSUPPORTED;
    return NAU_ERROR_IO;
}

static int reset(struct nau_exporter *e)
{
    int ret = 0;
    if (e->format && e->format->pb)
        ret = avio_closep(&e->format->pb);
    swr_free(&e->converter);
    av_frame_free(&e->frame);
    av_packet_free(&e->packet);
    avcodec_free_context(&e->encoder);
    avformat_free_context(e->format);
    memset(e, 0, sizeof(*e));
    return ret;
}

static int set_options(AVDictionary **dict, const struct nau_exporter_option *options)
{
    for (const struct nau_exporter_option *o = options; o && o->name; o++) {
        int ret = av_dict_set(dict, o->name, o->value, 0);
        if (ret < 0)
            return ret;
    }
    return 0;
}

struct nau_exporter *nau_exporter_create(void)
{
    return av_mallocz(sizeof(struct nau_exporter));
}

int nau_exporter_open(struct nau_exporter *e, const char *filename,
                      const struct nau_exporter_config *config)
{
    if (!e || e->opened)
        return NAU_ERROR_INVALID;

    if (!filename)
        return NAU_ERROR_INVALID;

    if (!config)
        return NAU_ERROR_INVALID;

    if (!config->container || !config->encoder)
        return NAU_ERROR_INVALID;

    if (!config->sample_rate || config->sample_rate > NAU_MAX_SAMPLE_RATE)
        return NAU_ERROR_INVALID;

    if (config->bit_rate < 0)
        return NAU_ERROR_INVALID;

    const enum AVSampleFormat sample_format = get_sample_format(config->sample_format);
    if (sample_format == AV_SAMPLE_FMT_NONE)
        return NAU_ERROR_INVALID;

    const AVCodec *codec = avcodec_find_encoder_by_name(config->encoder);
    if (!codec || codec->type != AVMEDIA_TYPE_AUDIO)
        return NAU_ERROR_UNSUPPORTED;

    AVDictionary *options = NULL;
    /* Keep a local-file URL in the context too: muxers such as MP4 may reopen it. */
    char *url = av_asprintf("file:%s", filename);
    if (!url)
        return NAU_ERROR_MEMORY;
    int ret = avformat_alloc_output_context2(&e->format, NULL, config->container, url);
    av_free(url);
    if (ret < 0)
        goto fail;
    e->encoder = avcodec_alloc_context3(codec);
    e->frame = av_frame_alloc();
    e->packet = av_packet_alloc();
    AVStream *stream = avformat_new_stream(e->format, NULL);
    if (!e->encoder || !e->frame || !e->packet || !stream) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    e->encoder->sample_rate = (int)config->sample_rate;
    e->encoder->sample_fmt = sample_format;
    e->encoder->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    e->encoder->time_base = (AVRational){1, (int)config->sample_rate};
    e->encoder->bit_rate = config->bit_rate;
    if (e->format->oformat->flags & AVFMT_GLOBALHEADER)
        e->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ret = set_options(&options, config->encoder_options);
    if (ret < 0)
        goto fail;
    ret = avcodec_open2(e->encoder, codec, &options);
    if (ret < 0)
        goto fail;
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (e->encoder->sample_rate != (int)config->sample_rate) {
        ret = AVERROR(EINVAL);
        goto fail;
    }
    if (av_channel_layout_compare(&e->encoder->ch_layout, &stereo)) {
        ret = AVERROR(EINVAL);
        goto fail;
    }
    if (av_dict_count(options)) {
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }
    av_dict_free(&options);
    ret = avcodec_parameters_from_context(stream->codecpar, e->encoder);
    if (ret < 0)
        goto fail;
    stream->time_base = e->encoder->time_base;

    e->frame_size = e->encoder->frame_size ? e->encoder->frame_size : 4096;
    e->frame->format = e->encoder->sample_fmt;
    e->frame->sample_rate = e->encoder->sample_rate;
    e->frame->nb_samples = e->frame_size;
    ret = av_channel_layout_copy(&e->frame->ch_layout, &e->encoder->ch_layout);
    if (ret < 0)
        goto fail;
    ret = av_frame_get_buffer(e->frame, 0);
    if (ret < 0)
        goto fail;

    if (av_get_packed_sample_fmt(e->encoder->sample_fmt) != AV_SAMPLE_FMT_S16) {
        ret = swr_alloc_set_opts2(&e->converter, &e->encoder->ch_layout, e->encoder->sample_fmt,
                                  e->encoder->sample_rate, &stereo, AV_SAMPLE_FMT_FLT,
                                  (int)config->sample_rate, 0, NULL);
        if (ret < 0)
            goto fail;

        ret = swr_init(e->converter);
        if (ret < 0)
            goto fail;
    }

    ret = set_options(&options, config->container_options);
    if (ret < 0)
        goto fail;
    ret = avio_open(&e->format->pb, e->format->url, AVIO_FLAG_WRITE);
    if (ret < 0)
        goto fail;
    ret = avformat_write_header(e->format, &options);
    if (ret < 0)
        goto fail;
    if (av_dict_count(options)) {
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }
    av_dict_free(&options);
    e->opened = 1;
    e->empty_packet = config->empty_packet;
    return NAU_OK;

fail:
    av_dict_free(&options);
    reset(e);
    return export_error(ret);
}

static int encode(struct nau_exporter *e, const AVFrame *frame)
{
    int ret = avcodec_send_frame(e->encoder, frame);
    if (ret < 0)
        return ret;
    for (;;) {
        ret = avcodec_receive_packet(e->encoder, e->packet);
        if (ret < 0)
            break;
        AVStream *stream = e->format->streams[0];
        av_packet_rescale_ts(e->packet, e->encoder->time_base, stream->time_base);
        e->packet->stream_index = stream->index;
        ret = av_interleaved_write_frame(e->format, e->packet);
        av_packet_unref(e->packet);
        if (ret < 0)
            return ret;
    }
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

static int encode_buffer(struct nau_exporter *e)
{
    e->frame->nb_samples = e->buffered;
    e->frame->pts = e->frames - e->buffered;
    int ret = encode(e, e->frame);
    e->buffered = 0;
    return ret;
}

static int convert(struct nau_exporter *e, const float *samples, int frames)
{
    const enum AVSampleFormat format = e->encoder->sample_fmt;
    const int planar = av_sample_fmt_is_planar(format);
    const int bytes = av_get_bytes_per_sample(format);
    uint8_t *dst[2] = {NULL, NULL};
    for (int c = 0; c < (planar ? 2 : 1); c++)
        dst[c] = e->frame->data[c] + (size_t)e->buffered * (size_t)bytes * (planar ? 1U : 2U);
    if (e->converter) {
        const uint8_t *src[] = {(const uint8_t *)samples};
        int ret = swr_convert(e->converter, dst, frames, src, frames);
        return ret < 0 ? ret : ret == frames ? 0 : AVERROR_BUG;
    }
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < 2; c++) {
            const float value = samples[2 * (size_t)i + (size_t)c];
            const float x = isfinite(value) ? fmaxf(-1.f, fminf(1.f, value)) : 0.f;
            int16_t *out = (int16_t *)dst[planar ? c : 0];
            out[planar ? i : 2 * i + c] = (int16_t)lrintf(x * 32767.f);
        }
    }
    return 0;
}

int nau_exporter_write(struct nau_exporter *e, const float *samples, uint32_t frames)
{
    if (!e || !e->opened)
        return NAU_ERROR_INVALID;

    if (frames && !samples)
        return NAU_ERROR_INVALID;

    if (frames > INT_MAX)
        return NAU_ERROR_INVALID;

    if ((size_t)frames > SIZE_MAX / sizeof(*samples) / 2)
        return NAU_ERROR_INVALID;

    if (e->frames > INT64_MAX - frames)
        return NAU_ERROR_INVALID;

    if (e->error < 0)
        return export_error(e->error);
    while (frames) {
        int ret;
        if (!e->buffered) {
            e->frame->nb_samples = e->frame_size;
            ret = av_frame_make_writable(e->frame);
            if (ret < 0) {
                e->error = ret;
                break;
            }
        }
        const int count = (int)FFMIN(frames, (uint32_t)(e->frame_size - e->buffered));
        ret = convert(e, samples, count);
        if (ret < 0) {
            e->error = ret;
            break;
        }
        e->buffered += count;
        e->frames += count;
        samples += 2 * (size_t)count;
        frames -= (uint32_t)count;
        if (e->buffered == e->frame_size) {
            ret = encode_buffer(e);
            if (ret < 0) {
                e->error = ret;
                break;
            }
        }
    }
    return export_error(e->error);
}

int nau_exporter_closep(struct nau_exporter **exporterp)
{
    if (!exporterp)
        return NAU_OK;
    struct nau_exporter *e = *exporterp;
    if (!e)
        return NAU_OK;
    int ret = e->error;
    if (e->opened) {
        if (ret >= 0 && e->buffered)
            ret = encode_buffer(e);
        if (ret >= 0)
            ret = encode(e, NULL);
        if (ret >= 0 && !e->frames && e->empty_packet) {
            /* Some containers derive their empty-file sample count from packet timestamps. */
            ret = av_new_packet(e->packet, 0);
            if (ret >= 0) {
                e->packet->pts = e->packet->dts = 0;
                ret = av_interleaved_write_frame(e->format, e->packet);
            }
        }
        int trailer_ret = av_write_trailer(e->format);
        if (ret >= 0)
            ret = trailer_ret;
    }
    int close_ret = reset(e);
    if (ret >= 0)
        ret = close_ret;
    av_freep(exporterp);
    return export_error(ret);
}
