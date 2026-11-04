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

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/fifo.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavutil/tx.h>

#include "nopemd.h"
#include "internal.h"
#include "mod_filtering.h"
#include "log.h"
#include "msg.h"

#define AUDIO_NBITS      10
#define AUDIO_NBSAMPLES  (1<<(AUDIO_NBITS))
#define AUDIO_NBCHANNELS 2

struct frame_info {
    int format;
    int width;
    int height;
    AVChannelLayout channel_layout;
    int sample_rate;
};

static void reset_frame_info(struct frame_info *frame_info)
{
    frame_info->format = AV_PIX_FMT_NONE;
    frame_info->width = 0;
    frame_info->height = 0;
    frame_info->sample_rate = 0;
    av_channel_layout_uninit(&frame_info->channel_layout);
}

/*
 * Track pass boundaries until buffered output reaches them.
 * Sample indices count from the graph's first input sample.
 */
struct pass_marker {
    int64_t pass_number;
    int64_t sample_index;
    int64_t sample_pts;
    int64_t sample_pts_offset;
    int forward;
};

struct filtering_io {
    int64_t pass_number;
    int64_t pts_offset;
    int64_t end_pts;
    int64_t sample_count;
    int has_frame;
};

static void reset_filtering_io(struct filtering_io *io, int64_t pass_number)
{
    *io = (struct filtering_io){
        .pass_number = pass_number,
        .end_pts     = AV_NOPTS_VALUE,
    };
}

struct filtering_ctx {
    void *log_ctx;

    AVThreadMessageQueue *in_queue;
    AVThreadMessageQueue *out_queue;

    AVCodecParameters *codecpar;
    char *filters;
    int sw_pix_fmt;
    int max_pixels;
    int audio_texture;
    AVRational st_timebase;

    AVFilterGraph *filter_graph;
    struct frame_info frame_info;
    struct nmdi_loop *loop;

    int continuous_audio;                  // preserve audio filter state across passes

    /* Filters may still output an earlier pass while receiving the next ones. */
    struct filtering_io in;
    int64_t in_pending_pass_number;         // pass awaiting its first frame; -1 if none

    AVFifo *pass_markers;                   // pass boundaries not yet reached by output

    struct filtering_io out;

    AVFilterContext *buffersink_ctx;        // graph output
    AVFilterContext *buffersrc_ctx;         // graph input
    float *window_func_lut;                 // audio window function lookup table
    AVTXContext *avtx;
    av_tx_fn avtx_func;
    float *rdft_data[AUDIO_NBCHANNELS];     // real discrete fourier transform data for each channel
};

struct filtering_ctx *nmdi_filtering_alloc(void)
{
    struct filtering_ctx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->codecpar = avcodec_parameters_alloc();
    if (!ctx->codecpar) {
        av_freep(&ctx);
        return NULL;
    }
    return ctx;
}

/**
 * Convert an audio frame (PCM data) to a textured video frame with waves and
 * FFT lines
 */
static void audio_frame_to_sound_texture(struct filtering_ctx *ctx, AVFrame *dst_video,
                                         const AVFrame *audio_src)
{
    const int nb_samples = audio_src->nb_samples;
    const int width = nb_samples / 2;
    const float scale = 1.f / sqrt(AUDIO_NBSAMPLES/2 + 1);

    TRACE(ctx, "transform audio filtered frame in %s @ ts=%s into an audio texture",
          av_get_sample_fmt_name(audio_src->format),
          av_ts2timestr(audio_src->pts, &ctx->st_timebase));

    dst_video->pts = audio_src->pts;

    memset(dst_video->data[0], 0, dst_video->height * dst_video->linesize[0]);

    /* Copy waves */
    for (int ch = 0; ch < AUDIO_NBCHANNELS; ch++) {
        const int lz = dst_video->linesize[0];
        float *samples_dst = (float *)(dst_video->data[0] + ch * lz);
        const float *samples_src = (const float *)audio_src->extended_data[ch];

        for (int i = 0; i < width; i++)
            samples_dst[i] = (samples_src[width/2 + i] + 1.f) / 2.f;
    }

    /* Fourier transform */
    for (int ch = 0; ch < AUDIO_NBCHANNELS; ch++) {
        const int lz = dst_video->linesize[0];
        float *fft_dst = (float *)(dst_video->data[0] + (AUDIO_NBCHANNELS + ch) * lz);
        const float *samples_src = (const float *)audio_src->extended_data[ch];
        float *bins = ctx->rdft_data[ch];

        /* Apply window function to input samples */
        for (int i = 0; i < nb_samples; i++)
            bins[i] = samples_src[i] * ctx->window_func_lut[i];

        /* Run transform.
         *
         * After avtx_func(), the bins are an array of successive real and
         * imaginary floats of size NB_SAMPLES/2 + 1.
         */
        ctx->avtx_func(ctx->avtx, bins, bins, sizeof(float));

        /* Get magnitude of frequency bins and copy result into texture
         *
         * Note: since we only have space for N samples in the texture, we skip
         * the last complex (highest frequency one).
         */
#define MAGNITUDE(re, im) (hypotf(re, im) * scale)
        for (int i = 0; i < width; i++)
            fft_dst[i] = MAGNITUDE(bins[2*i], bins[2*i + 1]);
    }

    /* Downscaled versions of the FFT */
    for (int i = 0; i < AUDIO_NBITS-1; i++) {
        for (int ch = 0; ch < AUDIO_NBCHANNELS; ch++) {
            const int lz = dst_video->linesize[0];
            const int source_line = (i + 1)*AUDIO_NBCHANNELS + ch;
            float *fft_src = (float *)(dst_video->data[0] +  source_line                     * lz);
            float *fft_dst = (float *)(dst_video->data[0] + (source_line + AUDIO_NBCHANNELS) * lz);

            const int source_step = 1 << i;
            const int nb_identical_values = source_step << 1;
            const int nb_dest_pixels = width / nb_identical_values;

            TRACE(ctx, "line %2d->%2d: %3d different pixels (copied %3dx) as destination, step source: %d",
                  source_line, source_line + AUDIO_NBCHANNELS, nb_dest_pixels, nb_identical_values, source_step);

            for (int j = 0; j < nb_dest_pixels; j++) {
                const float avg = (fft_src[ j*2      * source_step] +
                                   fft_src[(j*2 + 1) * source_step]) / 2.f;

                for (int x = 0; x < nb_identical_values; x++)
                    fft_dst[j*nb_identical_values + x] = avg;
            }
        }
    }
}

/**
 * Setup the libavfilter filtergraph for user filter but also to have a way to
 * request a pixel format we want, and let libavfilter insert the necessary
 * scaling filter (typically, an automatic conversion from yuv420p to rgb32).
 */
static int setup_filtergraph(struct filtering_ctx *ctx)
{
    int ret = 0;
    char args[512];
    const AVFilter *buffersrc, *buffersink;
    AVFilterInOut *outputs, *inputs;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(ctx->frame_info.format);
    const AVCodecParameters *codecpar = ctx->codecpar;
    const AVRational time_base = ctx->st_timebase;

    avfilter_graph_free(&ctx->filter_graph);

    if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)
        return 0;

    outputs = avfilter_inout_alloc();
    inputs  = avfilter_inout_alloc();

    buffersrc  = avfilter_get_by_name(codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "buffer" : "abuffer");
    buffersink = avfilter_get_by_name(codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "buffersink" : "abuffersink");

    ctx->filter_graph = avfilter_graph_alloc();

    if (!inputs || !outputs || !ctx->filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    av_opt_set_int(ctx->filter_graph, "threads", 1, 0);

    inputs->name  = av_strdup("out");
    outputs->name = av_strdup("in");
    if (!inputs->name || !outputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* create buffer filter source (where we push the frame) */
    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%s:time_base=%d/%d:pixel_aspect=%d/%d",
                 ctx->frame_info.width, ctx->frame_info.height, av_get_pix_fmt_name(ctx->frame_info.format),
                 time_base.num, time_base.den,
                 codecpar->sample_aspect_ratio.num, codecpar->sample_aspect_ratio.den);
    } else {
        snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s",
                 time_base.num, time_base.den, ctx->frame_info.sample_rate,
                 av_get_sample_fmt_name(ctx->frame_info.format));
        char chl[32] = {0};
        av_channel_layout_describe(&ctx->frame_info.channel_layout, chl, sizeof(chl));
        av_strlcatf(args, sizeof(args), ":channel_layout=%s", chl);
    }

    TRACE(ctx, "graph buffer source args: %s", args);

    ret = avfilter_graph_create_filter(&ctx->buffersrc_ctx, buffersrc,
                                       outputs->name, args, NULL, ctx->filter_graph);
    if (ret < 0) {
        LOG(ctx, ERROR, "Unable to create buffer filter source");
        goto end;
    }

    /* create buffer filter sink (where we pull the frame) */
    ret = avfilter_graph_create_filter(&ctx->buffersink_ctx, buffersink,
                                       inputs->name, NULL, NULL, ctx->filter_graph);
    if (ret < 0) {
        LOG(ctx, ERROR, "Unable to create buffer filter sink");
        goto end;
    }

#define SEP(a) ((a)[0] && (a)[strlen(a) - 1] != ';' ? "," : "")

    /* define the output of the graph */
    snprintf(args, sizeof(args), "sws_flags=+full_chroma_int;%s", ctx->filters ? ctx->filters : "");
    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        enum AVPixelFormat sw_pix_fmt = nmdi_pix_fmts_nmd2ff(ctx->sw_pix_fmt);
        if (ctx->sw_pix_fmt == NMD_PIXFMT_AUTO) {
            const enum nmd_pixel_format fmt = nmdi_pix_fmts_ff2nmd(ctx->frame_info.format);
            if (fmt == -1) {
                LOG(ctx, DEBUG, "Unsupported software pixel format: %s, falling back to rgba",
                    av_get_pix_fmt_name(ctx->frame_info.format));
                sw_pix_fmt = AV_PIX_FMT_RGBA;
            } else {
                sw_pix_fmt = ctx->frame_info.format;
            }
        }
        const enum AVPixelFormat pix_fmt = !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL) ? sw_pix_fmt : ctx->frame_info.format;

        if (ctx->max_pixels) {
            int w = codecpar->width, h = codecpar->height;
            nmdi_update_dimensions(&w, &h, ctx->max_pixels);
            av_strlcatf(args, sizeof(args),
                        "%sscale=%d:%d:force_original_aspect_ratio=decrease",
                        SEP(args), w, h);
        }

        av_strlcatf(args, sizeof(args), "%sformat=%s, settb=tb=%d/%d", SEP(args), av_get_pix_fmt_name(pix_fmt),
                    time_base.num, time_base.den);
    } else if (ctx->audio_texture) {
        av_strlcatf(args, sizeof(args), "%saformat=sample_fmts=fltp:channel_layouts=stereo, asetnsamples=%d, asettb=tb=%d/%d",
                    SEP(args), AUDIO_NBSAMPLES, time_base.num, time_base.den);
    } else {
        av_strlcatf(args, sizeof(args), "%saformat=sample_fmts=flt:channel_layouts=stereo, asettb=tb=%d/%d",
                    SEP(args), time_base.num, time_base.den);
    }

    TRACE(ctx, "graph buffer sink args: %s", args);

    /* create our filter graph */
    inputs->filter_ctx  = ctx->buffersink_ctx;
    outputs->filter_ctx = ctx->buffersrc_ctx;

    ret = avfilter_graph_parse_ptr(ctx->filter_graph, args, &inputs, &outputs, NULL);
    if (ret < 0)
        goto end;

    ret = avfilter_graph_config(ctx->filter_graph, NULL);
    if (ret < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

static AVFrame *get_audio_frame(void)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return NULL;

    frame->format = AV_PIX_FMT_RGB32;

    frame->width  = AUDIO_NBSAMPLES/2;      // samples are float (32 bits), pix fmt is rgb32 (32 bits as well)
    /* height:
     *   AUDIO_NBCHANNELS (waves lines)
     * + AUDIO_NBCHANNELS (fft lines of width AUDIO_NBCHANNELS/2, or 1<<(AUDIO_NBITS-1))
     * + AUDIO_NBITS-1 AUDIO_NBCHANNELS (fft lines downscaled) */
    frame->height = (1 + AUDIO_NBITS) * AUDIO_NBCHANNELS;

    if (av_frame_get_buffer(frame, 16) < 0) {
        av_frame_free(&frame);
        return NULL;
    }

    memset(frame->data[0], 0, frame->height * frame->linesize[0]);

    return frame;
}

static char *update_filters_str(char *filters, const char *append)
{
    char *str;

    if (filters) {
        str = av_asprintf("%s,%s", filters, append);
        av_free(filters);
    } else {
        str = av_strdup(append);
    }
    return str;
}

int nmdi_filtering_init(void *log_ctx,
                        struct filtering_ctx *ctx,
                        AVThreadMessageQueue *in_queue,
                        AVThreadMessageQueue *out_queue,
                        const AVStream *stream,
                        const AVCodecContext *avctx,
                        double media_rotation,
                        struct nmdi_loop *loop,
                        const struct nmdi_opts *o)
{
    ctx->log_ctx = log_ctx;
    ctx->in_queue  = in_queue;
    ctx->out_queue = out_queue;
    ctx->loop = loop;
    ctx->sw_pix_fmt = o->sw_pix_fmt;
    ctx->max_pixels = o->max_pixels;
    ctx->audio_texture = o->audio_texture;
    ctx->st_timebase = stream->time_base;

    int ret = avcodec_parameters_from_context(ctx->codecpar, avctx);
    if (ret < 0)
        return ret;

    if (ctx->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->audio_texture) {
        /* Pre-calc windowing function */
        ctx->window_func_lut = av_malloc_array(AUDIO_NBSAMPLES, sizeof(*ctx->window_func_lut));
        if (!ctx->window_func_lut)
            return AVERROR(ENOMEM);
        for (int i = 0; i < AUDIO_NBSAMPLES; i++)
            ctx->window_func_lut[i] = .5f * (1 - cos(2*M_PI*i / (AUDIO_NBSAMPLES-1)));

        /* Real Discrete Fourier Transform context (Real to Complex) */
        const float scale = 1.f;
        ret = av_tx_init(&ctx->avtx, &ctx->avtx_func, AV_TX_FLOAT_RDFT, 0, AUDIO_NBSAMPLES, &scale, AV_TX_INPLACE);
        if (ret < 0) {
            LOG(ctx, ERROR, "Unable to init AVTX context with N=%d", AUDIO_NBITS);
            return ret;
        }

        ctx->rdft_data[0] = av_calloc(AUDIO_NBSAMPLES + 2, sizeof(*ctx->rdft_data[0]));
        ctx->rdft_data[1] = av_calloc(AUDIO_NBSAMPLES + 2, sizeof(*ctx->rdft_data[1]));
        if (!ctx->rdft_data[0] || !ctx->rdft_data[1])
            return AVERROR(ENOMEM);
    }

    /* Audio textures need whole FFT windows, so they keep a graph per pass. */
    if (ctx->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !ctx->audio_texture) {
        ctx->continuous_audio = 1;
        ctx->pass_markers = av_fifo_alloc2(4, sizeof(struct pass_marker), AV_FIFO_FLAG_AUTO_GROW);
        if (!ctx->pass_markers)
            return AVERROR(ENOMEM);
    }

    if (o->filters) {
        ctx->filters = av_strdup(o->filters);
        if (!ctx->filters)
            return AVERROR(ENOMEM);
    }

    if (ctx->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && o->autorotate) {
        if (fabs(media_rotation - 90) < 1.0)
            ctx->filters = update_filters_str(ctx->filters, "transpose=clock");
        else if (fabs(media_rotation - 180) < 1.0)
            ctx->filters = update_filters_str(ctx->filters, "vflip,hflip");
        else if (fabs(media_rotation - 270) < 1.0)
            ctx->filters = update_filters_str(ctx->filters, "transpose=cclock");
        TRACE(ctx, "update filtergraph to: %s", ctx->filters);
    }

    return 0;
}

/* End timestamp in the stream time base, or AV_NOPTS_VALUE if unknown. */
static int64_t get_frame_end(const struct filtering_ctx *ctx, const AVFrame *frame)
{
    if (frame->pts == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;

    if (frame->nb_samples > 0 && frame->sample_rate > 0)
        return frame->pts + av_rescale_q(frame->nb_samples, (AVRational){1, frame->sample_rate}, ctx->st_timebase);
    return frame->duration > 0 ? frame->pts + frame->duration : AV_NOPTS_VALUE;
}

static int send_frame(struct filtering_ctx *ctx, AVFrame *frame)
{
    int ret;
    struct message msg = {
        .type = MSG_FRAME,
        .frame.frame = frame,
    };
    int64_t end_pts = get_frame_end(ctx, frame);
    if (end_pts != AV_NOPTS_VALUE)
        end_pts += ctx->out.pts_offset;

    TRACE(ctx, "sending filtered frame to the sink");
    ret = av_thread_message_queue_send(ctx->out_queue, &msg, 0);
    if (ret < 0) {
        if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
            LOG(ctx, ERROR, "unable to send frame: %s", av_err2str(ret));
    } else {
        ctx->out.has_frame = 1;
        ctx->out.end_pts = FFMAX(ctx->out.end_pts, end_pts);
    }

    return ret;
}

static int push_frame(struct filtering_ctx *ctx, AVFrame *inframe)
{
    int ret;

    TRACE(ctx, "pushing frame %p into filtergraph", inframe);

    ret = av_buffersrc_write_frame(ctx->buffersrc_ctx, inframe);
    if (ret < 0) {
        LOG(ctx, ERROR, "unable to push frame into filtergraph: %s", av_err2str(ret));
        return ret;
    }

    if (inframe) {
        ctx->in.has_frame = 1;
        ctx->in.end_pts = FFMAX(ctx->in.end_pts, get_frame_end(ctx, inframe));
    }

    return 0;
}

static int pull_frame(struct filtering_ctx *ctx, AVFrame *outframe)
{
    int ret;
    const int do_audio_texture = ctx->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->audio_texture;
    AVFrame *filtered_frame = outframe;

    TRACE(ctx, "pulling frame from filtergraph");

    if (do_audio_texture) {
        filtered_frame = av_frame_alloc();
        if (!filtered_frame)
            return AVERROR(ENOMEM);
    }

    ret = av_buffersink_get_frame(ctx->buffersink_ctx, filtered_frame);
    if (ret < 0) {
        if (do_audio_texture)
            av_frame_free(&filtered_frame);
        if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN))
            LOG(ctx, ERROR, "unable to pull frame from filtergraph: %s", av_err2str(ret));
        return ret;
    }

    TRACE(ctx, "filtered %s %s frame @ ts=%s",
          av_get_media_type_string(ctx->codecpar->codec_type),
          ctx->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? av_get_pix_fmt_name(filtered_frame->format)
                                                          : av_get_sample_fmt_name(filtered_frame->format),
          av_ts2timestr(filtered_frame->pts, &ctx->st_timebase));

    if (do_audio_texture) {
        AVFrame *audio_texture_frame = get_audio_frame();
        audio_frame_to_sound_texture(ctx, audio_texture_frame, filtered_frame);
        av_frame_free(&filtered_frame);
        av_frame_move_ref(outframe, audio_texture_frame);
        av_free(audio_texture_frame);
    }

    outframe->opaque = (void *)(intptr_t)ctx->out.pass_number;

    return 0;
}

static AVFrame *copy_samples(const struct filtering_ctx *ctx, const AVFrame *src, int offset, int nb_samples)
{
    AVFrame *dst = av_frame_alloc();
    if (!dst)
        return NULL;
    dst->format = src->format;
    dst->sample_rate = src->sample_rate;
    dst->nb_samples = nb_samples;
    if (av_channel_layout_copy(&dst->ch_layout, &src->ch_layout) < 0 ||
        av_frame_get_buffer(dst, 0) < 0 ||
        av_frame_copy_props(dst, src) < 0) {
        av_frame_free(&dst);
        return NULL;
    }
    av_samples_copy(dst->extended_data, src->extended_data, 0, offset, nb_samples,
                    src->ch_layout.nb_channels, src->format);
    const AVRational sample_tb = {1, src->sample_rate};
    dst->pts = src->pts + av_rescale_q(offset, sample_tb, ctx->st_timebase);
    dst->duration = av_rescale_q(nb_samples, sample_tb, ctx->st_timebase);
    return dst;
}

static int enter_pass(struct filtering_ctx *ctx, const struct pass_marker *marker)
{
    ctx->out.pass_number = marker->pass_number;
    ctx->out.has_frame = 0;
    ctx->out.pts_offset = marker->sample_pts_offset;
    if (!marker->forward)
        return 0;
    struct message msg = {
        .type = MSG_PASS,
        .pass.number = marker->pass_number,
    };
    return av_thread_message_queue_send(ctx->out_queue, &msg, 0);
}

/*
 * Filters can return a frame spanning several passes. Split it at each pass
 * boundary and restore the media timestamps.
 *
 * Count samples because the stream time base may be coarser than one sample.
 * Filters must preserve sample count, adjusted for the output sample rate.
 */
static int send_audio_frame(struct filtering_ctx *ctx, AVFrame *frame)
{
    struct pass_marker marker;
    while (av_fifo_peek(ctx->pass_markers, &marker, 1, 0) >= 0) {
        const int64_t start_sample = av_rescale(marker.sample_index, frame->sample_rate, ctx->frame_info.sample_rate);
        const int64_t nb_samples = start_sample - ctx->out.sample_count;
        if (nb_samples >= frame->nb_samples)
            break;

        if (nb_samples > 0) {
            AVFrame *head = copy_samples(ctx, frame, 0, nb_samples);
            AVFrame *tail = copy_samples(ctx, frame, nb_samples, frame->nb_samples - nb_samples);
            if (!head || !tail) {
                av_frame_free(&head);
                av_frame_free(&tail);
                return AVERROR(ENOMEM);
            }
            head->pts -= ctx->out.pts_offset;
            head->opaque = (void *)(intptr_t)ctx->out.pass_number;
            int ret = send_frame(ctx, head);
            if (ret < 0) {
                av_frame_free(&head);
                av_frame_free(&tail);
                return ret;
            }
            ctx->out.sample_count += nb_samples;
            tail->pts = marker.sample_pts;
            av_frame_unref(frame);
            av_frame_move_ref(frame, tail);
            av_frame_free(&tail);
        }

        av_fifo_drain2(ctx->pass_markers, 1);
        if (ctx->out.pass_number && !ctx->out.has_frame && nmdi_loop_report_empty_pass(ctx->loop, ctx->out.pass_number)) {
            LOG(ctx, WARNING, "No filtered frame in the loop range, ending playback");
            avfilter_graph_free(&ctx->filter_graph);
            return AVERROR_EOF;
        }
        int ret = enter_pass(ctx, &marker);
        if (ret < 0)
            return ret;
    }

    frame->pts -= ctx->out.pts_offset;
    frame->opaque = (void *)(intptr_t)ctx->out.pass_number;
    const int64_t nb_samples = frame->nb_samples;
    const int ret = send_frame(ctx, frame);
    if (ret >= 0)
        ctx->out.sample_count += nb_samples;
    return ret;
}

static int pull_send_frame(struct filtering_ctx *ctx)
{
    int ret;

    AVFrame *filtered_frame = av_frame_alloc();
    if (!filtered_frame)
        return AVERROR(ENOMEM);

    ret = pull_frame(ctx, filtered_frame);

    if (ret < 0) {
        av_frame_free(&filtered_frame);
        return ret;
    }

    ret = ctx->continuous_audio ? send_audio_frame(ctx, filtered_frame) : send_frame(ctx, filtered_frame);
    if (ret < 0) {
        av_frame_free(&filtered_frame);
        return ret;
    }

    return 0;
}

static int check_playback_end(struct filtering_ctx *ctx, const AVFrame *frame)
{
    int64_t playback_start, playback_end;
    nmdi_loop_get_playback_range(ctx->loop, &playback_start, &playback_end);
    if (playback_end <= 0)
        return 0;
    if (frame->pts <= av_rescale_q(playback_end, AV_TIME_BASE_Q, ctx->st_timebase))
        return 0;
    nmdi_loop_set_playback_stopped_at(ctx->loop, playback_end);
    return 1;
}

static int flush_frames(struct filtering_ctx *ctx);

static int start_pass(struct filtering_ctx *ctx, int64_t pass)
{
    if (ctx->filters || ctx->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        int ret = flush_frames(ctx);
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
        avfilter_graph_free(&ctx->filter_graph);
        reset_frame_info(&ctx->frame_info);
    }
    /* Pass 0 can be empty after a seek; only stop after an empty full pass. */
    if (ctx->out.pass_number && !ctx->out.has_frame && nmdi_loop_report_empty_pass(ctx->loop, ctx->out.pass_number)) {
        LOG(ctx, WARNING, "No filtered frame in the loop range, ending playback");
        return AVERROR_EOF;
    }
    reset_filtering_io(&ctx->in, pass);
    reset_filtering_io(&ctx->out, pass);
    return 0;
}

static int flush_frames(struct filtering_ctx *ctx)
{
    int ret;

    if (!ctx->filter_graph)
        return 0;

    TRACE(ctx, "push null frame into %s filtergraph to trigger flushing",
          av_get_media_type_string(ctx->codecpar->codec_type));

    ret = push_frame(ctx, NULL);
    if (ret < 0)
        return ret;

    do {
        ret = pull_send_frame(ctx);
    } while (ret >= 0);

    return ret;
}

static void reset_audio_timeline(struct filtering_ctx *ctx)
{
    reset_filtering_io(&ctx->in, ctx->out.pass_number);
    ctx->in_pending_pass_number = -1;
    ctx->out.pts_offset = 0;
    ctx->out.end_pts = AV_NOPTS_VALUE;
    ctx->out.sample_count = 0;
    av_fifo_reset2(ctx->pass_markers);
}

static int start_audio_pass(struct filtering_ctx *ctx, int64_t pass, int64_t offset, int forward)
{
    const struct pass_marker marker = {
        .pass_number       = pass,
        .sample_index      = ctx->in.sample_count,
        .sample_pts        = ctx->in.end_pts,
        .sample_pts_offset = offset,
        .forward           = forward,
    };
    ctx->in.pass_number = pass;
    ctx->in.pts_offset = offset;
    ctx->in.has_frame = 0;
    return av_fifo_write(ctx->pass_markers, &marker, 1);
}

static int announce_audio_pass(struct filtering_ctx *ctx, int64_t pass)
{
    if (ctx->in_pending_pass_number >= 0) {
        int ret = start_audio_pass(ctx, ctx->in_pending_pass_number, ctx->in.pts_offset, 1);
        if (ret < 0)
            return ret;
    }
    ctx->in_pending_pass_number = pass;
    return 0;
}

static int forward_audio_passes(struct filtering_ctx *ctx, int announced)
{
    if (announced && ctx->in_pending_pass_number >= 0) {
        int ret = start_audio_pass(ctx, ctx->in_pending_pass_number, ctx->in.pts_offset, 1);
        if (ret < 0)
            return ret;
        ctx->in_pending_pass_number = -1;
    }
    struct pass_marker marker;
    while (av_fifo_read(ctx->pass_markers, &marker, 1) >= 0) {
        int ret = enter_pass(ctx, &marker);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/*
 * Keep the audio graph across passes to avoid refilling filter buffers at
 * every loop boundary, which can stall playback.
 *
 * Offset input timestamps so each pass follows the previous one in the graph.
 * send_audio_frame() splits output at pass boundaries and restores media time.
 *
 *   media      |----[==== loop ====)----|
 *   pushed     |pass 0 ---------------)[pass 1 ------------)[pass 2 ------- ...
 *   offset      0                       in.end_pts - frame->pts
 */
static int push_audio_frame(struct filtering_ctx *ctx, AVFrame *frame)
{
    int ret = 0;
    const int64_t pass = (intptr_t)frame->opaque;
    if (pass != ctx->in.pass_number) {
        if (ctx->in_pending_pass_number >= 0 && ctx->in_pending_pass_number != pass)
            ret = start_audio_pass(ctx, ctx->in_pending_pass_number, ctx->in.pts_offset, 1);
        if (ret >= 0) {
            const int64_t offset = ctx->in.end_pts == AV_NOPTS_VALUE ? 0 : ctx->in.end_pts - frame->pts;
            ret = start_audio_pass(ctx, pass, offset, ctx->in_pending_pass_number == pass);
        }
        ctx->in_pending_pass_number = -1;
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }
    }

    frame->pts += ctx->in.pts_offset;
    ctx->in.sample_count += frame->nb_samples;

    ret = push_frame(ctx, frame);
    av_frame_free(&frame);
    if (ret < 0)
        return ret;

    do {
        ret = pull_send_frame(ctx);
    } while (ret >= 0);
    return ret == AVERROR(EAGAIN) ? 0 : ret;
}

void nmdi_filtering_run(struct filtering_ctx *ctx)
{
    int ret;
    int in_err, out_err;

    TRACE(ctx, "filtering packets from %p into %p", ctx->in_queue, ctx->out_queue);

    /* Rebuild the graph for the first frame after a restart. */
    reset_frame_info(&ctx->frame_info);
    reset_filtering_io(&ctx->in, 0);
    reset_filtering_io(&ctx->out, 0);
    if (ctx->continuous_audio)
        reset_audio_timeline(ctx);

    for (;;) {
        AVFrame *frame;
        struct message msg;

        TRACE(ctx, "fetching a frame from the inqueue");
        ret = av_thread_message_queue_recv(ctx->in_queue, &msg, 0);
        if (ret < 0) {
            if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
                LOG(ctx, ERROR, "unable to fetch a frame from the inqueue: %s", av_err2str(ret));
            break;
        }

        if (msg.type == MSG_SEEK) {
            TRACE(ctx, "message is a seek, destroy filtergraph and forward message to out queue");
            avfilter_graph_free(&ctx->filter_graph);
            reset_frame_info(&ctx->frame_info);
            reset_filtering_io(&ctx->in, 0);
            reset_filtering_io(&ctx->out, 0);
            if (ctx->continuous_audio)
                reset_audio_timeline(ctx);
            av_thread_message_flush(ctx->out_queue);
            ret = av_thread_message_queue_send(ctx->out_queue, &msg, 0);
            if (ret < 0)
                break;
            continue;
        }

        if (msg.type == MSG_PASS && ctx->continuous_audio) {
            ret = announce_audio_pass(ctx, msg.pass.number);
            if (ret < 0)
                break;
            continue;
        }

        /* The boundary must follow all frames from the previous pass. */
        if (msg.type == MSG_PASS) {
            ret = start_pass(ctx, msg.pass.number);
            if (ret < 0)
                break;
            ret = av_thread_message_queue_send(ctx->out_queue, &msg, 0);
            if (ret < 0)
                break;
            continue;
        }

        frame = msg.frame.frame;

        TRACE(ctx, "filtering %s %s frame @ ts=%s",
              av_get_media_type_string(ctx->codecpar->codec_type),
              ctx->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? av_get_pix_fmt_name(frame->format)
                                                              : av_get_sample_fmt_name(frame->format),
              av_ts2timestr(frame->pts, &ctx->st_timebase));

        if (!ctx->continuous_audio && (intptr_t)frame->opaque != ctx->out.pass_number) {
            ret = start_pass(ctx, (intptr_t)frame->opaque);
            if (ret < 0) {
                av_frame_free(&frame);
                break;
            }
        }

        struct frame_info *frame_info = &ctx->frame_info;
        if (frame_info->format != frame->format ||
            frame_info->width != frame->width ||
            frame_info->height != frame->height ||
            frame_info->sample_rate != frame->sample_rate ||
            av_channel_layout_compare(&frame_info->channel_layout, &frame->ch_layout)) {
            frame_info->format = frame->format;
            frame_info->width = frame->width;
            frame_info->height = frame->height;
            frame_info->sample_rate = frame->sample_rate;
            av_channel_layout_copy(&frame_info->channel_layout, &frame->ch_layout);

            ret = setup_filtergraph(ctx);
            if (ret < 0) {
                av_frame_free(&frame);
                break;
            }

            if (ctx->continuous_audio) {
                ret = forward_audio_passes(ctx, 0);
                if (ret < 0) {
                    av_frame_free(&frame);
                    break;
                }
                const int64_t in_pending_pass_number = ctx->in_pending_pass_number;
                reset_audio_timeline(ctx);
                ctx->in_pending_pass_number = in_pending_pass_number;
            }
        }

        // TODO: replace with a trim filter in libavfilter (check if hw accelerated
        // filters work)
        if (frame->pts < 0) {
            av_frame_free(&frame);
            TRACE(ctx, "frame ts is negative, skipping");
            continue;
        } else if (check_playback_end(ctx, frame)) {
            av_frame_free(&frame);
            TRACE(ctx, "reached trim duration");
            ret = AVERROR_EXIT; // skip draining at the playback end
            break;
        }

        if (ctx->continuous_audio && ctx->filter_graph) {
            ret = push_audio_frame(ctx, frame);
            if (ret < 0)
                break;
        } else if (!ctx->filter_graph) {
            ret = send_frame(ctx, frame);
            if (ret < 0) {
                av_frame_free(&frame);
                break;
            }
            ctx->in.has_frame = 1;
            ctx->in.end_pts = ctx->out.end_pts;
        } else {
            ret = push_frame(ctx, frame);
            av_frame_free(&frame);
            if (ret < 0)
                break;

            ret = pull_send_frame(ctx);
            if (ret < 0 && ret != AVERROR(EAGAIN))
                break;
        }
    }

    if (ret == AVERROR_EOF) {
        ret = flush_frames(ctx);
        if (ctx->continuous_audio && ret == AVERROR_EOF) {
            const int err = forward_audio_passes(ctx, 1);
            if (err < 0)
                ret = err;
        }
    }

    if (ret < 0 && ret != AVERROR_EOF) {
        in_err = out_err = ret;
    } else {
        in_err = AVERROR_EXIT;
        out_err = AVERROR_EOF;
    }
    TRACE(ctx, "notify decoder with %s and sink with %s",
          av_err2str(in_err), av_err2str(out_err));
    av_thread_message_queue_set_err_send(ctx->in_queue,  in_err);
    av_thread_message_flush(ctx->in_queue);
    av_thread_message_queue_set_err_recv(ctx->out_queue, out_err);
}

void nmdi_filtering_free(struct filtering_ctx **fp)
{
    struct filtering_ctx *ctx = *fp;

    if (!ctx)
        return;

    if (ctx->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->audio_texture) {
        av_freep(&ctx->window_func_lut);
        av_freep(&ctx->rdft_data[0]);
        av_freep(&ctx->rdft_data[1]);
        av_tx_uninit(&ctx->avtx);
    }
    avfilter_graph_free(&ctx->filter_graph);
    av_fifo_freep2(&ctx->pass_markers);
    avcodec_parameters_free(&ctx->codecpar);
    av_freep(&ctx->filters);
    reset_frame_info(&ctx->frame_info);
    av_freep(fp);
}
