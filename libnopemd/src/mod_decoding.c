/*
 * This file is part of nope.media.
 *
 * Copyright (c) 2015 Stupeflix
 *
 * nope.media is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * nope.media is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with nope.media; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>

#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#include "mod_decoding.h"
#include "decoders.h"
#include "internal.h"
#include "loop.h"
#include "msg.h"
#include "log.h"

extern const struct decoder nmdi_decoder_ffmpeg_sw;
static const struct decoder *decoder_def_software = &nmdi_decoder_ffmpeg_sw;

#if HAVE_MEDIACODEC_HWACCEL
extern const struct decoder nmdi_decoder_mediacodec;
extern const struct decoder nmdi_decoder_mediacodec_sw;
static const struct decoder *decoder_def_hwaccel = &nmdi_decoder_mediacodec;
/* Our FFmpeg build relies on Android for codecs such as H.264 and AAC */
static const struct decoder *decoder_def_fallback = &nmdi_decoder_mediacodec_sw;
#else
extern const struct decoder nmdi_decoder_ffmpeg_hw;
static const struct decoder *decoder_def_hwaccel = &nmdi_decoder_ffmpeg_hw;
static const struct decoder *decoder_def_fallback = NULL;
#endif

struct decoding_ctx {
    void *log_ctx;

    AVThreadMessageQueue *pkt_queue;
    AVThreadMessageQueue *frames_queue;

    int is_image;
    int frame_count;

    struct decoder_ctx *decoder;

    AVRational st_timebase;
    AVFrame *tmp_frame;
    int64_t seek_request;

    struct nmdi_pass_timeline timeline;
    struct nmdi_loop *loop;
    struct nmdi_pass_msg *passes;   // retained pass descriptions, ordered by pass number
    size_t nb_passes;               // number of retained descriptions in `passes`
    size_t passes_size;             // allocated capacity of `passes`, in descriptions
    int looping;                    // a pass description was received since startup or the last seek
    int64_t pass;                   // current pass number, advanced when one of its frames is decoded
};

struct decoding_ctx *nmdi_decoding_alloc(void)
{
    struct decoding_ctx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->decoder = nmdi_decoder_alloc();
    if (!ctx->decoder) {
        av_freep(&ctx);
        return NULL;
    }
    return ctx;
}

const AVCodecContext *nmdi_decoding_get_avctx(struct decoding_ctx *ctx)
{
    return ctx->decoder->avctx;
}

int nmdi_decoding_init(void *log_ctx,
                       struct decoding_ctx *ctx,
                       AVThreadMessageQueue *pkt_queue,
                       AVThreadMessageQueue *frames_queue,
                       int is_image,
                       const AVStream *stream,
                       const struct nmdi_pass_timeline *timeline,
                       struct nmdi_loop *loop,
                       const struct nmdi_opts *opts)
{
    int ret;

    AVCodecParameters *par = avcodec_parameters_alloc();
    if (!par)
        return AVERROR(ENOMEM);

    ctx->log_ctx = log_ctx;
    ctx->pkt_queue = pkt_queue;
    ctx->frames_queue = frames_queue;
    ctx->is_image = is_image;
    ctx->timeline = *timeline;
    ctx->loop = loop;

    const struct decoder *decs[3];
    int nb_decs = 0;
    const enum AVMediaType type = stream->codecpar->codec_type;
    if (type == AVMEDIA_TYPE_VIDEO && opts->auto_hwaccel)
        decs[nb_decs++] = decoder_def_hwaccel;
    decs[nb_decs++] = decoder_def_software;
    if (decoder_def_fallback)
        decs[nb_decs++] = decoder_def_fallback;

    ctx->st_timebase = stream->time_base;

#define DUMP_INFO(par, name) do {                                       \
    if ((par)->codec_type == AVMEDIA_TYPE_AUDIO) {                      \
        char chl[1024];                                                 \
        av_channel_layout_describe(&(par)->ch_layout, chl, sizeof(chl)); \
        TRACE(ctx, name " stream: %s %s @ %dHz tb=%d/%d",               \
              chl, av_get_sample_fmt_name((par)->format),               \
              (par)->sample_rate,                                       \
              ctx->st_timebase.num, ctx->st_timebase.den);              \
    } else {                                                            \
        TRACE(ctx, name " stream: %dx%d in %s tb=%d/%d",                \
              (par)->width, (par)->height,                              \
              av_get_pix_fmt_name((par)->format),                       \
              ctx->st_timebase.num, ctx->st_timebase.den);              \
    }                                                                   \
} while (0)

    DUMP_INFO(stream->codecpar, "original");

    ret = nmdi_decoder_init(log_ctx, ctx->decoder, decs[0], stream, ctx, opts);
    for (int i = 1; ret < 0 && i < nb_decs; i++) {
        TRACE(ctx, "unable to init %s decoder, fallback on %s decoder",
              decs[i - 1]->name, decs[i]->name);
        if (ret != AVERROR_DECODER_NOT_FOUND)
            LOG(ctx, ERROR, "Decoder fallback due to %s", av_err2str(ret));
        ret = nmdi_decoder_init(log_ctx, ctx->decoder, decs[i], stream, ctx, opts);
    }
    if (ret < 0) {
        avcodec_parameters_free(&par);
        return ret;
    }

    avcodec_parameters_from_context(par, ctx->decoder->avctx);
    DUMP_INFO(par, "initialized");

    LOG(ctx, INFO, "selected decoder: %s", ctx->decoder->dec->name);

    avcodec_parameters_free(&par);

    return 0;
}

static int64_t get_best_effort_ts(const AVFrame *f)
{
    const int64_t t = f->best_effort_timestamp;
    return t != AV_NOPTS_VALUE ? t : f->pts;
}

static int queue_frame(struct decoding_ctx *ctx, AVFrame *frame)
{
    int ret;
    struct message msg = {
        .type = MSG_FRAME,
        .data = frame,
    };

    if (ctx->is_image && ctx->frame_count++ > 0)
        return AVERROR_EOF;

    TRACE(ctx, "queue frame with ts=%s", av_ts2timestr(frame->pts, &ctx->st_timebase));

    ret = av_thread_message_queue_send(ctx->frames_queue, &msg, 0);
    if (ret < 0) {
        if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
            LOG(ctx, ERROR, "Unable to push frame: %s", av_err2str(ret));
        av_thread_message_queue_set_err_recv(ctx->frames_queue, ret);
    }
    return ret;
}

static int queue_cached_frame(struct decoding_ctx *ctx)
{
    int ret;
    TRACE(ctx, "got a cached frame (t=%s) to push", av_ts2timestr(ctx->tmp_frame->pts, &ctx->st_timebase));
    AVFrame *prev_frame = ctx->tmp_frame;
    ctx->tmp_frame = NULL;
    ret = queue_frame(ctx, prev_frame);
    if (ret < 0) {
        av_frame_free(&prev_frame);
        return ret;
    }
    return 0;
}

static void reset_passes(struct decoding_ctx *ctx)
{
    ctx->nb_passes = 0;
    ctx->looping = 0;
    ctx->pass = 0;
}

/* Output buffered frames before clearing the decoder state for the next pass. */
static int drain_decoder(struct decoding_ctx *ctx)
{
    int ret;
    do {
        ret = nmdi_decoder_push_packet(ctx->decoder, NULL);
    } while (ret == 0 || ret == AVERROR(EAGAIN));
    if (ret != AVERROR_EOF)
        return ret;
    nmdi_decoder_flush(ctx->decoder);
    return 0;
}

static int update_pass(struct decoding_ctx *ctx, const struct nmdi_pass_msg *msg)
{
    ctx->looping = 1;

    struct nmdi_pass_msg *last = ctx->nb_passes ? &ctx->passes[ctx->nb_passes - 1] : NULL;
    if (last && last->pass == msg->pass) {
        last->end = msg->end;
        return 0;
    }

    if (ctx->nb_passes == ctx->passes_size) {
        const size_t size = FFMAX(ctx->passes_size * 2, 16);
        struct nmdi_pass_msg *passes = av_realloc_array(ctx->passes, size, sizeof(*passes));
        if (!passes)
            return AVERROR(ENOMEM);
        ctx->passes = passes;
        ctx->passes_size = size;
    }
    ctx->passes[ctx->nb_passes++] = *msg;
    return 0;
}

/*
 * Decoder output is ordered by pass, so earlier passes can be released.
 */
static const struct nmdi_pass_msg *get_pass(struct decoding_ctx *ctx, int64_t pass)
{
    size_t i = 0;
    while (i < ctx->nb_passes && ctx->passes[i].pass < pass)
        i++;
    if (i == ctx->nb_passes || ctx->passes[i].pass != pass)
        return NULL;
    if (i) {
        ctx->nb_passes -= i;
        memmove(ctx->passes, ctx->passes + i, ctx->nb_passes * sizeof(*ctx->passes));
    }
    return &ctx->passes[0];
}

/*
 * Remove the pass offset to recover the media timestamp, and attach the pass
 * number to the frame. Return 1 to drop frames from unknown passes or at or
 * after their pass end; return 0 to keep the frame.
 */
static int set_frame_pass(struct decoding_ctx *ctx, AVFrame *frame, int64_t *tsp)
{
    const int64_t pass = nmdi_pass_from_timestamp(&ctx->timeline, *tsp);
    const struct nmdi_pass_msg *pass_msg = pass >= ctx->pass ? get_pass(ctx, pass) : NULL;
    if (!pass_msg) {
        LOG(ctx, WARNING, "dropping a frame of the loop pass %" PRId64 " in pass %" PRId64, pass, ctx->pass);
        return 1;
    }

    const int64_t ts = *tsp - pass * ctx->timeline.stride;
    if (ts >= pass_msg->end) {
        TRACE(ctx, "frame ts:%s past the end of the loop", av_ts2timestr(ts, &ctx->st_timebase));
        nmdi_loop_drop_frame(ctx->loop, pass_msg->seek_rev, pass, ts);
        return 1;
    }

    if (pass > ctx->pass) {
        TRACE(ctx, "start of the loop pass %" PRId64, pass);
        ctx->pass = pass;
        ctx->seek_request = pass_msg->start;
    }

    frame->opaque = (void *)(intptr_t)pass;
    *tsp = ts;
    return 0;
}

int nmdi_decoding_queue_frame(struct decoding_ctx *ctx, AVFrame *frame)
{
    int ret;

    if (!frame) {
        TRACE(ctx, "async_queue_frame() called for flushing");
        if (ctx->tmp_frame) {
            ret = queue_cached_frame(ctx);
            if (ret < 0)
                return ret;
        }
        return AVERROR_EOF;
    }

    int64_t ts = get_best_effort_ts(frame);
    TRACE(ctx, "processing frame with ts=%s", av_ts2timestr(ts, &ctx->st_timebase));

    const int64_t prev_pass = ctx->pass;
    if (ctx->looping) {
        ret = set_frame_pass(ctx, frame, &ts);
        if (ret) {
            av_frame_free(&frame);
            return 0;
        }
    }

    /* Emit the previous pass's cached frame before processing the next pass. */
    if (ctx->pass != prev_pass && ctx->tmp_frame) {
        ret = queue_cached_frame(ctx);
        if (ret < 0)
            return ret;
    }

    frame->pts = ts;

    if (ctx->seek_request != AV_NOPTS_VALUE && ts < ctx->seek_request) {
        TRACE(ctx, "frame ts:%s (%"PRId64"), skipping because before %s (%"PRId64")",
              av_ts2timestr(ts, &ctx->st_timebase), ts,
              av_ts2timestr(ctx->seek_request, &ctx->st_timebase), ctx->seek_request);
        av_frame_free(&ctx->tmp_frame);
        ctx->tmp_frame = frame;
        /* Display this cached frame from the loop start, trimming its duration accordingly. */
        if (ctx->pass) {
            if (frame->duration > 0)
                frame->duration = FFMAX(ts + frame->duration - ctx->seek_request, 0);
            frame->pts = ctx->seek_request;
        }
        return 0;
    }

    if (ctx->tmp_frame) {
        if (ctx->seek_request != AV_NOPTS_VALUE && ts == ctx->seek_request) {
            av_frame_free(&ctx->tmp_frame);
        } else {
            ret = queue_cached_frame(ctx);
            if (ret < 0)
                return ret;
        }
    } else {
        if (ctx->seek_request != AV_NOPTS_VALUE && (ctx->seek_request > 0 || ctx->pass) &&
            frame->pts > ctx->seek_request) {
            TRACE(ctx, "first frame obtained is after requested time, fixup its ts from %s to %s",
                  av_ts2timestr(frame->pts, &ctx->st_timebase),
                  av_ts2timestr(ctx->seek_request, &ctx->st_timebase));
            frame->pts = ctx->seek_request;
        }
    }

    ctx->seek_request = AV_NOPTS_VALUE;
    return queue_frame(ctx, frame);
}

void nmdi_decoding_run(struct decoding_ctx *ctx)
{
    int ret;
    int in_err, out_err;

    TRACE(ctx, "decoding packets from %p into %p", ctx->pkt_queue, ctx->frames_queue);

    ctx->seek_request = AV_NOPTS_VALUE;
    reset_passes(ctx);

    for (;;) {
        AVPacket *pkt;
        struct message msg;

        TRACE(ctx, "fetching a packet");
        ret = av_thread_message_queue_recv(ctx->pkt_queue, &msg, 0);
        if (ret < 0)
            break;

        if (msg.type == MSG_SEEK) {
            const int64_t seek_ts = *(int64_t *)msg.data;

            TRACE(ctx, "got a seek message (to %s) in the pkt queue",
                  PTS2TIMESTR(seek_ts));

            /* Make sure the decoder has no packet remaining to consume and
             * pushed (or dropped) all its cached frames. After this flush, we
             * can assume that the decoder will not called async_queue_frame()
             * until a new packet is pushed. */
            nmdi_decoder_flush(ctx->decoder);

            av_frame_free(&ctx->tmp_frame);

            /* Let's save some little time by dropping frames in the queue so
             * the user don't get a shit ton of false positives before the
             * frames he requested. */
            av_thread_message_flush(ctx->frames_queue);

            /* Mark the seek request so async_queue_frame() can do its
             * "filtering" work. */
            ctx->seek_request = av_rescale_q(seek_ts, AV_TIME_BASE_Q, ctx->st_timebase);
            reset_passes(ctx);

            /* Forward seek message */
            ret = av_thread_message_queue_send(ctx->frames_queue, &msg, 0);
            if (ret < 0) {
                nmdi_msg_free_data(&msg);
                break;
            }

            continue;
        }

        if (msg.type == MSG_LOOP) {
            const struct nmdi_pass_msg *pass_msg = msg.data;
            ret = 0;
            if (pass_msg->reset) {
                TRACE(ctx, "reset the decoder for the loop pass %" PRId64, pass_msg->pass);
                ret = drain_decoder(ctx);
            }
            if (ret >= 0)
                ret = update_pass(ctx, pass_msg);
            nmdi_msg_free_data(&msg);
            if (ret < 0)
                break;
            continue;
        }

        pkt = msg.data;
        TRACE(ctx, "got a packet of size %d, push it to decoder", pkt->size);
        ret = nmdi_decoder_push_packet(ctx->decoder, pkt);
        av_packet_free(&pkt);
        if (ret < 0)
            break;
    }

    /* Fetch remaining frames */
    if (ret == AVERROR_EOF) {
        TRACE(ctx, "flush cached frames");
        do {
            ret = nmdi_decoder_push_packet(ctx->decoder, NULL);
        } while (ret == 0 || ret == AVERROR(EAGAIN));
    }

    /* We pushed everything we could to the decoder, now we make sure frame
     * queuing callback won't be called anymore */
    nmdi_decoder_flush(ctx->decoder);

    av_frame_free(&ctx->tmp_frame);

    if (ret < 0 && ret != AVERROR_EOF) {
        in_err = out_err = ret;
    } else {
        in_err = AVERROR_EXIT;
        out_err = AVERROR_EOF;
    }
    TRACE(ctx, "notify demuxer with %s and frames queue with %s",
          av_err2str(in_err), av_err2str(out_err));
    av_thread_message_queue_set_err_send(ctx->pkt_queue,    in_err);
    av_thread_message_flush(ctx->pkt_queue);
    av_thread_message_queue_set_err_recv(ctx->frames_queue, out_err);
}

void nmdi_decoding_free(struct decoding_ctx **ctxp)
{
    struct decoding_ctx *ctx = *ctxp;
    if (!ctx)
        return;
    nmdi_decoder_free(&ctx->decoder);
    av_freep(&ctx->passes);
    av_freep(ctxp);
}
