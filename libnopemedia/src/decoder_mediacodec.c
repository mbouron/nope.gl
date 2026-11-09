/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

#include <stdbool.h>
#include <stdio.h>

#include <libavutil/attributes.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>

#include "decoders.h"
#include "internal.h"
#include "log.h"
#include "mediacodec.h"
#include "mod_decoding.h"

#define OUTPUT_TIMEOUT_US 10000
#define STALL_TIMEOUT_US  2000000

/*
 * Queue sizes do not include frames held elsewhere. Allow three for decoding,
 * one for filtering, two for frame selection, and four for the caller.
 */
#define PIPELINE_IMAGES 6
#define CALLER_IMAGES   4
/* The decoder also needs free buffers in the image reader's 64-slot queue */
#define MAX_IMAGES      32

struct mcdec_context {
    AVCodecParameters *par;
    struct nmdi_mediacodec *mc;
};

static av_printf_format(3, 0) void log_callback(void *log_ctx, int level, const char *fmt, va_list vl)
{
    const int nmd_level = level <= AV_LOG_ERROR   ? NMD_LOG_ERROR
                        : level <= AV_LOG_WARNING ? NMD_LOG_WARNING
                        : level <= AV_LOG_INFO    ? NMD_LOG_INFO
                        : level <= AV_LOG_VERBOSE ? NMD_LOG_VERBOSE
                        : NMD_LOG_DEBUG;
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    nmdi_log_print(log_ctx, nmd_level, __FILE__, __LINE__, "mediacodec", "%s", buf);
}

static int init(struct decoder_ctx *ctx, const struct nmdi_opts *opts, bool software)
{
    struct mcdec_context *mcdec = ctx->priv_data;

    mcdec->par = avcodec_parameters_alloc();
    if (!mcdec->par)
        return AVERROR(ENOMEM);
    int ret = avcodec_parameters_from_context(mcdec->par, ctx->avctx);
    if (ret < 0)
        return ret;

    const int max_images = opts->max_nb_frames + opts->max_nb_sink + PIPELINE_IMAGES + CALLER_IMAGES;
    const struct nmdi_mediacodec_params params = {
        .par        = mcdec->par,
        .time_base  = ctx->st_timebase,
        .software   = software,
        .max_images = FFMIN(max_images, MAX_IMAGES),
        .log_ctx    = ctx->log_ctx,
        .log        = log_callback,
    };
    return nmdi_mediacodec_create(&mcdec->mc, &params);
}

static int mcdec_init(struct decoder_ctx *ctx, const struct nmdi_opts *opts)
{
    return init(ctx, opts, false);
}

static int mcdec_init_sw(struct decoder_ctx *ctx, const struct nmdi_opts *opts)
{
    return init(ctx, opts, true);
}

static int queue_next_frame(struct decoder_ctx *ctx, int64_t timeout_us)
{
    struct mcdec_context *mcdec = ctx->priv_data;

    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);
    int ret = nmdi_mediacodec_receive(mcdec->mc, frame, timeout_us);
    if (ret >= 0)
        ret = nmdi_decoding_queue_frame(ctx->decoding_ctx, frame);
    if (ret < 0)
        av_frame_free(&frame);
    return ret;
}

/* Count waits across retries so a stalled codec cannot return EAGAIN indefinitely */
static int wait_next_frame(struct decoder_ctx *ctx, int64_t *waited_us)
{
    struct mcdec_context *mcdec = ctx->priv_data;

    int ret = queue_next_frame(ctx, OUTPUT_TIMEOUT_US);
    if (ret != AVERROR(EAGAIN)) {
        *waited_us = 0;
        return ret;
    }
    *waited_us += OUTPUT_TIMEOUT_US;
    if (*waited_us >= STALL_TIMEOUT_US) {
        *waited_us = 0;
        ret = nmdi_mediacodec_reset(mcdec->mc, "the decoder is stalled");
        if (ret < 0)
            return ret;
    }
    return AVERROR(EAGAIN);
}

static int mcdec_push_packet(struct decoder_ctx *ctx, const AVPacket *pkt)
{
    struct mcdec_context *mcdec = ctx->priv_data;
    int64_t waited_us = 0;
    int ret;

    if (!pkt || !pkt->size) {
        ret = nmdi_mediacodec_send(mcdec->mc, NULL);
        while (ret >= 0 || ret == AVERROR(EAGAIN))
            ret = wait_next_frame(ctx, &waited_us);
        if (ret != AVERROR_EOF)
            return ret;
        return nmdi_decoding_queue_frame(ctx->decoding_ctx, NULL);
    }

    for (;;) {
        ret = nmdi_mediacodec_send(mcdec->mc, pkt);
        if (ret != AVERROR(EAGAIN))
            break;
        ret = wait_next_frame(ctx, &waited_us);
        if (ret < 0 && ret != AVERROR(EAGAIN))
            return ret;
    }
    if (ret < 0)
        return ret;

    do {
        ret = queue_next_frame(ctx, 0);
    } while (ret >= 0);
    return ret == AVERROR(EAGAIN) ? 0 : ret;
}

static void mcdec_flush(struct decoder_ctx *ctx)
{
    struct mcdec_context *mcdec = ctx->priv_data;
    nmdi_mediacodec_flush(mcdec->mc);
}

static void mcdec_uninit(struct decoder_ctx *ctx)
{
    struct mcdec_context *mcdec = ctx->priv_data;
    nmdi_mediacodec_freep(&mcdec->mc);
    avcodec_parameters_free(&mcdec->par);
}

const struct decoder nmdi_decoder_mediacodec = {
    .name           = "mediacodec",
    .init           = mcdec_init,
    .uninit         = mcdec_uninit,
    .push_packet    = mcdec_push_packet,
    .flush          = mcdec_flush,
    .priv_data_size = sizeof(struct mcdec_context),
};

const struct decoder nmdi_decoder_mediacodec_sw = {
    .name           = "mediacodec_sw",
    .init           = mcdec_init_sw,
    .uninit         = mcdec_uninit,
    .push_packet    = mcdec_push_packet,
    .flush          = mcdec_flush,
    .priv_data_size = sizeof(struct mcdec_context),
};
