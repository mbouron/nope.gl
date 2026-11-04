/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

#include <string.h>

#include <libavutil/avutil.h>
#include <libavutil/common.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>

#include "loop.h"

/* Allow for timestamps outside the reported media range. */
#define PASS_MARGIN (3600LL * AV_TIME_BASE)

/*
 * Return 1 if ts is strictly before the effective pass end, 0 otherwise.
 * The effective end is the earlier of end and drop. Use INT64_MAX if end is
 * unknown or the pass reached EOF without dropped frames.
 * ts uses the stream time base, without the pass offset.
 */
int nmdi_pass_ends_after(const struct nmdi_pass *pass, int64_t ts)
{
    const int64_t end = pass->end != AV_NOPTS_VALUE ? FFMIN(pass->end, pass->drop) : INT64_MAX;
    return ts < end;
}

int nmdi_loop_init(struct nmdi_loop *s)
{
    s->playback_start = 0;
    s->playback_end = AV_NOPTS_VALUE;
    s->playback_stopped_at = AV_NOPTS_VALUE;
    s->loop_start = AV_NOPTS_VALUE;
    s->loop_end = AV_NOPTS_VALUE;
    s->loop_rev = 0;
    s->seek_rev = 0;
    s->empty_pass = (struct nmdi_pass){.loop_rev = -1};
    s->passes_count = 0;
    s->passes_capacity = 16;
    s->passes = av_malloc_array(s->passes_capacity, sizeof(*s->passes));
    if (!s->passes)
        return AVERROR(ENOMEM);
    int ret = AVERROR(pthread_mutex_init(&s->lock, NULL));
    if (ret < 0)
        av_freep(&s->passes);
    return ret;
}

void nmdi_loop_uninit(struct nmdi_loop *s)
{
    av_freep(&s->passes);
    pthread_mutex_destroy(&s->lock);
}

void nmdi_loop_set_playback_range(struct nmdi_loop *s, int64_t start, int64_t end)
{
    pthread_mutex_lock(&s->lock);
    s->playback_start = start;
    s->playback_end = end;
    pthread_mutex_unlock(&s->lock);
}

void nmdi_loop_get_playback_range(struct nmdi_loop *s, int64_t *start, int64_t *end)
{
    pthread_mutex_lock(&s->lock);
    *start = s->playback_start;
    *end = s->playback_end;
    pthread_mutex_unlock(&s->lock);
}

void nmdi_loop_set_playback_stopped_at(struct nmdi_loop *s, int64_t end)
{
    pthread_mutex_lock(&s->lock);
    s->playback_stopped_at = end;
    pthread_mutex_unlock(&s->lock);
}

int64_t nmdi_loop_get_playback_stopped_at(struct nmdi_loop *s)
{
    pthread_mutex_lock(&s->lock);
    const int64_t end = s->playback_stopped_at;
    pthread_mutex_unlock(&s->lock);
    return end;
}

int64_t nmdi_loop_set(struct nmdi_loop *s, int64_t start, int64_t end)
{
    pthread_mutex_lock(&s->lock);
    s->loop_start = start;
    s->loop_end = end;
    const int64_t loop_rev = ++s->loop_rev;
    pthread_mutex_unlock(&s->lock);
    return loop_rev;
}

void nmdi_loop_get(struct nmdi_loop *s, int64_t *start, int64_t *end, int64_t *loop_rev)
{
    pthread_mutex_lock(&s->lock);
    *start = s->loop_start;
    *end = s->loop_end;
    *loop_rev = s->loop_rev;
    pthread_mutex_unlock(&s->lock);
}

/*
 * Convert the exclusive loop end from microseconds to stream ticks.
 * Round up to keep frames before the end. If the loop extends beyond
 * `end_time`, use one tick after the rounded `end_time` to include the frame
 * normally displayed there.
 */
int64_t nmdi_loop_stream_end(int64_t end, int64_t end_time, AVRational time_base)
{
    if (end_time != AV_NOPTS_VALUE && end > end_time)
        return av_rescale_q(end_time, AV_TIME_BASE_Q, time_base) + 1;
    return av_rescale_q_rnd(end, AV_TIME_BASE_Q, time_base, AV_ROUND_UP);
}

static int add_pass(struct nmdi_loop *s, int64_t pass_number, int64_t loop_rev)
{
    if (s->passes_count == s->passes_capacity) {
        const size_t size = s->passes_capacity * 2;
        struct nmdi_pass *passes = av_realloc_array(s->passes, size, sizeof(*passes));
        if (!passes)
            return AVERROR(ENOMEM);
        s->passes = passes;
        s->passes_capacity = size;
    }
    s->passes[s->passes_count++] = (struct nmdi_pass){
        .number   = pass_number,
        .loop_rev = loop_rev,
        .end      = AV_NOPTS_VALUE,
        .drop     = INT64_MAX,
    };
    return 0;
}

static struct nmdi_pass *find_pass(struct nmdi_loop *s, int64_t pass_number)
{
    if (!s->passes_count || pass_number < s->passes[0].number || pass_number - s->passes[0].number >= (int64_t)s->passes_count)
        return NULL;
    struct nmdi_pass *pass = &s->passes[pass_number - s->passes[0].number];
    return pass->number == pass_number ? pass : NULL;
}

/* Reset pass numbering and advance `seek_rev` to distinguish old and new passes. */
int64_t nmdi_loop_reset_passes(struct nmdi_loop *s)
{
    pthread_mutex_lock(&s->lock);
    s->passes_count = 0;
    add_pass(s, 0, -1); // the initial allocation always has room for pass 0
    s->empty_pass = (struct nmdi_pass){.loop_rev = -1};
    const int64_t seek_rev = ++s->seek_rev;
    pthread_mutex_unlock(&s->lock);
    return seek_rev;
}

/* Record the previous pass's packet cutoff and start the next pass. */
int nmdi_loop_start_pass(struct nmdi_loop *s, int64_t pass_number, int64_t loop_rev, int64_t prev_end)
{
    pthread_mutex_lock(&s->lock);
    struct nmdi_pass *prev = find_pass(s, pass_number - 1);
    if (prev)
        prev->end = prev_end;
    const int ret = add_pass(s, pass_number, loop_rev);
    pthread_mutex_unlock(&s->lock);
    return ret;
}

/*
 * Track the earliest dropped frame to know which requests this pass can cover.
 * Ignore reports from before the latest seek.
 */
void nmdi_loop_drop_frame(struct nmdi_loop *s, int64_t seek_rev, int64_t pass_number, int64_t ts)
{
    pthread_mutex_lock(&s->lock);
    struct nmdi_pass *pass = seek_rev == s->seek_rev ? find_pass(s, pass_number) : NULL;
    if (pass)
        pass->drop = FFMIN(pass->drop, ts);
    pthread_mutex_unlock(&s->lock);
}

/* Copy the pass if retained. Return 1 on success, or 0 without changing dst. */
int nmdi_loop_get_pass(struct nmdi_loop *s, int64_t pass_number, struct nmdi_pass *dst)
{
    pthread_mutex_lock(&s->lock);
    const struct nmdi_pass *pass = find_pass(s, pass_number);
    const int found = pass != NULL;
    if (found)
        *dst = *pass;
    pthread_mutex_unlock(&s->lock);
    return found;
}

/* Release passes before `pass_number`. */
void nmdi_loop_release_pass_history(struct nmdi_loop *s, int64_t pass_number)
{
    pthread_mutex_lock(&s->lock);
    size_t nb = 0;
    while (nb < s->passes_count && s->passes[nb].number < pass_number)
        nb++;
    if (nb) {
        s->passes_count -= nb;
        memmove(s->passes, s->passes + nb, s->passes_count * sizeof(*s->passes));
    }
    pthread_mutex_unlock(&s->lock);
}

/*
 * If its loop range is still current, record the empty full pass and return 1
 * to report EOF. Otherwise return 0, since the new range may produce frames.
 * If its history was released, assume the current range.
 */
int nmdi_loop_report_empty_pass(struct nmdi_loop *s, int64_t pass_number)
{
    pthread_mutex_lock(&s->lock);
    const struct nmdi_pass *pass = find_pass(s, pass_number);
    const int64_t loop_rev = pass ? pass->loop_rev : s->loop_rev;
    const int end = loop_rev == s->loop_rev;
    if (end)
        s->empty_pass = pass ? *pass : (struct nmdi_pass){
            .number   = pass_number,
            .loop_rev = loop_rev,
            .end      = AV_NOPTS_VALUE,
            .drop     = INT64_MAX,
        };
    pthread_mutex_unlock(&s->lock);
    return end;
}

/*
 * Copy the pass that stopped playback with no filtered frames. Return 1 if
 * found, or 0 without changing dst.
 *
 * If its history was released before it was reported, the snapshot uses the
 * reported number and the loop revision at report time, with end set to
 * AV_NOPTS_VALUE and drop to INT64_MAX.
 */
int nmdi_loop_get_empty_pass(struct nmdi_loop *s, struct nmdi_pass *dst)
{
    pthread_mutex_lock(&s->lock);
    const int found = s->empty_pass.loop_rev >= 0;
    if (found)
        *dst = s->empty_pass;
    pthread_mutex_unlock(&s->lock);
    return found;
}

void nmdi_pass_timeline_init(struct nmdi_pass_timeline *s, const AVStream *st, int64_t duration)
{
    *s = (struct nmdi_pass_timeline){0};

    if (duration == AV_NOPTS_VALUE || duration < 0 || !st->time_base.num || !st->time_base.den)
        return;

    const int64_t margin = av_rescale_q_rnd(PASS_MARGIN, AV_TIME_BASE_Q, st->time_base, AV_ROUND_UP);
    const int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    const int64_t length = av_rescale_q_rnd(duration, AV_TIME_BASE_Q, st->time_base, AV_ROUND_UP);
    s->origin = start - margin;
    s->stride = length + 2 * margin;
    s->end = start + length;

    /* Leave room for timestamp arithmetic in stream ticks and microseconds. */
    const int64_t stride_us = av_rescale_q_rnd(s->stride, st->time_base, AV_TIME_BASE_Q, AV_ROUND_UP);
    s->max_pass = INT64_MAX / 4 / FFMAX(FFMAX(s->stride, stride_us), 1);
}

int64_t nmdi_pass_from_timestamp(const struct nmdi_pass_timeline *s, int64_t ts)
{
    if (!s->stride || ts == AV_NOPTS_VALUE)
        return 0;
    const int64_t d = ts - s->origin;
    return d >= 0 ? d / s->stride : -((-d + s->stride - 1) / s->stride);
}
