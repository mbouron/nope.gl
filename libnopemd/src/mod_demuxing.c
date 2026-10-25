/*
 * This file is part of nope.media.
 *
 * Copyright (c) 2023 Nope Forge
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

#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/display.h>
#include <libavutil/eval.h>
#include <libavutil/opt.h>

#include "mod_demuxing.h"
#include "internal.h"
#include "log.h"
#include "loop.h"
#include "msg.h"

#define H264_NAL_IDR_SLICE   5
#define HEVC_NAL_IDR_W_RADL 19
#define HEVC_NAL_IDR_N_LP   20

#define MAX_SEEK_RETRIES 8
#define MAX_SEEK_DROPS   4096

struct demuxing_ctx {
    void *log_ctx;
    AVFormatContext *fmt_ctx;
    AVStream *stream;
    int stream_idx;
    int is_image;
    AVThreadMessageQueue *src_queue;
    AVThreadMessageQueue *pkt_queue;
    nmd_seek_callback_type seek_callback;
    void *seek_callback_arg;
    int64_t start_time64;
    AVPacket *seek_pkt;             // key frame packet found by the last seek
    int has_seek_pkt;

    struct nmdi_loop *loop;
    int64_t end_time;               // `end_time` option in microseconds
    struct nmdi_pass_timeline timeline;
    int nal_length_size;            // H.264/HEVC NAL unit length size, 0 for Annex B
    int64_t seek_rev;               // revision of pass numbering, copied from the shared loop state
    int64_t pass;                   // loop pass of the packets read
    int64_t pass_start;             // requested start of the pass, in stream ticks without the pass offset
    int64_t pass_end;               // exclusive end sent for the pass, in stream ticks; `INT64_MAX` if none
    int pass_pending;               // send the pass description before its first packet
    int pass_has_candidate_packet;  // set after reading a packet that could produce a frame before the loop end
};

struct demuxing_ctx *nmdi_demuxing_alloc(void)
{
    struct demuxing_ctx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    return ctx;
}

// XXX: we should probably prefer the stream duration over the format
// duration
int64_t nmdi_demuxing_probe_duration(const struct demuxing_ctx *ctx)
{
    if (!ctx->is_image) {
        int64_t probe_duration64 = ctx->fmt_ctx->duration;
        AVRational scaleq = AV_TIME_BASE_Q;

        if (probe_duration64 == AV_NOPTS_VALUE && ctx->stream->time_base.den) {
            probe_duration64 = ctx->stream->duration;
            scaleq = ctx->stream->time_base;
        }

        if (probe_duration64 != AV_NOPTS_VALUE)
            return av_rescale_q_rnd(probe_duration64, scaleq, AV_TIME_BASE_Q, 0);
    }
    return AV_NOPTS_VALUE;
}

double nmdi_demuxing_probe_rotation(const struct demuxing_ctx *ctx)
{
    AVStream *st = (AVStream *)ctx->stream; // XXX: Fix FFmpeg.
    AVDictionaryEntry *rotate_tag = av_dict_get(st->metadata, "rotate", NULL, 0);
    const AVCodecParameters *par = st->codecpar;
    const AVPacketSideData *side_data = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    const uint8_t *displaymatrix = side_data ? side_data->data : NULL;
    double theta = 0;

    if (rotate_tag && *rotate_tag->value && strcmp(rotate_tag->value, "0")) {
        char *tail;
        theta = av_strtod(rotate_tag->value, &tail);
        if (*tail)
            theta = 0;
    }
    if (displaymatrix && !theta)
        theta = -av_display_rotation_get((const int32_t *)displaymatrix);
    theta -= 360*floor(theta/360 + 0.9/360);
    return theta;
}

const AVStream *nmdi_demuxing_get_stream(const struct demuxing_ctx *ctx)
{
    return ctx->stream;
}

const struct nmdi_pass_timeline *nmdi_demuxing_get_pass_timeline(const struct demuxing_ctx *ctx)
{
    return &ctx->timeline;
}

int nmdi_demuxing_is_image(const struct demuxing_ctx *ctx)
{
    return ctx->is_image;
}

int nmdi_demuxing_init(void *log_ctx,
                       struct demuxing_ctx *ctx,
                       AVThreadMessageQueue *src_queue,
                       AVThreadMessageQueue *pkt_queue,
                       const char *filename,
                       struct nmdi_loop *loop,
                       const struct nmdi_opts *opts)
{
    enum AVMediaType media_type;

    ctx->log_ctx = log_ctx;

    ctx->src_queue = src_queue;
    ctx->pkt_queue = pkt_queue;
    ctx->seek_callback = opts->seek_callback;
    ctx->seek_callback_arg = opts->seek_callback_arg;
    ctx->start_time64 = opts->start_time64;
    ctx->loop = loop;
    ctx->end_time = opts->end_time64;

    switch (opts->avselect) {
    case NMD_SELECT_VIDEO: media_type = AVMEDIA_TYPE_VIDEO; break;
    case NMD_SELECT_AUDIO: media_type = AVMEDIA_TYPE_AUDIO; break;
    default:
        av_assert0(0);
    }

    AVDictionary *fmt_opts = NULL;
    if (!strncmp(filename, "fd:", 3)) {
        int fd;
        int ret = sscanf(filename, "fd:%d", &fd);
        if (ret != 1)
            return AVERROR(EINVAL);

        ret = av_dict_set_int(&fmt_opts, "fd", fd, 0);
        if (ret < 0)
            return ret;

        filename = "fd:";
    }

    TRACE(ctx, "opening %s", filename);
    int ret = avformat_open_input(&ctx->fmt_ctx, filename, NULL, &fmt_opts);
    av_dict_free(&fmt_opts);

    if (ret < 0) {
        LOG(ctx, ERROR, "Unable to open input file '%s'", filename);
        return ret;
    }

    TRACE(ctx, "find stream info");
    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        LOG(ctx, ERROR, "Unable to find input stream information");
        return ret;
    }

    TRACE(ctx, "find best stream");
    ret = av_find_best_stream(ctx->fmt_ctx, media_type, opts->stream_idx, -1, NULL, 0);
    if (ret < 0) {
        LOG(ctx, ERROR, "Unable to find a %s stream in the input file",
            av_get_media_type_string(media_type));
        return ret;
    }
    ctx->stream_idx = ret;
    ctx->stream = ctx->fmt_ctx->streams[ctx->stream_idx];
    ctx->is_image = strstr(ctx->fmt_ctx->iformat->name, "image2") ||
                    strstr(ctx->fmt_ctx->iformat->name, "_pipe");
    LOG(ctx, INFO, "Selected %s stream %d",
        av_get_media_type_string(media_type), ctx->stream_idx);

    /* Automatically discard all the other streams so we don't have to filter
     * them out most of the time */
    for (int i = 0; i < ctx->fmt_ctx->nb_streams; i++)
        if (i != ctx->stream_idx)
            ctx->fmt_ctx->streams[i]->discard = AVDISCARD_ALL;

    av_dump_format(ctx->fmt_ctx, 0, filename, 0);

    ctx->seek_pkt = av_packet_alloc();
    if (!ctx->seek_pkt)
        return AVERROR(ENOMEM);

    nmdi_pass_timeline_init(&ctx->timeline, ctx->stream, nmdi_demuxing_probe_duration(ctx));

    const AVCodecParameters *par = ctx->stream->codecpar;
    if (par->codec_id == AV_CODEC_ID_H264 && par->extradata_size > 4 && par->extradata[0] == 1)
        ctx->nal_length_size = (par->extradata[4] & 3) + 1;
    else if (par->codec_id == AV_CODEC_ID_HEVC && par->extradata_size > 22 &&
             (par->extradata[0] || par->extradata[1] || par->extradata[2] > 1))
        ctx->nal_length_size = (par->extradata[21] & 3) + 1;

    return 0;
}

static int read_stream_packet(struct demuxing_ctx *ctx, AVPacket *pkt)
{
    int ret;
    AVFormatContext *fmt_ctx = ctx->fmt_ctx;
    const int target_stream_idx = ctx->stream->index;

    for (;;) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0)
            break;

        if (pkt->stream_index != target_stream_idx) {
            TRACE(ctx, "pkt->idx=%d vs %d",
                  pkt->stream_index, target_stream_idx);
            av_packet_unref(pkt);
            continue;
        }

        break;
    }

    TRACE(ctx, "packet ret %s", av_err2str(ret));
    return ret;
}

static int pull_packet(struct demuxing_ctx *ctx, AVPacket *pkt)
{
    if (ctx->has_seek_pkt) {
        av_packet_move_ref(pkt, ctx->seek_pkt);
        ctx->has_seek_pkt = 0;
        return 0;
    }
    return read_stream_packet(ctx, pkt);
}

/* Read up to the next key frame packet, AVERROR(EAGAIN) if none is flagged */
static int read_key_packet(struct demuxing_ctx *ctx, AVPacket *pkt)
{
    for (int i = 0; i < MAX_SEEK_DROPS; i++) {
        int ret = read_stream_packet(ctx, pkt);
        if (ret < 0)
            return ret;
        if (pkt->flags & AV_PKT_FLAG_KEY)
            return 0;
        av_packet_unref(pkt);
    }
    return AVERROR(EAGAIN);
}

/*
 * Seek to a key frame displayed at or before ts (AV_TIME_BASE).
 *
 * avformat_seek_file() may seek by DTS, so the key frame's PTS can be past the
 * requested time (ts).
 *
 * The MPEG-TS demuxer also searches timestamps without checking key frame
 * flags, and can land on packets the decoder cannot start from. Read up to the
 * next key frame and seek further back if its PTS is past ts.
 */
static int seek(struct demuxing_ctx *ctx, int64_t ts)
{
    AVPacket *pkt = ctx->seek_pkt;
    const AVRational tb = ctx->stream->time_base;
    const int64_t target = av_rescale_q(ts, AV_TIME_BASE_Q, tb);
    const int64_t first_pts = ctx->stream->start_time;
    int64_t seek_ts = ts;
    int64_t seek_offset = 0;

    av_packet_unref(pkt);
    ctx->has_seek_pkt = 0;

    for (int i = 0; i <= MAX_SEEK_RETRIES; i++) {
        int ret = avformat_seek_file(ctx->fmt_ctx, -1, INT64_MIN, seek_ts, seek_ts, 0);
        if (ret < 0)
            break;

        ret = read_key_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN))
            break;
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;

        if (ret >= 0) {
            const int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            const int first = first_pts != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && pts <= first_pts;
            if (pts == AV_NOPTS_VALUE || pts <= target || first || i == MAX_SEEK_RETRIES) {
                ctx->has_seek_pkt = 1;
                return 0;
            }
        }

        /* Seek further back if the demuxer keeps landing past the requested time. */
        const int64_t key_ts = ret < 0 ? INT64_MAX : av_rescale_q(pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts, tb, AV_TIME_BASE_Q);
        av_packet_unref(pkt);
        if (key_ts > seek_ts) {
            seek_offset = seek_offset ? seek_offset * 2 : AV_TIME_BASE / 2;
            seek_ts -= seek_offset;
        } else {
            seek_ts = key_ts - 1;
        }
        TRACE(ctx, "no key frame displayed at ts=%s, seek back to ts=%s", PTS2TIMESTR(ts), PTS2TIMESTR(seek_ts));
    }

    LOG(ctx, WARNING, "No key frame found to start from at ts=%s", PTS2TIMESTR(ts));
    return avformat_seek_file(ctx->fmt_ctx, -1, INT64_MIN, ts, ts, 0);
}

static void reset_pass(struct demuxing_ctx *ctx, int64_t start)
{
    ctx->pass = 0;
    ctx->pass_start = start;
    ctx->pass_end = INT64_MAX;
    ctx->pass_pending = 0;
    ctx->pass_has_candidate_packet = 0;
    ctx->seek_rev = nmdi_loop_reset_passes(ctx->loop);
}

/*
 * Read the requested range and its revision. `start_us` is in microseconds;
 * `start` and `end` are in stream ticks, without the pass offset.
 * `end` is `INT64_MAX` if this pass cannot loop.
 */
static void get_loop(struct demuxing_ctx *ctx, int64_t *start_us, int64_t *start, int64_t *end, int64_t *loop_rev)
{
    *start_us = *start = AV_NOPTS_VALUE;
    *end = INT64_MAX;

    int64_t loop_start, loop_end;
    nmdi_loop_get(ctx->loop, &loop_start, &loop_end, loop_rev);

    const struct nmdi_pass_timeline *timeline = &ctx->timeline;
    if (!timeline->stride || ctx->pass >= timeline->max_pass)
        return;

    if (loop_end == AV_NOPTS_VALUE)
        return;

    /*
     * Keep media timestamps within the interval reserved for pass 0, so adding
     * a pass offset cannot place them in another pass's interval. Clipping
     * also prevents overflow when rescaling.
     */
    const AVRational tb = ctx->stream->time_base;
    const int64_t timeline_end = timeline->origin + timeline->stride;
    const int64_t timeline_end_us = av_rescale_q(timeline_end, tb, AV_TIME_BASE_Q);
    const int64_t loop_start_ts = av_rescale_q(FFMIN(loop_start, timeline_end_us), AV_TIME_BASE_Q, tb);
    const int64_t loop_end_ts = FFMIN(nmdi_loop_stream_end(FFMIN(loop_end, timeline_end_us), ctx->end_time, tb), timeline_end);
    if (loop_end_ts <= loop_start_ts || ctx->pass_start >= loop_end_ts)
        return;

    *start_us = loop_start;
    *start = loop_start_ts;
    *end = loop_end_ts;
}

static int send_pass(struct demuxing_ctx *ctx, int64_t start, int64_t end, int reset)
{
    struct nmdi_pass_msg *pass_msg = av_malloc(sizeof(*pass_msg));
    if (!pass_msg)
        return AVERROR(ENOMEM);
    *pass_msg = (struct nmdi_pass_msg){
        .seek_rev = ctx->seek_rev,
        .pass     = ctx->pass,
        .start    = start,
        .end      = end,
        .reset    = reset,
    };

    struct message msg = {.type = MSG_LOOP, .data = pass_msg};
    int ret = av_thread_message_queue_send(ctx->pkt_queue, &msg, 0);
    if (ret < 0) {
        nmdi_msg_free_data(&msg);
        return ret;
    }
    ctx->pass_end = end;
    ctx->pass_pending = 0;
    return 0;
}

/*
 * No packet in this pass could produce a frame before the loop end. Retry
 * from the same start without the loop cutoff, so a request before the first
 * media frame can still return that frame.
 */
static int stop_looping(struct demuxing_ctx *ctx)
{
    const AVRational tb = ctx->stream->time_base;
    const int64_t first_ts = ctx->stream->start_time != AV_NOPTS_VALUE ? ctx->stream->start_time : 0;
    const int64_t start = ctx->pass_start == INT64_MIN ? first_ts : ctx->pass_start;
    int ret = seek(ctx, av_rescale_q(start, tb, AV_TIME_BASE_Q));
    if (ret < 0)
        return ret;

    ctx->pass_start = INT64_MAX;
    return send_pass(ctx, AV_NOPTS_VALUE, INT64_MAX, 1);
}

static int start_next_pass(struct demuxing_ctx *ctx, int64_t start_us, int64_t start, int64_t loop_rev, int64_t prev_end)
{
    TRACE(ctx, "loop back to ts=%s", PTS2TIMESTR(start_us));
    int ret = seek(ctx, start_us);
    if (ret < 0) {
        LOG(ctx, ERROR, "Unable to loop back to ts=%s", PTS2TIMESTR(start_us));
        return ret;
    }

    ctx->pass++;
    ctx->pass_start = start;
    ctx->pass_pending = 1;
    ctx->pass_has_candidate_packet = 0;
    return nmdi_loop_start_pass(ctx->loop, ctx->pass, loop_rev, prev_end);
}

/* Type of the first VCL NAL unit of an H.264 or HEVC packet, -1 if none */
static int get_vcl_nal(const struct demuxing_ctx *ctx, const AVPacket *pkt, const uint8_t **nalp, size_t *sizep)
{
    const int hevc = ctx->stream->codecpar->codec_id == AV_CODEC_ID_HEVC;
    const uint8_t *data = pkt->data;
    const size_t size = (size_t)pkt->size;
    const size_t length_size = (size_t)ctx->nal_length_size;
    size_t pos = 0;

    for (;;) {
        size_t nal_size;
        if (length_size) {
            if (size - pos < length_size)
                return -1;
            nal_size = 0;
            for (size_t i = 0; i < length_size; i++)
                nal_size = nal_size << 8 | data[pos++];
            if (!nal_size || nal_size > size - pos)
                return -1;
        } else {
            while (size - pos >= 4 && (data[pos] || data[pos + 1] || data[pos + 2] != 1))
                pos++;
            if (size - pos < 4)
                return -1;
            pos += 3;
            nal_size = size - pos;
        }

        const int type = hevc ? data[pos] >> 1 & 0x3f : data[pos] & 0x1f;
        if (hevc ? type < 32 : type >= 1 && type <= 5) {
            *nalp = data + pos;
            *sizep = nal_size;
            return type;
        }
        if (length_size)
            pos += nal_size;
    }
}

/*
 * Keep decoding across a wrap only if the restart packet can reset frame
 * references while allowing buffered frames to be output. Otherwise, drain
 * and flush first: leading frames in an open GOP could use references left
 * over from the previous repetition.
 */
static int is_clean_restart(const struct demuxing_ctx *ctx, const AVPacket *pkt)
{
    const AVCodecParameters *par = ctx->stream->codecpar;
    if (par->codec_type == AVMEDIA_TYPE_AUDIO)
        return 1;
    if (!(pkt->flags & AV_PKT_FLAG_KEY))
        return 0;

    const uint8_t *nal;
    size_t nal_size;
    switch (par->codec_id) {
    case AV_CODEC_ID_VP8:
    case AV_CODEC_ID_VP9:
    case AV_CODEC_ID_AV1:
        return 1;
    case AV_CODEC_ID_H264:
        return get_vcl_nal(ctx, pkt, &nal, &nal_size) == H264_NAL_IDR_SLICE;
    case AV_CODEC_ID_HEVC: {
        const int type = get_vcl_nal(ctx, pkt, &nal, &nal_size);
        /*
         * A set `no_output_of_prior_pics_flag` would discard buffered pictures.
         * It follows `first_slice_segment_in_pic_flag` in the slice header.
         */
        return (type == HEVC_NAL_IDR_W_RADL || type == HEVC_NAL_IDR_N_LP) && nal_size > 2 && !(nal[2] & 0x40);
    }
    default:
        return 0;
    }
}

/*
 * Use decoding order (DTS) to decide when to wrap. A reference frame shown
 * after the loop end may still be needed to decode a frame shown before it.
 * The decoder discards output frames at or after the loop end.
 */
static int read_packet(struct demuxing_ctx *ctx, AVPacket *pkt)
{
    for (;;) {
        int64_t start_us, start, end, loop_rev;
        get_loop(ctx, &start_us, &start, &end, &loop_rev);
        if (!ctx->pass_pending && end != ctx->pass_end) {
            int ret = send_pass(ctx, start, end, 0);
            if (ret < 0)
                return ret;
        }

        int ret = pull_packet(ctx, pkt);
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;

        /*
         * Decode packets with unknown DTS, such as the first packets after a
         * Matroska seek. Their presentation times cannot safely determine the
         * end of the pass: later packets may still depend on them.
         */
        const int64_t dts = ret < 0 ? AV_NOPTS_VALUE : pkt->dts;
        int wrap = end != INT64_MAX && (ret == AVERROR_EOF || (dts != AV_NOPTS_VALUE && dts >= end));
        if (wrap && !ctx->pass_has_candidate_packet) {
            LOG(ctx, WARNING, "No frame in the loop range, not looping anymore");
            av_packet_unref(pkt);
            ret = stop_looping(ctx);
            if (ret < 0)
                return ret;
            continue;
        }

        if (wrap) {
            /* Record the first packet left out of this pass, or EOF if all were sent. */
            const int64_t pass_end = ret == AVERROR_EOF ? INT64_MAX : dts;
            av_packet_unref(pkt);
            ret = start_next_pass(ctx, start_us, start, loop_rev, pass_end);
            if (ret < 0)
                return ret;
            /* Without a usable key frame to restart decoding, continue without looping. */
            if (!ctx->has_seek_pkt) {
                LOG(ctx, WARNING, "No key frame to loop back to, not looping anymore");
                ctx->pass_start = INT64_MAX;
                ret = send_pass(ctx, AV_NOPTS_VALUE, INT64_MAX, 1);
                if (ret < 0)
                    return ret;
            }
            continue;
        }

        if (ret < 0)
            return ret;

        /* Packets without a known timestamp may still produce a frame before the loop end. */
        const int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        if (end == INT64_MAX || pts == AV_NOPTS_VALUE || pts < end)
            ctx->pass_has_candidate_packet = 1;
        if (ctx->pass_pending) {
            ret = send_pass(ctx, ctx->pass_start, end, !is_clean_restart(ctx, pkt));
            if (ret < 0)
                return ret;
        }

        if (ctx->pass) {
            const int64_t offset = ctx->pass * ctx->timeline.stride;
            if (pkt->pts != AV_NOPTS_VALUE)
                pkt->pts += offset;
            if (pkt->dts != AV_NOPTS_VALUE)
                pkt->dts += offset;
        }
        return 0;
    }
}

void nmdi_demuxing_run(struct demuxing_ctx *ctx)
{
    int ret;
    int in_err, out_err;

    TRACE(ctx, "demuxing packets in queue %p", ctx->pkt_queue);

    reset_pass(ctx, INT64_MIN);

    for (;;) {
        struct message msg;

        ret = av_thread_message_queue_recv(ctx->src_queue, &msg, AV_THREAD_MESSAGE_NONBLOCK);
        if (ret != AVERROR(EAGAIN)) {
            if (ret < 0)
                break;

            if (msg.type == MSG_SEEK) {
                av_assert0(!ctx->is_image);

                /* Make later modules stop working ASAP */
                av_thread_message_flush(ctx->pkt_queue);

                /* do actual seek so the following packet that will be pulled in
                 * this current thread will be at the (approximate) requested time */
                const int64_t seek_to = *(int64_t *)msg.data;
                LOG(ctx, INFO, "Seek in media at ts=%s", PTS2TIMESTR(seek_to));
                ret = seek(ctx, seek_to);
                if (ret < 0) {
                    nmdi_msg_free_data(&msg);
                    break;
                }
                reset_pass(ctx, av_rescale_q(seek_to, AV_TIME_BASE_Q, ctx->stream->time_base));
                if (ctx->seek_callback) {
                    const double t = (seek_to - ctx->start_time64) / (double)AV_TIME_BASE;
                    ctx->seek_callback(ctx->seek_callback_arg, t);
                }
            }

            /* Forward the message */
            ret = av_thread_message_queue_send(ctx->pkt_queue, &msg, 0);
            if (ret < 0) {
                nmdi_msg_free_data(&msg);
                break;
            }
        }

        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            ret = AVERROR(ENOMEM);
            break;
        }

        ret = read_packet(ctx, pkt);
        if (ret < 0) {
            av_packet_free(&pkt);
            break;
        }

        TRACE(ctx, "pulled a packet of size %d, sending to decoder", pkt->size);

        msg.type = MSG_PACKET;
        msg.data = pkt;

        ret = av_thread_message_queue_send(ctx->pkt_queue, &msg, 0);
        TRACE(ctx, "sent packet to decoder, ret=%s", av_err2str(ret));

        if (ret < 0) {
            nmdi_msg_free_data(&msg);
            if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
                LOG(ctx, ERROR, "Unable to send packet to decoder: %s", av_err2str(ret));
            TRACE(ctx, "can't send pkt to decoder: %s", av_err2str(ret));
            av_thread_message_queue_set_err_recv(ctx->pkt_queue, ret);
            break;
        }
    }

    if (ret < 0 && ret != AVERROR_EOF) {
        in_err = out_err = ret;
    } else {
        in_err = AVERROR_EXIT;
        out_err = AVERROR_EOF;
    }
    TRACE(ctx, "notify user with %s and decoder with %s",
          av_err2str(in_err), av_err2str(out_err));
    av_thread_message_queue_set_err_send(ctx->src_queue, in_err);
    av_thread_message_flush(ctx->src_queue);
    av_thread_message_queue_set_err_recv(ctx->pkt_queue, out_err);
}

void nmdi_demuxing_free(struct demuxing_ctx **ctxp)
{
    struct demuxing_ctx *ctx = *ctxp;
    if (!ctx)
        return;
    av_packet_free(&ctx->seek_pkt);
    avformat_close_input(&ctx->fmt_ctx);
    av_freep(ctxp);
}
