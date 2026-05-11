/*
 * Copyright 2025-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <SDL3/SDL.h>
#include <nopegl/nopegl.h>

#include "viewer_export.h"
#include "viewer_scene.h"

const struct export_profile export_profiles[] = {
    {"MP4 / H264 4:2:0",       "mp4", "libx264",    "yuv420p", 18, 1},
    {"MP4 / H264 4:4:4",       "mp4", "libx264",    "yuv444p", 18, 0},
    {"MP4 / AV1 4:2:0",        "mp4", "libsvtav1",  "yuv420p", 18, 1},
    {"MOV / QTRLE (Lossless)", "mov", "qtrle",      NULL,      -1, 0},
    {"NUT / FFV1 (Lossless)",  "nut", "ffv1",       NULL,      -1, 0},
    {"Animated GIF",           "gif", "gif",        NULL,      -1, 0},
};
const int nb_export_profiles = sizeof(export_profiles) / sizeof(export_profiles[0]);

int export_is_profile_available(int index)
{
    if (index < 0 || index >= nb_export_profiles)
        return 0;
    return avcodec_find_encoder_by_name(export_profiles[index].encoder) != NULL;
}

struct export_ctx {
    SDL_Thread *thread;

    char *filename;
    struct scene_cmd_queue *snapshot_queue;
    struct ngl_scene *scene;
    struct export_profile profile;
    uint32_t width;
    uint32_t height;
    int32_t framerate[2];
    double duration;

    /* Cross-thread state. */
    _Atomic enum export_state state;
    _Atomic float progress;
    _Atomic int cancel_requested;
    char error_msg[256];
};

/*
 * Transition RUNNING -> target via CAS. First writer wins: a late error
 * can't overwrite an earlier CANCELLED (or vice versa), and the error_msg
 * captured on the winning transition is preserved.
 */
static int export_try_transition(struct export_ctx *s, enum export_state target)
{
    enum export_state expected = EXPORT_RUNNING;
    return atomic_compare_exchange_strong(&s->state, &expected, target);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static void export_set_error(struct export_ctx *s, const char *fmt, ...)
{
    if (atomic_load_explicit(&s->state, memory_order_relaxed) != EXPORT_RUNNING)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->error_msg, sizeof(s->error_msg), fmt, ap);
    va_end(ap);
    if (!export_try_transition(s, EXPORT_ERROR))
        return;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "export error: %s", s->error_msg);
}

static void export_finalize(struct export_ctx *s, enum export_state target)
{
    export_try_transition(s, target);
}

static int export_thread(void *arg)
{
    struct export_ctx *s = arg;
    const struct export_profile *prof = &s->profile;

    AVFormatContext *ofmt_ctx = NULL;
    AVCodecContext *enc_ctx = NULL;
    struct SwsContext *sws_ctx = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    struct ngl_ctx *ngl = NULL;
    uint8_t *capture_buffer = NULL;
    int ret;

    SDL_Semaphore *snapshot_sem = SDL_CreateSemaphore(0);
    if (!snapshot_sem) {
        export_set_error(s, "could not create snapshot semaphore");
        goto end;
    }
    scene_cmd_post(s->snapshot_queue, (struct scene_cmd){
        .type     = SCENE_CMD_SNAPSHOT,
        .snapshot = {.out = &s->scene, .done = snapshot_sem},
    });
    SDL_WaitSemaphore(snapshot_sem);
    SDL_DestroySemaphore(snapshot_sem);

    /* Check for early cancellation. */
    if (atomic_load(&s->cancel_requested)) {
        export_finalize(s, EXPORT_CANCELLED);
        goto end;
    }
    if (!s->scene) {
        export_set_error(s, "no scene to export");
        goto end;
    }

    /* Mask height to even before deriving width, so width is computed from
     * the final encode height — otherwise an odd input height would skew
     * the output aspect. */
    s->height &= ~1U;
    const struct ngl_scene_params *sp = ngl_scene_get_params(s->scene);
    if (sp && sp->width > 0 && sp->height > 0)
        s->width = (uint32_t)((uint64_t)s->height * (uint32_t)sp->width / (uint32_t)sp->height);
    else
        s->width = s->height * 16 / 9;
    s->width &= ~1U;

    /* Find encoder. */
    const AVCodec *codec = avcodec_find_encoder_by_name(prof->encoder);
    if (!codec) {
        export_set_error(s, "encoder '%s' not found", prof->encoder);
        goto end;
    }

    /* Output format context. */
    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, prof->format, s->filename);
    if (ret < 0) {
        export_set_error(s, "could not create output context");
        goto end;
    }

    /* Encoder context. */
    enc_ctx = avcodec_alloc_context3(codec);
    if (!enc_ctx) {
        export_set_error(s, "could not allocate encoder context");
        goto end;
    }

    enc_ctx->width = (int)s->width;
    enc_ctx->height = (int)s->height;
    enc_ctx->time_base = (AVRational){s->framerate[1], s->framerate[0]};
    enc_ctx->framerate = (AVRational){s->framerate[0], s->framerate[1]};

    if (prof->pix_fmt) {
        enc_ctx->pix_fmt = av_get_pix_fmt(prof->pix_fmt);
    } else {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
        const enum AVPixelFormat *pix_fmts = NULL;
        int nb_pix_fmts = 0;
        const int cfg_ret = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_PIX_FORMAT,
                                                         0, (const void **)&pix_fmts, &nb_pix_fmts);
        if (cfg_ret >= 0 && pix_fmts && nb_pix_fmts > 0)
            enc_ctx->pix_fmt = pix_fmts[0];
        else
            enc_ctx->pix_fmt = AV_PIX_FMT_RGBA;
#else
        if (codec->pix_fmts && codec->pix_fmts[0] != AV_PIX_FMT_NONE)
            enc_ctx->pix_fmt = codec->pix_fmts[0];
        else
            enc_ctx->pix_fmt = AV_PIX_FMT_RGBA;
#endif
    }

    if (prof->crf >= 0)
        av_opt_set_int(enc_ctx->priv_data, "crf", prof->crf, 0);

    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(enc_ctx, codec, NULL);
    if (ret < 0) {
        export_set_error(s, "could not open encoder '%s'", prof->encoder);
        goto end;
    }

    AVStream *stream = avformat_new_stream(ofmt_ctx, NULL);
    if (!stream) {
        export_set_error(s, "could not create output stream");
        goto end;
    }
    ret = avcodec_parameters_from_context(stream->codecpar, enc_ctx);
    if (ret < 0) {
        export_set_error(s, "could not copy encoder parameters to stream");
        goto end;
    }
    stream->time_base = enc_ctx->time_base;

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, s->filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            export_set_error(s, "could not open '%s'", s->filename);
            goto end;
        }
    }

    AVDictionary *opts = NULL;
    if (prof->faststart)
        av_dict_set(&opts, "movflags", "+faststart", 0);
    ret = avformat_write_header(ofmt_ctx, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        export_set_error(s, "could not write header");
        goto end;
    }

    sws_ctx = sws_getContext((int)s->width, (int)s->height, AV_PIX_FMT_RGBA,
                             (int)s->width, (int)s->height, enc_ctx->pix_fmt,
                             SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        export_set_error(s, "could not create color converter");
        goto end;
    }

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt) {
        export_set_error(s, "could not allocate frame/packet");
        goto end;
    }
    frame->format = enc_ctx->pix_fmt;
    frame->width = enc_ctx->width;
    frame->height = enc_ctx->height;
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        export_set_error(s, "could not allocate frame buffer");
        goto end;
    }

    capture_buffer = SDL_calloc(s->width * s->height, 4);
    if (!capture_buffer) {
        export_set_error(s, "could not allocate capture buffer");
        goto end;
    }

    ngl = ngl_create();
    if (!ngl) {
        export_set_error(s, "could not create ngl context");
        goto end;
    }

    struct ngl_config cfg = {
        .offscreen      = 1,
        .width          = s->width,
        .height         = s->height,
        .capture_buffer = capture_buffer,
        .clear_color    = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    ret = ngl_configure(ngl, &cfg);
    if (ret < 0) {
        export_set_error(s, "could not configure ngl context (ret=%d)", ret);
        goto end;
    }

    ret = ngl_set_scene(ngl, s->scene);
    if (ret < 0) {
        export_set_error(s, "could not set scene (ret=%d)", ret);
        goto end;
    }

    const int64_t nb_frames = (int64_t)(s->duration * s->framerate[0] / s->framerate[1]);
    for (int64_t i = 0; i < nb_frames; i++) {
        if (atomic_load(&s->cancel_requested)) {
            export_finalize(s, EXPORT_CANCELLED);
            goto end;
        }

        const double t = (double)i * s->framerate[1] / s->framerate[0];
        ret = ngl_draw(ngl, t, NULL);
        if (ret < 0) {
            export_set_error(s, "ngl_draw failed at t=%.3f (ret=%d)", t, ret);
            goto end;
        }

        const uint8_t *src_data[1] = {capture_buffer};
        const int src_linesize[1] = {(int)(s->width * 4)};
        ret = av_frame_make_writable(frame);
        if (ret < 0) {
            export_set_error(s, "av_frame_make_writable failed at frame %lld (ret=%d)",
                             (long long)i, ret);
            goto end;
        }
        sws_scale(sws_ctx, src_data, src_linesize, 0, (int)s->height,
                  frame->data, frame->linesize);
        frame->pts = i;

        ret = avcodec_send_frame(enc_ctx, frame);
        if (ret < 0) {
            export_set_error(s, "encoding error at frame %lld", (long long)i);
            goto end;
        }
        while (ret >= 0) {
            ret = avcodec_receive_packet(enc_ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                export_set_error(s, "encoding error at frame %lld", (long long)i);
                goto end;
            }
            av_packet_rescale_ts(pkt, enc_ctx->time_base, stream->time_base);
            pkt->stream_index = stream->index;
            ret = av_interleaved_write_frame(ofmt_ctx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                export_set_error(s, "write error at frame %lld (ret=%d)",
                                 (long long)i, ret);
                goto end;
            }
        }

        atomic_store(&s->progress, (float)(i + 1) / (float)nb_frames);
    }

    ret = avcodec_send_frame(enc_ctx, NULL);
    if (ret < 0) {
        export_set_error(s, "could not flush encoder (ret=%d)", ret);
        goto end;
    }
    while (1) {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            export_set_error(s, "drain error (ret=%d)", ret);
            goto end;
        }
        av_packet_rescale_ts(pkt, enc_ctx->time_base, stream->time_base);
        pkt->stream_index = stream->index;
        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            export_set_error(s, "write error during drain (ret=%d)", ret);
            goto end;
        }
    }

    ret = av_write_trailer(ofmt_ctx);
    if (ret < 0) {
        export_set_error(s, "could not write trailer (ret=%d)", ret);
        goto end;
    }
    atomic_store(&s->progress, 1.0f);
    export_finalize(s, EXPORT_DONE);

end:
    if (ngl)
        ngl_set_scene(ngl, NULL);
    ngl_freep(&ngl);
    SDL_free(capture_buffer);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&enc_ctx);
    if (ofmt_ctx) {
        if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);
    }
    return 0;
}

struct export_ctx *export_create(void)
{
    return SDL_calloc(1, sizeof(struct export_ctx));
}

int export_start(struct export_ctx *s, const struct export_params *params)
{
    if (s->thread)
        return -1;

    if (params->framerate[0] <= 0 || params->framerate[1] <= 0 || params->height == 0)
        return -1;

    s->filename = SDL_strdup(params->filename);
    if (!s->filename)
        return -1;

    s->snapshot_queue = params->snapshot_queue;
    s->profile  = export_profiles[params->profile_index];
    s->width    = 0; /* Filled by export_thread once the snapshot arrives. */
    s->height   = params->height;
    memcpy(s->framerate, params->framerate, sizeof(s->framerate));
    s->duration = params->duration;
    atomic_store(&s->state, EXPORT_RUNNING);
    atomic_store(&s->progress, 0.0f);
    atomic_store(&s->cancel_requested, 0);
    s->error_msg[0] = 0;

    s->thread = SDL_CreateThread(export_thread, "export", s);
    if (!s->thread) {
        atomic_store(&s->state, EXPORT_ERROR);
        return -1;
    }
    return 0;
}

enum export_state export_get_state(struct export_ctx *s)
{
    return s ? atomic_load(&s->state) : EXPORT_IDLE;
}

float export_get_progress(struct export_ctx *s)
{
    return s ? atomic_load(&s->progress) : 0.0f;
}

const char *export_get_error(struct export_ctx *s)
{
    return (s && s->error_msg[0]) ? s->error_msg : NULL;
}

void export_cancel(struct export_ctx *s)
{
    if (s)
        atomic_store(&s->cancel_requested, 1);
}

void export_freep(struct export_ctx **sp)
{
    struct export_ctx *s = *sp;
    if (!s)
        return;
    if (s->thread) {
        atomic_store(&s->cancel_requested, 1);
        SDL_WaitThread(s->thread, NULL);
    }
    ngl_scene_unrefp(&s->scene);
    SDL_free(s->filename);
    SDL_free(s);
    *sp = NULL;
}
