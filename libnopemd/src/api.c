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

#include <stdint.h>
#include <float.h> /* for DBL_MAX */
#include <math.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>

#include "nopemd.h"
#include "async.h"
#include "log.h"
#include "internal.h"

#if HAVE_MEDIACODEC_HWACCEL
#include <unistd.h>

#include "mediacodec.h"
#endif

struct nmd_ctx {
    const AVClass *class;                   // necessary for the AVOption mechanism
    struct log_ctx *log_ctx;
    char *filename;                         // input filename
    char *logname;

    struct nmdi_opts opts;

    struct async_context *actx;
    int context_configured;

    AVFrame *cached_frame;

    /*
     * Media time targeted by the last seek, from `nmd_seek()` or
     * `nmd_get_frame()`, in `AV_TIME_BASE_Q` units. Reset to `AV_NOPTS_VALUE` by
     * `nmd_stop()`, and when `ret_frame()` gets a frame, even one not returned
     * again to the caller.
     */
    int64_t seek_time64;

    AVRational st_timebase;                 // stream timebase
    struct nmdi_pass_timeline pass_timeline;

    double loop_start;                     // loop range requested by the user, in seconds relative to `start_time`
    double loop_end;
    int64_t loop_start64;                   // loop range in media time, in microseconds; `AV_NOPTS_VALUE` without a loop
    int64_t loop_end64;
    int64_t loop_rev;                       // revision of the requested range (see `nmdi_loop`)

    /* These timestamps include the pass offset, in stream ticks (see `get_frame_pos()`). */
    int64_t last_pushed_frame_ts;           // ts value of the latest pushed frame (it acts as a UID)
    int64_t last_pushed_frame_pass;
    int64_t last_frame_poped_ts;
    int64_t first_ts;
    int64_t last_ts;
    int eof; // set if the latest frame returned was NULL and meant EOF

    int64_t entering_time;
    const char *cur_func_name;
};

static int averror_to_nmd_err(int averror)
{
    if (averror >= 0)
        return NMD_RET_SUCCESS;
    else if (averror == AVERROR_EOF)
        return NMD_ERR_EOF;
    else if (averror == AVERROR(ENOMEM))
        return NMD_ERR_MEMORY;
    else
        return NMD_ERR_GENERIC;
}

#define OFFSET(x) offsetof(struct nmd_ctx, opts.x)
static const AVOption options[] = {
    { "avselect",               NULL, OFFSET(avselect),               AV_OPT_TYPE_INT,       {.i64=NMD_SELECT_VIDEO}, 0, NB_NMD_MEDIA_SELECTION-1 },
    { "start_time",             NULL, OFFSET(start_time),             AV_OPT_TYPE_DOUBLE,    {.dbl= 0},      0, DBL_MAX },
    { "end_time",               NULL, OFFSET(end_time),               AV_OPT_TYPE_DOUBLE,    {.dbl=-1},     -1, DBL_MAX },
    { "dist_time_seek_trigger", NULL, OFFSET(dist_time_seek_trigger), AV_OPT_TYPE_DOUBLE,    {.dbl=1.5},    -1, DBL_MAX },
    { "max_nb_packets",         NULL, OFFSET(max_nb_packets),         AV_OPT_TYPE_INT,       {.i64=5},       1, 100 },
    { "max_nb_frames",          NULL, OFFSET(max_nb_frames),          AV_OPT_TYPE_INT,       {.i64=2},       1, 100 },
    { "max_nb_sink",            NULL, OFFSET(max_nb_sink),            AV_OPT_TYPE_INT,       {.i64=2},       1, 100 },
    { "filters",                NULL, OFFSET(filters),                AV_OPT_TYPE_STRING,    {.str=NULL},    0,       0 },
    { "sw_pix_fmt",             NULL, OFFSET(sw_pix_fmt),             AV_OPT_TYPE_INT,       {.i64=NMD_PIXFMT_BGRA},  0, INT_MAX },
    { "autorotate",             NULL, OFFSET(autorotate),             AV_OPT_TYPE_INT,       {.i64=0},       0, 1 },
    { "auto_hwaccel",           NULL, OFFSET(auto_hwaccel),           AV_OPT_TYPE_INT,       {.i64=1},       0, 1 },
    { "thread_stack_size",      NULL, OFFSET(thread_stack_size),      AV_OPT_TYPE_INT,       {.i64=0},       0, INT_MAX },
    { "opaque",                 NULL, OFFSET(opaque),                 AV_OPT_TYPE_BINARY,    {.str=NULL},    0, 0 },
    { "max_pixels",             NULL, OFFSET(max_pixels),             AV_OPT_TYPE_INT,       {.i64=0},       0, INT_MAX },
    { "audio_texture",          NULL, OFFSET(audio_texture),          AV_OPT_TYPE_INT,       {.i64=1},       0, 1 },
    { "vt_pix_fmt",             NULL, OFFSET(vt_pix_fmt),             AV_OPT_TYPE_STRING,    {.str="bgra"},  0, 0 },
    { "stream_idx",             NULL, OFFSET(stream_idx),             AV_OPT_TYPE_INT,       {.i64=-1},     -1, INT_MAX },
    { "use_pkt_duration",       NULL, OFFSET(use_pkt_duration),       AV_OPT_TYPE_INT,       {.i64=1},       0, 1 },
    { NULL }
};

static const char *item_name(void *arg)
{
    const struct nmd_ctx *s = arg;
    return s->logname;
}

static const AVClass class = {
    .class_name = "nope.media",
    .item_name  = item_name,
    .option     = options,
};

int nmd_set_option(struct nmd_ctx *s, const char *key, ...)
{
    va_list ap;
    int n, ret = 0;
    double d;
    char *str;
    void *ptr;
    const AVOption *o = av_opt_find(s, key, NULL, 0, 0);

    va_start(ap, key);

    if (!o) {
        LOG(s, ERROR, "Option '%s' not found", key);
        ret = AVERROR(EINVAL);
        goto end;
    }

    if (s->context_configured) {
        LOG(s, ERROR, "Context is already configured, can not set option '%s'", key);
        ret = AVERROR(EINVAL);
        goto end;
    }

    switch (o->type) {
    case AV_OPT_TYPE_INT:
        n = va_arg(ap, int);
        ret = av_opt_set_int(s, key, n, 0);
        break;
    case AV_OPT_TYPE_DOUBLE:
        d = va_arg(ap, double);
        ret = av_opt_set_double(s, key, d, 0);
        break;
    case AV_OPT_TYPE_STRING:
        str = va_arg(ap, char *);
        ret = av_opt_set(s, key, str, 0);
        break;
    case AV_OPT_TYPE_BINARY:
        ptr = va_arg(ap, void *);
        ret = av_opt_set_bin(s, key, ptr, sizeof(ptr), 0);
        break;
    default:
        av_assert0(0);
    }

end:
    va_end(ap);
    return averror_to_nmd_err(ret);
}

static void free_context(struct nmd_ctx *s)
{
    if (!s)
        return;
    av_freep(&s->filename);
    av_freep(&s->logname);
    nmdi_log_free(&s->log_ctx);
    av_opt_free(s);
    av_freep(&s);
}

/* Destroy data allocated by configure_context() */
static void free_temp_context_data(struct nmd_ctx *s)
{
    TRACE(s, "free temporary context data");

    av_frame_free(&s->cached_frame);

    nmdi_async_free(&s->actx);

    s->context_configured = 0;
}

void nmd_set_log_callback(struct nmd_ctx *s, void *arg, nmd_log_callback_type callback)
{
    nmdi_log_set_callback(s->log_ctx, arg, callback);
}

int nmd_set_seek_callback(struct nmd_ctx *s, void *arg, nmd_seek_callback_type callback)
{
    if (s->context_configured) {
        LOG(s, ERROR, "Context is already configured, can not set the seek callback");
        return AVERROR(EINVAL);
    }

    s->opts.seek_callback = callback;
    s->opts.seek_callback_arg = arg;
    return 0;
}

struct nmd_ctx *nmd_create(const char *filename)
{
    const struct {
        const char *libname;
        unsigned build_version;
        unsigned runtime_version;
    } fflibs[] = {
        {"avutil",     LIBAVUTIL_VERSION_INT,     avutil_version()},
        {"avcodec",    LIBAVCODEC_VERSION_INT,    avcodec_version()},
        {"avformat",   LIBAVFORMAT_VERSION_INT,   avformat_version()},
        {"avfilter",   LIBAVFILTER_VERSION_INT,   avfilter_version()},
    };

    av_assert0(AV_TIME_BASE == 1000000);

    struct nmd_ctx *s = av_mallocz(sizeof(*s));
    if (!s)
        return NULL;

    s->filename = av_strdup(filename);
    s->logname  = av_asprintf("nope.media:%s", av_basename(filename));
    if (!s->filename || !s->logname)
        goto fail;

    s->class = &class;

    av_log_set_level(LOG_LEVEL);

    s->log_ctx = nmdi_log_alloc();
    if (!s->log_ctx || nmdi_log_init(s->log_ctx, s) < 0)
        goto fail;

    LOG(s, INFO, "nope.media %d.%d.%d",
        NMD_VERSION_MAJOR, NMD_VERSION_MINOR, NMD_VERSION_MICRO);

#define VFMT(v) (v)>>16, (v)>>8 & 0xff, (v) & 0xff
    for (size_t i = 0; i < FF_ARRAY_ELEMS(fflibs); i++) {
        const unsigned bversion = fflibs[i].build_version;
        const unsigned rversion = fflibs[i].runtime_version;
        LOG(s, INFO, "lib%-12s build:%3u.%3u.%3u runtime:%3u.%3u.%3u",
            fflibs[i].libname, VFMT(bversion), VFMT(rversion));
        if (bversion != rversion)
            LOG(s, WARNING, "/!\\ build and runtime version of FFmpeg mismatch /!\\");
    }

    avformat_network_init();

    av_opt_set_defaults(s);

    s->last_ts              = AV_NOPTS_VALUE;
    s->first_ts             = AV_NOPTS_VALUE;
    s->last_frame_poped_ts  = AV_NOPTS_VALUE;
    s->last_pushed_frame_ts = AV_NOPTS_VALUE;
    s->seek_time64          = AV_NOPTS_VALUE;
    s->loop_start64         = AV_NOPTS_VALUE;
    s->loop_end64           = AV_NOPTS_VALUE;

    av_assert0(!s->context_configured);
    return s;

fail:
    free_context(s);
    return NULL;
}

void nmd_freep(struct nmd_ctx **sp)
{
    struct nmd_ctx *s = *sp;

    if (!s)
        return;

    LOG(s, DEBUG, "destroying context");

    free_temp_context_data(s);
    free_context(s);
    *sp = NULL;
}

/**
 * Map the timeline time to the media time
 */
static int64_t get_media_time(const struct nmdi_opts *o, int64_t t)
{
    const int64_t mt = o->start_time64 + t;
    return o->end_time64 == AV_NOPTS_VALUE ? mt : FFMIN(mt, o->end_time64);
}

static void update_loop(struct nmd_ctx *s)
{
    const struct nmdi_opts *o = &s->opts;

    int64_t start = AV_NOPTS_VALUE;
    int64_t end = AV_NOPTS_VALUE;
    if (s->loop_end > s->loop_start) {
        start = get_media_time(o, TIME2INT64(s->loop_start));
        end = o->start_time64 + TIME2INT64(s->loop_end);
        /* Include the frame held at `end_time` when the requested loop extends beyond it. */
        if (o->end_time64 != AV_NOPTS_VALUE && end > o->end_time64)
            end = o->end_time64 + 1;
        if (end <= start)
            start = end = AV_NOPTS_VALUE;
    }

    if (start == s->loop_start64 && end == s->loop_end64)
        return;

    s->loop_start64 = start;
    s->loop_end64 = end;

    TRACE(s, "loop range: [%s,%s)", PTS2TIMESTR(s->loop_start64), PTS2TIMESTR(s->loop_end64));
    s->loop_rev = nmdi_async_set_loop(s->actx, s->loop_start64, s->loop_end64);
}

static int set_context_fields(struct nmd_ctx *s)
{
    struct nmdi_opts *o = &s->opts;

    if (o->sw_pix_fmt != NMD_PIXFMT_AUTO) {
        const enum AVPixelFormat fmt = nmdi_pix_fmts_nmd2ff(o->sw_pix_fmt);
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
        if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            LOG(s, ERROR, "Invalid software decoding pixel format specified");
            return AVERROR(EINVAL);
        }
    }

    if (o->auto_hwaccel && (o->filters || o->autorotate)) {
        LOG(s, WARNING, "Filters ('%s') or autorotate (%d) settings "
            "are set but hwaccel is enabled, disabling auto_hwaccel so these "
            "options are honored", o->filters, o->autorotate);
        o->auto_hwaccel = 0;
    }

    LOG(s, INFO, "avselect:%d start_time:%f end_time:%f "
        "dist_time_seek_trigger:%f queues:[%d %d %d] filters:'%s'",
        o->avselect, o->start_time, o->end_time,
        o->dist_time_seek_trigger,
        o->max_nb_packets, o->max_nb_frames, o->max_nb_sink,
        o->filters ? o->filters : "");

    o->start_time64 = TIME2INT64(o->start_time);
    o->dist_time_seek_trigger64 = TIME2INT64(o->dist_time_seek_trigger);
    o->end_time64 = o->end_time < 0 ? AV_NOPTS_VALUE : TIME2INT64(o->end_time);

    if (o->end_time64 != AV_NOPTS_VALUE && o->end_time64 < 0) {
        LOG(s, ERROR, "Invalid end time (%g): must be greater or equal to start_time (%g)", o->end_time, o->start_time);
        return AVERROR(EINVAL);
    }

    TRACE(s, "rescaled values: start_time=%s end_time=%s dist=%s",
          PTS2TIMESTR(o->start_time64),
          PTS2TIMESTR(o->end_time64),
          PTS2TIMESTR(o->dist_time_seek_trigger64));

    av_assert0(!s->actx);
    s->actx = nmdi_async_alloc_context();
    if (!s->actx)
        return AVERROR(ENOMEM);

    int ret = nmdi_async_init(s->actx, s->log_ctx, s->filename, &s->opts);
    if (ret < 0)
        return ret;

    update_loop(s);

    s->context_configured = 1;

    return 0;
}

/**
 * This can not be done earlier inside the context allocation function because
 * it requires user option to be set, which is done between the context
 * allocation and the first call to nmd_get_*frame() or nmd_get_duration().
 */
static int configure_context(struct nmd_ctx *s)
{
    if (s->context_configured)
        return 1;

    TRACE(s, "set context fields");
    int ret = set_context_fields(s);
    if (ret < 0) {
        LOG(s, ERROR, "Unable to set context fields: %s", av_err2str(ret));
        free_temp_context_data(s);
        return ret;
    }

    return 0;
}

static const int col_spc_map[] = {
    [AVCOL_SPC_RGB]                = NMD_COL_SPC_RGB,
    [AVCOL_SPC_BT709]              = NMD_COL_SPC_BT709,
    [AVCOL_SPC_UNSPECIFIED]        = NMD_COL_SPC_UNSPECIFIED,
    [AVCOL_SPC_RESERVED]           = NMD_COL_SPC_RESERVED,
    [AVCOL_SPC_FCC]                = NMD_COL_SPC_FCC,
    [AVCOL_SPC_BT470BG]            = NMD_COL_SPC_BT470BG,
    [AVCOL_SPC_SMPTE170M]          = NMD_COL_SPC_SMPTE170M,
    [AVCOL_SPC_SMPTE240M]          = NMD_COL_SPC_SMPTE240M,
    [AVCOL_SPC_YCGCO]              = NMD_COL_SPC_YCGCO,
    [AVCOL_SPC_BT2020_NCL]         = NMD_COL_SPC_BT2020_NCL,
    [AVCOL_SPC_BT2020_CL]          = NMD_COL_SPC_BT2020_CL,
    [AVCOL_SPC_SMPTE2085]          = NMD_COL_SPC_SMPTE2085,
    [AVCOL_SPC_CHROMA_DERIVED_NCL] = NMD_COL_SPC_CHROMA_DERIVED_NCL,
    [AVCOL_SPC_CHROMA_DERIVED_CL]  = NMD_COL_SPC_CHROMA_DERIVED_CL,
    [AVCOL_SPC_ICTCP]              = NMD_COL_SPC_ICTCP,
};

NMDI_STATIC_ASSERT(col_spc_map, FF_ARRAY_ELEMS(col_spc_map) < 32);

static int get_nmd_col_spc(int avcol_spc)
{
    if (avcol_spc < 0 || avcol_spc >= FF_ARRAY_ELEMS(col_spc_map))
        return NMD_COL_SPC_UNSPECIFIED;
    return col_spc_map[avcol_spc];
}

static const int col_rng_map[] = {
    [AVCOL_RANGE_UNSPECIFIED] = NMD_COL_RNG_UNSPECIFIED,
    [AVCOL_RANGE_MPEG]        = NMD_COL_RNG_LIMITED,
    [AVCOL_RANGE_JPEG]        = NMD_COL_RNG_FULL,
};

NMDI_STATIC_ASSERT(col_rng_map, FF_ARRAY_ELEMS(col_rng_map) < 32);

static int get_nmd_col_rng(int avcol_rng)
{
    if (avcol_rng < 0 || avcol_rng >= FF_ARRAY_ELEMS(col_rng_map))
        return NMD_COL_RNG_UNSPECIFIED;
    return col_rng_map[avcol_rng];
}

static const int col_pri_map[] = {
    [AVCOL_PRI_RESERVED0]   = NMD_COL_PRI_RESERVED0,
    [AVCOL_PRI_BT709]       = NMD_COL_PRI_BT709,
    [AVCOL_PRI_UNSPECIFIED] = NMD_COL_PRI_UNSPECIFIED,
    [AVCOL_PRI_RESERVED]    = NMD_COL_PRI_RESERVED,
    [AVCOL_PRI_BT470M]      = NMD_COL_PRI_BT470M,
    [AVCOL_PRI_BT470BG]     = NMD_COL_PRI_BT470BG,
    [AVCOL_PRI_SMPTE170M]   = NMD_COL_PRI_SMPTE170M,
    [AVCOL_PRI_SMPTE240M]   = NMD_COL_PRI_SMPTE240M,
    [AVCOL_PRI_FILM]        = NMD_COL_PRI_FILM,
    [AVCOL_PRI_BT2020]      = NMD_COL_PRI_BT2020,
    [AVCOL_PRI_SMPTE428]    = NMD_COL_PRI_SMPTE428,
    [AVCOL_PRI_SMPTE431]    = NMD_COL_PRI_SMPTE431,
    [AVCOL_PRI_SMPTE432]    = NMD_COL_PRI_SMPTE432,
    [AVCOL_PRI_JEDEC_P22]   = NMD_COL_PRI_JEDEC_P22,
};

NMDI_STATIC_ASSERT(col_pri_map, FF_ARRAY_ELEMS(col_pri_map) < 32);

static int get_nmd_col_pri(int avcol_pri)
{
    if (avcol_pri < 0 || avcol_pri >= FF_ARRAY_ELEMS(col_pri_map))
        return NMD_COL_PRI_UNSPECIFIED;
    return col_pri_map[avcol_pri];
}

static const int col_trc_map[] = {
    [AVCOL_TRC_RESERVED0]    = NMD_COL_TRC_RESERVED0,
    [AVCOL_TRC_BT709]        = NMD_COL_TRC_BT709,
    [AVCOL_TRC_UNSPECIFIED]  = NMD_COL_TRC_UNSPECIFIED,
    [AVCOL_TRC_RESERVED]     = NMD_COL_TRC_RESERVED,
    [AVCOL_TRC_GAMMA22]      = NMD_COL_TRC_GAMMA22,
    [AVCOL_TRC_GAMMA28]      = NMD_COL_TRC_GAMMA28,
    [AVCOL_TRC_SMPTE170M]    = NMD_COL_TRC_SMPTE170M,
    [AVCOL_TRC_SMPTE240M]    = NMD_COL_TRC_SMPTE240M,
    [AVCOL_TRC_LINEAR]       = NMD_COL_TRC_LINEAR,
    [AVCOL_TRC_LOG]          = NMD_COL_TRC_LOG,
    [AVCOL_TRC_LOG_SQRT]     = NMD_COL_TRC_LOG_SQRT,
    [AVCOL_TRC_IEC61966_2_4] = NMD_COL_TRC_IEC61966_2_4,
    [AVCOL_TRC_BT1361_ECG]   = NMD_COL_TRC_BT1361_ECG,
    [AVCOL_TRC_IEC61966_2_1] = NMD_COL_TRC_IEC61966_2_1,
    [AVCOL_TRC_BT2020_10]    = NMD_COL_TRC_BT2020_10,
    [AVCOL_TRC_BT2020_12]    = NMD_COL_TRC_BT2020_12,
    [AVCOL_TRC_SMPTE2084]    = NMD_COL_TRC_SMPTE2084,
    [AVCOL_TRC_SMPTE428]     = NMD_COL_TRC_SMPTE428,
    [AVCOL_TRC_ARIB_STD_B67] = NMD_COL_TRC_ARIB_STD_B67,
};

NMDI_STATIC_ASSERT(col_trc_map, FF_ARRAY_ELEMS(col_trc_map) < 32);

static int get_nmd_col_trc(int avcol_trc)
{
    if (avcol_trc < 0 || avcol_trc >= FF_ARRAY_ELEMS(col_trc_map))
        return NMD_COL_TRC_UNSPECIFIED;
    return col_trc_map[avcol_trc];
}

#define START_FUNC_BASE(name, ...) do {                     \
    s->cur_func_name = name;                                \
    if (LOG_LEVEL >= AV_LOG_WARNING)                        \
        s->entering_time = av_gettime();                    \
    LOG(s, DEBUG, ">>> " name __VA_ARGS__);                \
} while (0)

#define START_FUNC(name)      START_FUNC_BASE(name, " requested")
#define START_FUNC_T(name, t) START_FUNC_BASE(name, " requested with t=%g", t)

#define END_FUNC(max_time_warning) do {                                                                     \
    const float exect = LOG_LEVEL >= AV_LOG_WARNING ? (av_gettime() - s->entering_time) / 1000000. : -1;    \
                                                                                                            \
    if (exect > max_time_warning)                                                                           \
        LOG(s, WARNING, "getting the frame took %fs!", exect);                                              \
                                                                                                            \
    if (LOG_LEVEL >= AV_LOG_WARNING)                                                                        \
        LOG(s, DEBUG, "<<< %s executed in %fs", s->cur_func_name, exect);                                   \
} while (0)

#define MAX_ASYNC_OP_TIME (10/1000.)
#define MAX_SYNC_OP_TIME  (1/60.)

static int64_t get_frame_pass(const AVFrame *frame)
{
    return (intptr_t)frame->opaque;
}

/*
 * Distinguish repeated frames internally: the frame at 2s in pass 1 must not
 * count as the same returned frame as 2s in pass 0. Public frame timestamps
 * keep their media time; only this internal position includes the pass offset.
 */
static int64_t get_frame_pos(const struct nmd_ctx *s, const AVFrame *frame)
{
    return frame->pts + get_frame_pass(frame) * s->pass_timeline.stride;
}

/* Return the frame only if different from previous one. We do not make a
 * simple pointer check because of the frame reference counting (and thus
 * pointer reuse, depending on many parameters)  */
static int ret_frame(struct nmd_ctx *s, AVFrame *avframe, int status, struct nmd_frame **framep)
{
    struct nmd_frame *frame = NULL;
    const struct nmdi_opts *o = &s->opts;

    s->eof = !avframe && (status == AVERROR_EOF || status == AVERROR_EXIT);

    if (!avframe) {
        LOG(s, DEBUG, "no frame to return");
        goto end;
    }

    const int64_t frame_ts = get_frame_pos(s, avframe);
    s->seek_time64 = AV_NOPTS_VALUE;

    TRACE(s, "last_pushed_frame_ts:%s (%"PRId64") frame_ts:%s (%"PRId64")",
          av_ts2timestr(s->last_pushed_frame_ts, &s->st_timebase),
          s->last_pushed_frame_ts,
          av_ts2timestr(frame_ts, &s->st_timebase),
          frame_ts);

    /* if same frame as previously, do not raise it again */
    if (s->last_pushed_frame_ts == frame_ts) {
        LOG(s, DEBUG, "same frame as previously, return NULL");
        av_frame_free(&avframe);
        goto end;
    }

    frame = av_mallocz(sizeof(*frame));
    if (!frame) {
        av_frame_free(&avframe);
        goto end;
    }

    s->last_pushed_frame_ts = frame_ts;
    s->last_pushed_frame_pass = get_frame_pass(avframe);
    nmdi_async_release_passes(s->actx, s->last_pushed_frame_pass);

    frame->internal = avframe;
    memcpy(frame->datap, avframe->data, sizeof(frame->datap));
    memcpy(frame->linesizep, avframe->linesize, sizeof(frame->linesizep));
    frame->pts      = avframe->pts;
    frame->ms       = av_rescale_q(avframe->pts, s->st_timebase, AV_TIME_BASE_Q);
    frame->ts       = avframe->pts * av_q2d(s->st_timebase);
    frame->color_space     = get_nmd_col_spc(avframe->colorspace);
    frame->color_range     = get_nmd_col_rng(avframe->color_range);
    frame->color_primaries = get_nmd_col_pri(avframe->color_primaries);
    frame->color_trc       = get_nmd_col_trc(avframe->color_trc);
    if (o->avselect == NMD_SELECT_VIDEO) {
        if (avframe->format == AV_PIX_FMT_VIDEOTOOLBOX ||
            avframe->format == AV_PIX_FMT_VAAPI        ||
            avframe->format == AV_PIX_FMT_MEDIACODEC) {
            frame->datap[0] = avframe->data[3];
        }
        frame->width   = avframe->width;
        frame->height  = avframe->height;
        frame->pix_fmt = nmdi_pix_fmts_ff2nmd(avframe->format);
        LOG(s, DEBUG, "return %dx%d video frame @ ts=%s",
            avframe->width, avframe->height, av_ts2timestr(avframe->pts, &s->st_timebase));
    } else if (o->avselect == NMD_SELECT_AUDIO && o->audio_texture) {
        frame->width   = avframe->width;
        frame->height  = avframe->height;
        frame->pix_fmt = NMD_SMPFMT_FLT;
        LOG(s, DEBUG, "return %dx%d audio tex frame @ ts=%s",
            avframe->width, avframe->height, av_ts2timestr(avframe->pts, &s->st_timebase));
    } else {
        frame->nb_samples = avframe->nb_samples;
        frame->pix_fmt = nmdi_smp_fmts_ff2nmd(avframe->format);
        LOG(s, DEBUG, "return %d samples audio frame @ ts=%s",
            avframe->nb_samples, av_ts2timestr(avframe->pts, &s->st_timebase));
    }

end:
    END_FUNC(MAX_SYNC_OP_TIME);

    *framep = frame;

    if (frame)
        return NMD_RET_NEWFRAME;
    else if (status >= 0)
        return NMD_RET_UNCHANGED;
    else if (s->eof)
        return NMD_ERR_EOF;

    return averror_to_nmd_err(status);
}

void nmd_frame_releasep(struct nmd_frame **framep)
{
    struct nmd_frame *frame = *framep;
    if (!frame)
        return;

    AVFrame *avframe = frame->internal;
    av_frame_free(&avframe);
    av_freep(framep);
}

void nmd_mc_frame_releasep(struct nmd_frame **framep, int release_fence_fd)
{
#if HAVE_MEDIACODEC_HWACCEL
    struct nmd_frame *frame = *framep;
    if (!frame) {
        if (release_fence_fd >= 0)
            close(release_fence_fd);
        return;
    }
    AVFrame *avframe = frame->internal;
    nmdi_mediacodec_release_frame(&avframe, release_fence_fd);
    av_freep(framep);
#else
    av_assert0(0);
#endif
}

static int pop_frame(struct nmd_ctx *s, AVFrame **framep)
{
    int ret = 0;
    AVFrame *frame = NULL;

    if (s->cached_frame) {
        TRACE(s, "we have a cached frame, pop this one");
        frame = s->cached_frame;
        s->cached_frame = NULL;
    } else {

        /* Stream time base is required to interpret the frame PTS */
        if (!s->st_timebase.den) {
            struct nmd_info info;
            ret = nmdi_async_fetch_info(s->actx, &info);
            if (ret < 0) {
                TRACE(s, "unable to fetch info %s", av_err2str(ret));
            } else {
                s->st_timebase = av_make_q(info.timebase[0], info.timebase[1]);
                LOG(s, DEBUG, "store stream timebase %d/%d",
                    s->st_timebase.num, s->st_timebase.den);
                av_assert0(s->st_timebase.den);
                ret = nmdi_async_get_pass_timeline(s->actx, &s->pass_timeline);
                av_assert0(ret >= 0);
            }
        }

        if (s->st_timebase.den) {
            ret = nmdi_async_pop_frame(s->actx, &frame);
            if (ret < 0)
                TRACE(s, "poped a message raising %s", av_err2str(ret));
        }
    }

    if (frame) {
        const int64_t ts = get_frame_pos(s, frame);
        TRACE(s, "poped frame with ts=%s (%"PRId64")", av_ts2timestr(ts, &s->st_timebase), ts);
        s->last_frame_poped_ts = ts;
    } else {
        TRACE(s, "no frame available");
        /* We save the last timestamp in order to avoid restarting the decoding
         * thread again */
        if (s->last_ts == AV_NOPTS_VALUE ||
            (s->last_frame_poped_ts != AV_NOPTS_VALUE && s->last_frame_poped_ts > s->last_ts)) {
            TRACE(s, "last timestamp is apparently %s", av_ts2timestr(s->last_ts, &s->st_timebase));
            s->last_ts = s->last_frame_poped_ts;
        }
    }

    TRACE(s, "pop frame %p", frame);
    *framep = frame;
    return ret;
}

#define SYNTH_FRAME 0

#if SYNTH_FRAME
static int ret_synth_frame(struct nmd_ctx *s, int64_t t64, struct nmd_frame **framep)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return NMD_ERR_MEMORY;
    const int frame_id = lrint(t64 * 60 / 1000000);

    frame->format = AV_PIX_FMT_RGBA;
    frame->width = frame->height = 1;
    frame->pts = t64;
    int ret = av_frame_get_buffer(frame, 16);
    if (ret < 0) {
        av_frame_free(&frame);
        return averror_to_nmd_err(ret);
    }
    frame->data[0][0] = (frame_id>>8 & 0xf) * 17;
    frame->data[0][1] = (frame_id>>4 & 0xf) * 17;
    frame->data[0][2] = (frame_id    & 0xf) * 17;
    frame->data[0][3] = 0xff;
    return ret_frame(s, frame, 0, framep);
}
#endif

int nmd_seek(struct nmd_ctx *s, double reqt)
{
    START_FUNC_T("SEEK", reqt);

    av_frame_free(&s->cached_frame);
    s->last_pushed_frame_ts = AV_NOPTS_VALUE;

    int ret = configure_context(s);
    if (ret < 0)
        goto end;

    const struct nmdi_opts *o = &s->opts;
    s->seek_time64 = get_media_time(o, TIME2INT64(reqt));
    ret = nmdi_async_seek(s->actx, s->seek_time64);

end:
    END_FUNC(MAX_ASYNC_OP_TIME);
    return averror_to_nmd_err(ret);
}

int nmd_stop(struct nmd_ctx *s)
{
    START_FUNC("STOP");

    av_frame_free(&s->cached_frame);
    s->last_pushed_frame_ts = AV_NOPTS_VALUE;
    s->seek_time64 = AV_NOPTS_VALUE;

    int ret = configure_context(s);
    if (ret < 0)
        goto end;

    ret = nmdi_async_stop(s->actx);

end:
    END_FUNC(MAX_ASYNC_OP_TIME);
    return averror_to_nmd_err(ret);
}

int nmd_set_loop_range(struct nmd_ctx *s, double start, double end)
{
    START_FUNC("SET LOOP");

    int ret = 0;
    if (isnan(start) || isnan(end)) {
        LOG(s, ERROR, "Invalid loop range [%g,%g)", start, end);
        ret = AVERROR(EINVAL);
        goto end;
    }

    /* Keep the times in microseconds far from overflowing */
    const double max_time = (double)(INT64_MAX / 4 / AV_TIME_BASE);
    s->loop_start = av_clipd(start, 0.0, max_time);
    s->loop_end = av_clipd(end, 0.0, max_time);

    if (s->context_configured)
        update_loop(s);

end:
    END_FUNC(MAX_ASYNC_OP_TIME);
    return averror_to_nmd_err(ret);
}

int nmd_start(struct nmd_ctx *s)
{
    START_FUNC("START");

    int ret = configure_context(s);
    if (ret < 0)
        goto end;

    ret = nmdi_async_start(s->actx);

end:
    END_FUNC(MAX_ASYNC_OP_TIME);
    return averror_to_nmd_err(ret);
}

/*
 * Stream timebase must be known when this function is called.
 */
static inline int64_t stream_time(const struct nmd_ctx *s, int64_t t)
{
    av_assert0(s->st_timebase.den);
    return av_rescale_q(t, AV_TIME_BASE_Q, s->st_timebase);
}

/*
 * Exclusive loop end in the stream time base, without the pass offset.
 * Limit it to the interval reserved for pass 0 to avoid overflow when the
 * loop extends far beyond the media.
 */
static inline int64_t stream_loop_end(const struct nmd_ctx *s)
{
    av_assert0(s->st_timebase.den);
    const int64_t timeline_end = s->pass_timeline.origin + s->pass_timeline.stride;
    const int64_t end = FFMIN(s->loop_end64, av_rescale_q(timeline_end, s->st_timebase, AV_TIME_BASE_Q));
    return nmdi_loop_stream_end(end, s->opts.end_time64, s->st_timebase);
}

/* Rounding must not move a request just before the loop end onto the excluded boundary. */
static int64_t stream_request_time(const struct nmd_ctx *s, int64_t vt)
{
    const int64_t stt = stream_time(s, vt);
    if (s->loop_end64 == AV_NOPTS_VALUE || !s->pass_timeline.stride || vt < s->loop_start64 || vt >= s->loop_end64)
        return stt;
    return FFMIN(stt, stream_loop_end(s) - 1);
}

/* Whether this pass ended under the current range. The next pass records the revision used at the wrap. */
static int is_pass_end_current(const struct nmd_ctx *s, int64_t pass)
{
    return nmdi_async_get_pass_loop_rev(s->actx, pass + 1) == s->loop_rev;
}

/* Whether frames at the requested time were left out when this pass ended. */
static int is_request_past_pass_end(const struct nmd_ctx *s, const AVFrame *candidate, int64_t stt)
{
    if (!candidate && s->last_pushed_frame_ts == AV_NOPTS_VALUE)
        return 0;
    const int64_t pass = candidate ? get_frame_pass(candidate) : s->last_pushed_frame_pass;
    return stt >= nmdi_async_get_pass_end(s->actx, pass);
}

/*
 * Measure how far playback must advance from the last returned frame to `vt`,
 * and choose the pass that should supply it. During looping, a request earlier
 * than that frame and inside the range belongs to the next repetition. Set
 * `seekp` when the requested frame is no longer available in the queued passes.
 */
static int64_t get_distance(const struct nmd_ctx *s, int64_t vt, int64_t *passp, int *seekp)
{
    const int64_t pass = s->last_pushed_frame_pass;
    const int64_t pts = s->last_pushed_frame_ts - pass * s->pass_timeline.stride;
    const int64_t stt = stream_request_time(s, vt);

    *passp = 0;
    *seekp = 0;

    /* Until a frame is returned, measure from the seek target. */
    if (s->seek_time64 != AV_NOPTS_VALUE)
        return stt - stream_time(s, s->seek_time64);

    /* Starting playback beyond the loop end keeps normal forward playback. */
    const int looping = s->loop_end64 != AV_NOPTS_VALUE && s->pass_timeline.stride &&
                        (pass || pts < stream_loop_end(s));
    if (!looping) {
        *passp = pass;
        return stt - pts;
    }

    /*
     * A frame from a later pass means the current pass has finished. After a
     * range change, that wrap may still reflect the old end, so check which
     * range revision caused it before treating it as the current boundary.
     */
    const AVFrame *next = s->cached_frame;
    const int pass_over = next && get_frame_pass(next) > pass;
    const int pass_end_current = pass_over && is_pass_end_current(s, pass);

    if (vt >= s->loop_end64) {
        *seekp = 1;
        return 0;
    }

    if (vt < s->loop_start64)
        return stt - s->last_pushed_frame_ts;

    /* A loop extending beyond the media holds its last frame until the caller wraps. */
    const int64_t end = FFMIN(stream_loop_end(s), s->pass_timeline.end);
    if (stt >= pts) {
        *passp = pass;
        if (pass_over) {
            *seekp = is_request_past_pass_end(s, NULL, stt);
            return 0;
        }
        return stt > end ? FFMAX(end - pts, 1) : stt - pts;
    }

    *passp = pass + 1;
    if (pass_end_current)
        return FFMAX(stt - next->pts, 0) + 1;

    const int64_t diff = end - pts + stt - stream_time(s, s->loop_start64);
    *seekp = diff <= 0;
    return diff;
}

int nmd_get_frame_ms(struct nmd_ctx *s, int64_t t64, struct nmd_frame **framep)
{
    int64_t diff;
    const struct nmdi_opts *o = &s->opts;

    START_FUNC_T("GET FRAME", t64 / 1000000.);

#if SYNTH_FRAME
    return ret_synth_frame(s, t64, framep);
#endif

    int ret = configure_context(s);
    if (ret < 0)
        return ret_frame(s, NULL, ret, framep);

    if (t64 < 0) {
        nmd_start(s);
        return ret_frame(s, NULL, 0, framep);
    }

    const int64_t vt = get_media_time(o, t64);
    TRACE(s, "t=%s -> vt=%s", PTS2TIMESTR(t64), PTS2TIMESTR(vt));

    /* Retry after a range change if the old range stopped playback with no filtered output. */
    const int64_t ended_loop_rev = nmdi_async_get_ended_loop_rev(s->actx);
    if (ended_loop_rev >= 0 && ended_loop_rev != s->loop_rev) {
        LOG(s, DEBUG, "loop range changed after an empty pass, seek to %s", PTS2TIMESTR(vt));
        av_frame_free(&s->cached_frame);
        s->last_pushed_frame_ts = AV_NOPTS_VALUE;
        s->last_ts = AV_NOPTS_VALUE;
        ret = nmdi_async_seek(s->actx, vt);
        if (ret < 0)
            return ret_frame(s, NULL, ret, framep);
        s->seek_time64 = vt;
    }

    if (s->last_ts != AV_NOPTS_VALUE && stream_time(s, vt) >= s->last_ts &&
        s->last_pushed_frame_ts == s->last_ts) {
        TRACE(s, "requested the last frame again");
        return ret_frame(s, NULL, 0, framep);
    }

    if (s->first_ts != AV_NOPTS_VALUE && stream_time(s, vt) <= s->first_ts &&
        s->last_pushed_frame_ts == s->first_ts) {
        TRACE(s, "requested the first frame again");
        return ret_frame(s, NULL, 0, framep);
    }

    AVFrame *candidate = NULL;
    int64_t target_pass = 0;
    int force_seek = 0;

    /*
     * Before returning the first frame after startup or nmd_seek(), pop a
     * candidate to compare with the requested time.
     */
    if (s->last_pushed_frame_ts == AV_NOPTS_VALUE) {

        /* If prefetch wasn't done (async not started), and we requested a time
         * that is beyond the initial start_time, we request an appropriate seek
         * before we start the decoding process in order to save one seek and
         * some decoding (a seek for the initial start_time, then another one soon
         * after to reach the requested time). */
        if (!nmdi_nmdi_async_started(s->actx) && vt > o->start_time64) {
            TRACE(s, "no prefetch, but requested time (%s) beyond initial start_time (%s)",
                  PTS2TIMESTR(vt), PTS2TIMESTR(o->start_time64));
            nmdi_async_seek(s->actx, vt);
        }

        TRACE(s, "no frame ever pushed yet, pop a candidate");
        const int pop_ret = pop_frame(s, &candidate);
        if (!candidate || pop_ret < 0) {
            TRACE(s, "can not get a single frame for this media");
            return ret_frame(s, NULL, pop_ret, framep);
        }

        /* At this point we can assume the stream timebase is known because
         * pop_frame() was called. */
        const int64_t stt = stream_request_time(s, vt);
        diff = stt - candidate->pts;
        target_pass = get_frame_pass(candidate);

        TRACE(s, "diff with candidate (t=%s): %s [%"PRId64"]",
              av_ts2timestr(candidate->pts, &s->st_timebase),
              av_ts2timestr(diff, &s->st_timebase), diff);

        /*
         * The candidate is later than the requested time.
         *
         * This can happen when the requested time precedes the media's first
         * timestamp. For example, a request for 0s should still return the
         * first frame even if its timestamp is 0.1s.
         *
         * It also happens in the following scenario:
         * - nmd_seek(s, 10);
         * - nmd_start(s);
         * - nmd_get_frame(s, 3, &frame);
         *
         * nmd_start() starts decoding from the 10s seek target, but the first
         * frame request is for 3s. The candidate comes from the seek to 10s,
         * so we must discard it and seek back to 3s to find the frame
         * corresponding to the requested time.
         *
         * Only return the candidate here if there was no nmd_seek(), or if the
         * requested time is at or after its target.
         */
        if (diff < 0) {
            if (s->seek_time64 == AV_NOPTS_VALUE || vt >= s->seek_time64) {
                /* Warning: we must absolutely NOT save the timestamp of the
                 * candidate if the first time requested is not actually 0 */
                if (t64 == 0)
                    s->first_ts = get_frame_pos(s, candidate);
                return ret_frame(s, candidate, pop_ret, framep);
            }
            /*
             * Discard the frame from the previous seek. diff is still negative,
             * so the backward-seek path below will seek to the requested vt.
             */
            av_frame_free(&candidate);
        }

    } else {
        /* At this point we can assume the stream timebase is known because
         * a frame was already pushed. */
        diff = get_distance(s, vt, &target_pass, &force_seek);

        TRACE(s, "diff with latest frame (t=%s) returned: %s [%"PRId64"] in loop pass %"PRId64,
              av_ts2timestr(s->last_pushed_frame_ts, &s->st_timebase),
              av_ts2timestr(diff, &s->st_timebase),
              diff, target_pass);
    }

    if (!diff && !force_seek)
        return ret_frame(s, candidate, 0, framep);

    /* Check if a seek is needed */
    const int forward_seek = av_compare_ts(diff, s->st_timebase, o->dist_time_seek_trigger64, AV_TIME_BASE_Q) >= 0;
    if (force_seek || diff < 0 || forward_seek) {
        if (force_seek)
            TRACE(s, "queued loop passes cannot supply the requested frame, seek");
        else if (diff < 0)
            TRACE(s, "diff %s [%"PRId64"] < 0 request backward seek",
                  av_ts2timestr(diff, &s->st_timebase), diff);
        else
            TRACE(s, "diff %s > %s request future seek",
                  av_ts2timestr(diff, &s->st_timebase),
                  PTS2TIMESTR(o->dist_time_seek_trigger64));

        /*
         * Keep a candidate if no frame has been returned yet. Seeking a still
         * image to a later timestamp may produce no new frame.
         */
        if (diff > 0 && s->last_pushed_frame_ts != AV_NOPTS_VALUE)
            av_frame_free(&candidate);

        av_frame_free(&s->cached_frame);

        ret = nmdi_async_seek(s->actx, vt);
        if (ret < 0) {
            av_frame_free(&candidate);
            return ret_frame(s, NULL, ret, framep);
        }
        target_pass = 0;
        s->seek_time64 = vt;
    }

    /* A backward request within the loop can advance to a later pass. */
    const int next_pass = s->last_pushed_frame_ts != AV_NOPTS_VALUE && target_pass > s->last_pushed_frame_pass;
    int seek_retried = 0;

    /* Consume frames until we get a frame as accurate as possible */
    for (;;) {
        const int next_is_cached_frame = !!s->cached_frame;

        TRACE(s, "grab another frame");
        AVFrame *next = NULL;
        const int pop_ret = pop_frame(s, &next);
        av_assert0(!s->cached_frame);

        /* We must not use av_compare_ts() here, because it doesn't make the
         * comparison with the lowest timebase value. Instead, we need to lower
         * our timestamp precision to the stream one to get as accurate as
         * possible. */
        const int64_t rescaled_vt = stream_request_time(s, vt);

        /*
         * Queued passes may still reflect an older range, or decoding may
         * have stopped before the expected wrap. Seek to the requested time
         * and retry once with the current range.
         */
        const int64_t pass = next ? get_frame_pass(next) : 0;
        const int need_seek = !seek_retried &&
            ((next_pass && !candidate &&
              (!next || pop_ret < 0 ||
               (pass < target_pass && next->pts >= stream_loop_end(s)) ||
               (pass == target_pass && next->pts > rescaled_vt))) ||
             (next && pass > target_pass && is_request_past_pass_end(s, candidate, rescaled_vt)));
        if (need_seek) {
            LOG(s, DEBUG, "queued loop passes cannot supply the frame at %s, seek", PTS2TIMESTR(vt));
            av_frame_free(&next);
            av_frame_free(&candidate);
            ret = nmdi_async_seek(s->actx, vt);
            if (ret < 0)
                return ret_frame(s, NULL, ret, framep);
            target_pass = 0;
            seek_retried = 1;
            s->seek_time64 = vt;
            continue;
        }

        if (!next || pop_ret < 0) {
            TRACE(s, "no more frame");
            break;
        }

        if (pass < target_pass) {
            TRACE(s, "skip frame %s of a previous loop pass", av_ts2timestr(next->pts, &s->st_timebase));
            av_frame_free(&next);
            continue;
        }

        if (pass > target_pass) {
            TRACE(s, "cache frame %s of the next loop pass", av_ts2timestr(next->pts, &s->st_timebase));
            s->cached_frame = next;
            break;
        }

        const int64_t next_duration = next->duration;

        if (s->opts.use_pkt_duration && next_duration > 0 && rescaled_vt >= next->pts) {
            const int64_t next_guessed_pts = next->pts + next_duration;
            if (rescaled_vt < next_guessed_pts) {
                av_frame_free(&candidate);
                av_frame_free(&s->cached_frame);
                s->cached_frame = NULL;
                return ret_frame(s, next, 0, framep);
            }
        }

        if (next->pts > rescaled_vt) {
            TRACE(s, "grabbed frame is in the future %s > %s",
                  av_ts2timestr(next->pts, &s->st_timebase), PTS2TIMESTR(vt));
            const int first = s->last_pushed_frame_ts == AV_NOPTS_VALUE || s->seek_time64 != AV_NOPTS_VALUE;
            if (!candidate && !next_is_cached_frame && first) {
                candidate = next;
                TRACE(s, "we need to return a frame, select this future frame anyway");
                if (t64 == 0)
                    s->first_ts = get_frame_pos(s, candidate);
            } else {
                s->cached_frame = next;
                TRACE(s, "cache frame %s for next call", av_ts2timestr(next->pts, &s->st_timebase));
            }
            break;
        }
        av_frame_free(&candidate);
        candidate = next;
        if (candidate->pts == rescaled_vt) {
            TRACE(s, "grabbed exact frame %s", av_ts2timestr(candidate->pts, &s->st_timebase));
            break;
        }
    }

    return ret_frame(s, candidate, ret, framep);
}

int nmd_get_frame(struct nmd_ctx *s, double t, struct nmd_frame **framep)
{
    int64_t t64 = TIME2INT64(t);

    /*
     * A time just below the exclusive loop end can round up to it in
     * microseconds. Keep it inside the range, unless the whole range rounds
     * to zero length and looping is therefore disabled.
     */
    const int64_t loop_start64 = TIME2INT64(s->loop_start);
    const int64_t loop_end64 = TIME2INT64(s->loop_end);
    if (loop_end64 > loop_start64 && t < s->loop_end && t64 >= loop_end64)
        t64 = loop_end64 - 1;

    return nmd_get_frame_ms(s, t64, framep);
}

int nmd_get_next_frame(struct nmd_ctx *s, struct nmd_frame **framep)
{
    START_FUNC("GET NEXT FRAME");

    int ret = configure_context(s);
    if (ret < 0)
        return ret_frame(s, NULL, ret, framep);

    if (s->eof) {
        av_frame_free(&s->cached_frame);
        s->last_pushed_frame_ts = AV_NOPTS_VALUE;

        const struct nmdi_opts *o = &s->opts;
        ret = nmdi_async_seek(s->actx, o->start_time64);
        if (ret < 0)
            LOG(s, ERROR, "Failed to seek back to beginning of the file");
    }

    AVFrame *frame = NULL;
    ret = pop_frame(s, &frame);
    if (!frame || ret < 0)
        return ret_frame(s, NULL, ret, framep);

    return ret_frame(s, frame, ret, framep);
}

int nmd_get_info(struct nmd_ctx *s, struct nmd_info *info)
{
    START_FUNC("GET INFO");

    int ret = configure_context(s);
    if (ret < 0)
        goto end;
    ret = nmdi_async_fetch_info(s->actx, info);
    if (ret < 0)
        goto end;
    TRACE(s, "media info: %dx%d %f tb:%d/%d",
          info->width, info->height, info->duration,
          info->timebase[0], info->timebase[1]);

end:
    END_FUNC(1.0);
    return averror_to_nmd_err(ret);
}

int nmd_get_duration(struct nmd_ctx *s, double *duration)
{
    START_FUNC("GET DURATION");

    struct nmd_info info;
    int ret = nmd_get_info(s, &info);
    if (ret < 0)
        goto end;
    *duration = info.duration;

end:
    END_FUNC(1.0);
    return averror_to_nmd_err(ret);
}
