/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2016-2022 GoPro Inc.
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

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <nopemd.h>

#include "config.h"

#include "animation.h"
#include "internal.h"
#include "log.h"
#include "node_animkeyframe.h"
#include "node_media.h"
#include "node_uniform.h"
#include "nopegl/nopegl.h"
#include "utils/memory.h"

struct media_opts {
    const char *filename;
    int nopemd_min_level;
    struct ngl_node *anim;
    int audio_tex;
    int32_t max_nb_packets;
    int32_t max_nb_frames;
    int32_t max_nb_sink;
    int32_t max_pixels;
    int32_t stream_idx;
    int hwaccel;
    char *filters;
    char *vt_pix_fmt;
    int loop;
};

static const struct param_choices nopemd_log_level_choices = {
    .name = "nopemd_log_level",
    .consts = {
        {"verbose", NMD_LOG_VERBOSE, .desc=NGLI_DOCSTRING("verbose messages")},
        {"debug",   NMD_LOG_DEBUG,   .desc=NGLI_DOCSTRING("debugging messages")},
        {"info",    NMD_LOG_INFO,    .desc=NGLI_DOCSTRING("informational messages")},
        {"warning", NMD_LOG_WARNING, .desc=NGLI_DOCSTRING("warning messages")},
        {"error",   NMD_LOG_ERROR,   .desc=NGLI_DOCSTRING("error messages")},
        {NULL}
    }
};

#define HWACCEL_DISABLED 0
#define HWACCEL_AUTO     1

static const struct param_choices nopemd_hwaccel_choices = {
    .name = "nopemd_hwaccel",
    .consts = {
        {"disabled", HWACCEL_DISABLED, .desc=NGLI_DOCSTRING("disable hardware acceleration")},
        {"auto",     HWACCEL_AUTO,     .desc=NGLI_DOCSTRING("enable hardware acceleration if available")},
        {NULL}
    }
};

static int filename_changed(struct ngl_node *node);

#define OFFSET(x) offsetof(struct media_opts, x)
static const struct node_param media_params[] = {
    {"filename", NGLI_PARAM_TYPE_STR, OFFSET(filename), {.str=NULL},
                 .flags=NGLI_PARAM_FLAG_NON_NULL | NGLI_PARAM_FLAG_FILEPATH | NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                 .update_func=filename_changed,
                 .desc=NGLI_DOCSTRING("path to input media file")},
    {"nopemd_min_level", NGLI_PARAM_TYPE_SELECT, OFFSET(nopemd_min_level), {.i32=NMD_LOG_WARNING},
                         .choices=&nopemd_log_level_choices,
                         .desc=NGLI_DOCSTRING("nope.media min logging level")},
    {"time_anim", NGLI_PARAM_TYPE_NODE, OFFSET(anim),
                  .node_types=(const uint32_t[]){NGL_NODE_ANIMATEDTIME, NGLI_NODE_NONE},
                  .desc=NGLI_DOCSTRING("time remapping animation (must use a `linear` interpolation)")},
    {"audio_tex", NGLI_PARAM_TYPE_BOOL, OFFSET(audio_tex),
                  .desc=NGLI_DOCSTRING("load the audio and expose it as a stereo waves and frequencies buffer")},
    {"max_nb_packets", NGLI_PARAM_TYPE_I32, OFFSET(max_nb_packets), {.i32=1},
                       .desc=NGLI_DOCSTRING("maximum number of packets in nope.media demuxing queue")},
    {"max_nb_frames",  NGLI_PARAM_TYPE_I32, OFFSET(max_nb_frames),  {.i32=1},
                       .desc=NGLI_DOCSTRING("maximum number of frames in nope.media decoding queue")},
    {"max_nb_sink",    NGLI_PARAM_TYPE_I32, OFFSET(max_nb_sink),    {.i32=1},
                       .desc=NGLI_DOCSTRING("maximum number of frames in nope.media filtering queue")},
    {"max_pixels",     NGLI_PARAM_TYPE_I32, OFFSET(max_pixels),     {.i32=0},
                       .desc=NGLI_DOCSTRING("maximum number of pixels per frame")},
    {"stream_idx",     NGLI_PARAM_TYPE_I32, OFFSET(stream_idx),     {.i32=-1},
                       .desc=NGLI_DOCSTRING("force a stream number instead of picking the \"best\" one")},
    {"hwaccel",        NGLI_PARAM_TYPE_SELECT, OFFSET(hwaccel),     {.i32=HWACCEL_AUTO},
                       .choices=&nopemd_hwaccel_choices,
                       .desc=NGLI_DOCSTRING("hardware acceleration")},
    {"filters",        NGLI_PARAM_TYPE_STR, OFFSET(filters),
                       .desc=NGLI_DOCSTRING("filters to apply on the media (nope.media/libavfilter)")},
    {"vt_pix_fmt",     NGLI_PARAM_TYPE_STR, OFFSET(vt_pix_fmt),  {.str="auto"},
                       .desc=NGLI_DOCSTRING("auto or a comma or space separated list of VideoToolbox (Apple) allowed output pixel formats")},
    {"loop",           NGLI_PARAM_TYPE_BOOL, OFFSET(loop),
                       .desc=NGLI_DOCSTRING("loop infinitely during playback, useful for animated formats (GIF)")},
    {NULL}
};

static const enum ngl_log_level log_levels[] = {
    [NMD_LOG_VERBOSE] = NGL_LOG_VERBOSE,
    [NMD_LOG_DEBUG]   = NGL_LOG_DEBUG,
    [NMD_LOG_INFO]    = NGL_LOG_INFO,
    [NMD_LOG_WARNING] = NGL_LOG_WARNING,
    [NMD_LOG_ERROR]   = NGL_LOG_ERROR,
};

ngli_printf_format(6, 0)
static void callback_nopemd_log(void *arg, int level, const char *filename, int ln,
                                const char *fn, const char *fmt, va_list vl)
{
    if (level < 0 || level >= NGLI_ARRAY_NB(log_levels))
        return;

    const struct media_opts *o = arg;
    if (level < o->nopemd_min_level)
        return;

    char logline[128];
    char *logbuf = NULL;
    const char *logp = logline;

    /* we need a copy because it may be re-used a 2nd time */
    va_list vl_copy;
    va_copy(vl_copy, vl);

    int len = vsnprintf(logline, sizeof(logline), fmt, vl);

    /* handle the case where the line doesn't fit the stack buffer */
    if (len >= sizeof(logline)) {
        const size_t logbuf_size = (size_t)len + 1;
        logbuf = ngli_try_malloc(logbuf_size);
        if (!logbuf) {
            va_end(vl_copy);
            return;
        }
        vsnprintf(logbuf, logbuf_size, fmt, vl_copy);
        logp = logbuf;
    }

    if (logp[0])
        ngli_log_print(log_levels[level], __FILE__, __LINE__, __func__,
                       "[NOPE.MEDIA %s:%d %s] %s", filename, ln, fn, logp);

    ngli_free(logbuf);
    va_end(vl_copy);
}

static void get_start_end_time(struct ngl_node *node, double *start, double *end)
{
    const struct media_opts *o = node->opts;

    *start = -DBL_MAX;
    *end   = -DBL_MAX;

    struct ngl_node *anim_node = o->anim;
    if (anim_node) {
        const struct variable_opts *anim = anim_node->opts;
        if (anim->animkf.count) {
            double times[2], values[2];
            ngli_animation_get_bounds(anim->animkf.data, anim->animkf.count,
                                      NGLI_ANIMATION_FLAG_TIME_VALUES, times, values);
            *start = values[0];
            if (anim->animkf.count > 1)
                *end = values[1];
        }
    }
}

static int media_init(struct ngl_node *node)
{
    struct media_priv *s = node->priv_data;

    ngli_fence_init(&s->duration_fence);
    ngli_fence_init(&s->release_fence);

    return 0;
}

static void get_duration(void *data, void *shared_data, uint32_t thread_index)
{
    struct duration_job *job = data;

    job->ret = nmd_get_duration(job->player, &job->duration);
}

static int get_media_time(struct ngl_node *node, double t, double *dst_time)
{
    struct media_priv *s = node->priv_data;
    const struct media_opts *o = node->opts;
    struct ngl_node *anim_node = o->anim;
    double media_time = t;
    double initial_seek = 0.0;
    double time_origin = 0.0;

    if (anim_node) {
        struct variable_info *anim = anim_node->priv_data;
        const struct variable_opts *anim_o = anim_node->opts;
        double times[2], values[2];
        ngli_animation_get_bounds(anim_o->animkf.data, anim_o->animkf.count,
                                  NGLI_ANIMATION_FLAG_TIME_VALUES, times, values);
        initial_seek = s->start_time;
        time_origin  = times[0] + anim_o->time_offset;

        double anim_t = t;
        if (o->loop && times[1] > times[0])
            anim_t = time_origin + fmod(NGLI_MAX(0.0, t - time_origin), times[1] - times[0]);

        int ret = ngli_node_update(anim_node, anim_t);
        if (ret < 0)
            return ret;
        const double dval = *(double *)anim->data;
        media_time = NGLI_MAX(0, dval - initial_seek);

        TRACE("remapped time f(%g)=%g", t, media_time);
    }

    if (o->loop && s->end_time == -DBL_MAX) {
        if (s->duration <= 0.0) {
            ngli_fence_wait(&s->duration_fence);
            if (s->duration_job.ret == 0)
                s->duration = s->duration_job.duration;
        }
        const double loop_duration = s->duration - initial_seek;
        if (loop_duration > 0.0)
            media_time = fmod(NGLI_MAX(0.0, t - time_origin), loop_duration);
    }

    *dst_time = media_time;
    return 0;
}

/* Scene time after which the time remapping holds its final value */
static double get_mapping_end(const struct ngl_node *node)
{
    const struct media_opts *o = node->opts;
    if (!o->anim)
        return INFINITY;

    const struct variable_opts *anim_o = o->anim->opts;
    if (anim_o->animkf.count < 2)
        return -INFINITY;

    double times[2], values[2];
    ngli_animation_get_bounds(anim_o->animkf.data, anim_o->animkf.count,
                              NGLI_ANIMATION_FLAG_TIME_VALUES, times, values);
    return times[1] + anim_o->time_offset;
}

/* Whether a keyframe before t already reaches the media time requested at t. */
static bool mapping_holds_before(const struct ngl_node *node, double t, double media_time)
{
    const struct media_priv *s = node->priv_data;
    const struct media_opts *o = node->opts;
    if (!o->anim)
        return false;

    const struct variable_opts *anim_o = o->anim->opts;
    size_t count = 0;
    for (; count < anim_o->animkf.count; count++) {
        const struct animkeyframe_opts *kf = anim_o->animkf.data[count]->opts;
        /* Later keyframe times are sanitized to be at least this time. */
        if (kf->time + anim_o->time_offset >= t)
            break;
    }
    if (!count)
        return false;

    double times[2], values[2];
    ngli_animation_get_bounds(anim_o->animkf.data, count,
                              NGLI_ANIMATION_FLAG_TIME_VALUES, times, values);
    return NGLI_MAX(0.0, values[1] - s->start_time) == media_time;
}

/*
 * Map the context loop to media times, limited by the parent update range.
 * With the loop option, use the configured media range instead.
 */
static int get_loop_range(struct ngl_node *node, struct ngli_timerange *dst)
{
    struct media_priv *s = node->priv_data;
    const struct media_opts *o = node->opts;
    const struct ngli_timerange *loop = &node->ctx->loop_range;
    const struct ngli_timerange *update = &node->update_range;
    int ret;

    /* Changes to time remapping invalidate this cache in media_invalidate(). */
    const struct ngli_timerange inputs[] = {*loop, *update};
    NGLI_STATIC_ASSERT(sizeof(inputs) == sizeof(s->loop_inputs), "loop inputs");
    if (!memcmp(inputs, s->loop_inputs, sizeof(inputs))) {
        *dst = s->next_loop_range;
        return 0;
    }

    struct ngli_timerange range = {0};
    if (o->loop) {
        range.end = s->end_time != -DBL_MAX ? s->end_time - s->start_time : INFINITY;
    } else if (loop->end > loop->start &&
               NGLI_MAX(loop->start, update->start) < get_mapping_end(node)) {
        ret = get_media_time(node, NGLI_MAX(loop->start, update->start), &range.start);
        if (ret < 0)
            return ret;
        const double loop_end = NGLI_MIN(loop->end, update->end);
        if (loop_end > get_mapping_end(node)) {
            /* Leave the end open to include the final mapped frame while playback holds it. */
            range.end = INFINITY;
        } else {
            ret = get_media_time(node, loop_end, &range.end);
            if (ret < 0)
                return ret;
            if (range.end > range.start && mapping_holds_before(node, loop_end, range.end)) {
                /*
                 * If a keyframe's time is before loop_end and its value maps
                 * to range.end, the animation holds that media time until
                 * loop_end.
                 *
                 * The player excludes range.end when looping, so we need to
                 * extend range.end by one stream tick to keep the held frame
                 * inside the loop after timestamp rounding.
                 *
                 * The player also rounds times to microseconds, so we need to
                 * extend range.end by at least one microsecond to keep the
                 * extension from rounding to zero. Until the time base is
                 * known, leave the end open so prefetch does not discard the
                 * held frame.
                 */
                range.end = nextafter(range.end + NGLI_MAX(s->timebase, 1e-6), INFINITY);
            } else if (update->end < loop->end) {
                /*
                 * Rounding a request just before the parent range ends can
                 * select a frame at or past range.end. Extend range.end as
                 * for held frames so the player includes that frame.
                 */
                range.end = nextafter(range.end + NGLI_MAX(s->timebase, 1e-6), INFINITY);
            }
        }
        if (range.end <= range.start)
            range = (struct ngli_timerange){0};
    }

    memcpy(s->loop_inputs, inputs, sizeof(inputs));
    *dst = s->next_loop_range = range;
    return 0;
}

/* Cache only successful range changes so failed ones are retried. */
static int set_player_loop(struct ngl_node *node, const struct ngli_timerange *range)
{
    struct media_priv *s = node->priv_data;
    if (range->start == s->loop_range.start && range->end == s->loop_range.end)
        return 0;

    TRACE("loop %s over [%g,%g)", node->label, range->start, range->end);
    int ret = nmd_set_loop_range(s->player, range->start, range->end);
    if (ret < 0) {
        LOG(ERROR, "unable to loop %s over [%g,%g)", node->label, range->start, range->end);
        return NGL_ERROR_EXTERNAL;
    }
    s->loop_range = *range;
    return 0;
}

static int media_set_loop_range(struct ngl_node *node)
{
    const struct media_priv *s = node->priv_data;
    if (node->state != NGLI_NODE_STATE_READY || !s->player)
        return 0;

    struct ngli_timerange range;
    int ret = get_loop_range(node, &range);
    if (ret < 0)
        return ret;
    return set_player_loop(node, &range);
}

static int media_prefetch(struct ngl_node *node)
{
    struct media_priv *s = node->priv_data;
    const struct media_opts *o = node->opts;
    struct ngl_ctx *ctx = node->ctx;
    int ret;

    s->player = nmd_create(o->filename);
    if (!s->player)
        return NGL_ERROR_MEMORY;

    nmd_set_log_callback(s->player, node->opts, callback_nopemd_log);

    get_start_end_time(node, &s->start_time, &s->end_time);

    if (s->start_time != -DBL_MAX)
        nmd_set_option(s->player, "start_time", s->start_time);

    if (s->end_time != -DBL_MAX)
        nmd_set_option(s->player, "end_time", s->end_time);

    if (o->max_nb_packets) nmd_set_option(s->player, "max_nb_packets", o->max_nb_packets);
    if (o->max_nb_frames)  nmd_set_option(s->player, "max_nb_frames",  o->max_nb_frames);
    if (o->max_nb_sink)    nmd_set_option(s->player, "max_nb_sink",    o->max_nb_sink);
    if (o->max_pixels)     nmd_set_option(s->player, "max_pixels",     o->max_pixels);
    if (o->filters)        nmd_set_option(s->player, "filters",        o->filters);

    nmd_set_option(s->player, "stream_idx", o->stream_idx);
    nmd_set_option(s->player, "auto_hwaccel", o->hwaccel);

    s->timebase = INFINITY;
    s->loop_range = (struct ngli_timerange){NAN, NAN};
    s->loop_inputs[0].start = NAN;
    struct ngli_timerange loop_range;
    ret = get_loop_range(node, &loop_range);
    if (ret < 0)
        return ret;
    /* Failure to set the loop does not prevent playback; retry on update. */
    set_player_loop(node, &loop_range);

    nmd_set_option(s->player, "sw_pix_fmt", NMD_PIXFMT_AUTO);
    if (o->audio_tex) {
        nmd_set_option(s->player, "avselect", NMD_SELECT_AUDIO);
        nmd_set_option(s->player, "audio_texture", 1);
        return 0;
    }

#if defined(TARGET_IPHONE) || defined(TARGET_DARWIN)
    const char *vt_pix_fmt = o->vt_pix_fmt;
    if (!strcmp(o->vt_pix_fmt, "auto"))
        vt_pix_fmt = "nv12,p010";
    nmd_set_option(s->player, "vt_pix_fmt", vt_pix_fmt);
#elif defined(HAVE_VAAPI)
    struct vaapi_ctx *vaapi_ctx = &ctx->vaapi_ctx;
    nmd_set_option(s->player, "opaque", &vaapi_ctx->va_display);
#endif

    /*
     * Before starting the decoder, seek to the media time of the expected first
     * update. This avoids decoding from the start only to seek again on update.
     */
    if (node->prefetch_time >= 0.0 && (!o->loop || s->end_time != -DBL_MAX)) {
        double media_time;
        ret = get_media_time(node, node->prefetch_time, &media_time);
        if (ret < 0)
            return ret;
        if (media_time > 0.0)
            nmd_seek(s->player, media_time);
    }

    nmd_start(s->player);

    if (o->loop && s->end_time == -DBL_MAX) {
        ngli_fence_wait(&s->duration_fence);
        s->duration_job = (struct duration_job) {.player = s->player};
        ngli_queue_add_job(&ctx->background_queue, &s->duration_job, &s->duration_fence, get_duration, NULL);
    }

    return 0;
}

static const char * const pix_fmt_names[] = {
    [NMD_PIXFMT_RGBA]        = "rgba",
    [NMD_PIXFMT_BGRA]        = "bgra",
    [NMD_PIXFMT_VT]          = "vt",
    [NMD_PIXFMT_MEDIACODEC]  = "mediacodec",
    [NMD_PIXFMT_VAAPI]       = "vaapi",
    [NMD_PIXFMT_NV12]        = "nv12",
    [NMD_PIXFMT_YUV420P]     = "yuv420p",
    [NMD_PIXFMT_YUV422P]     = "yuv422p",
    [NMD_PIXFMT_YUV444P]     = "yuv444p",
    [NMD_PIXFMT_P010LE]      = "p010le",
    [NMD_PIXFMT_YUV420P10LE] = "yuv420p10le",
    [NMD_PIXFMT_YUV422P10LE] = "yuv422p10le",
    [NMD_PIXFMT_YUV444P10LE] = "yuv444p10le",
};

static const char *get_pix_fmt_name(int pix_fmt)
{
    if (pix_fmt < 0 || pix_fmt >= NGLI_ARRAY_NB(pix_fmt_names))
        return NULL;

    return pix_fmt_names[pix_fmt];
}

static const char *get_nmd_ret_name(int nmd_err)
{
    switch (nmd_err) {
    case NMD_ERR_MEMORY:    return "memory error";
    case NMD_ERR_EOF:       return "reached EOF";
    case NMD_ERR_GENERIC:   return "generic error";
    case NMD_RET_SUCCESS:   return "success";
    case NMD_RET_UNCHANGED: return "unchanged";
    default:                return "unknown";
    }
}


static int media_update(struct ngl_node *node, double t)
{
    struct media_priv *s = node->priv_data;
    const struct media_opts *o = node->opts;

    /* Evaluate loop bounds first: they update the time remapping animation. */
    struct ngli_timerange loop_range;
    int ret = get_loop_range(node, &loop_range);
    if (ret < 0)
        return ret;
    if (!o->loop && isinf(loop_range.end) && isinf(s->timebase)) {
        /* Fetch stream info on update so prefetch stays asynchronous. */
        struct nmd_info info;
        if (nmd_get_info(s->player, &info) == 0 && info.timebase[0] > 0 && info.timebase[1] > 0) {
            s->timebase = (double)info.timebase[0] / info.timebase[1];
            s->loop_inputs[0].start = NAN;
            ret = get_loop_range(node, &loop_range);
            if (ret < 0)
                return ret;
        }
    }
    /* Keep playing if the loop cannot be set; retry on the next update. */
    set_player_loop(node, &loop_range);

    double media_time;
    ret = get_media_time(node, t, &media_time);
    if (ret < 0)
        return ret;

    /* Rounding must not move a request outside the media loop. */
    if (!o->loop && ngli_timerange_contains(&node->ctx->loop_range, t) &&
        s->loop_range.end > s->loop_range.start && media_time >= s->loop_range.end)
        media_time = nextafter(s->loop_range.end, -INFINITY);

    nmd_frame_releasep(&s->frame);

    TRACE("get frame from %s at t=%g", node->label, media_time);
    struct nmd_frame *frame = NULL;
    ret = nmd_get_frame(s->player, media_time, &frame);
    if (ret == NMD_RET_NEWFRAME) {
        const char *pix_fmt_str = get_pix_fmt_name(frame->pix_fmt);
        if (o->audio_tex) {
            if (frame->pix_fmt != NMD_SMPFMT_FLT) {
                LOG(ERROR, "unexpected %s (%d) nope.media frame",
                    pix_fmt_str ? pix_fmt_str : "unknown", frame->pix_fmt);
                return NGL_ERROR_BUG;
            }
            pix_fmt_str = "audio";
        } else if (!pix_fmt_str) {
            LOG(ERROR, "invalid pixel format %d in nope.media frame", frame->pix_fmt);
            return NGL_ERROR_BUG;
        }
        TRACE("got frame %dx%d %s with ts=%f", frame->width, frame->height,
              pix_fmt_str, frame->ts);
    } else if (ret < 0 && ret != NMD_ERR_EOF) {
        LOG(ERROR, "failed to get frame: %s", get_nmd_ret_name(ret));
    }
    s->frame = frame;
    return 0;
}

static void release_player(void *data, void *shared_data, uint32_t thread_index)
{
    struct release_job *job = data;

    nmd_freep(&job->player);
}

static void media_release(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct media_priv *s = node->priv_data;

    nmd_frame_releasep(&s->frame);

    struct ngli_fence *release_fence = &s->release_fence;
    ngli_fence_wait(release_fence);

    struct release_job *release_job = &s->release_job;
    release_job->player = s->player;
    s->player = NULL;
    s->duration = 0.0;
    s->timebase = INFINITY;

    ngli_queue_add_job(&ctx->background_queue, release_job, release_fence, release_player, NULL);
}

static void media_uninit(struct ngl_node *node)
{
    struct media_priv *s = node->priv_data;

    ngli_fence_wait(&s->duration_fence);
    ngli_fence_destroy(&s->duration_fence);
    ngli_fence_wait(&s->release_fence);
    ngli_fence_destroy(&s->release_fence);
}

static int filename_changed(struct ngl_node *node)
{
    node->force_release_prefetch = true;

    return 0;
}

static void media_invalidate(struct ngl_node *node)
{
    struct media_priv *s = node->priv_data;

    s->loop_inputs[0].start = NAN;

    double start_time;
    double end_time;
    get_start_end_time(node, &start_time, &end_time);

    if (start_time == -DBL_MAX && end_time == -DBL_MAX)
        return;

    if (start_time == s->start_time && end_time == s->end_time)
        return;

    node->force_release_prefetch = true;
}

const struct node_class ngli_media_class = {
    .id        = NGL_NODE_MEDIA,
    .name      = "Media",
    .init      = media_init,
    .prefetch  = media_prefetch,
    .update    = media_update,
    .invalidate = media_invalidate,
    .set_loop_range = media_set_loop_range,
    .release   = media_release,
    .uninit    = media_uninit,
    .opts_size = sizeof(struct media_opts),
    .priv_size = sizeof(struct media_priv),
    .params    = media_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};
