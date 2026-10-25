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
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>

#include "loop.h"

/* Reserve an hour on each side of the reported media range for timestamps outside it. */
#define PASS_MARGIN (3600LL * AV_TIME_BASE)

int nmdi_loop_init(struct nmdi_loop *s)
{
    s->start = AV_NOPTS_VALUE;
    s->end = AV_NOPTS_VALUE;
    s->loop_rev = 0;
    s->seek_rev = 0;
    s->ended_loop_rev = -1;
    s->nb_passes = 0;
    s->passes_size = 16;
    s->passes = av_malloc_array(s->passes_size, sizeof(*s->passes));
    if (!s->passes)
        return AVERROR(ENOMEM);
    int ret = AVERROR(pthread_mutex_init(&s->lock, NULL));
    if (ret < 0)
        av_freep(&s->passes);
    return ret;
}

int64_t nmdi_loop_set(struct nmdi_loop *s, int64_t start, int64_t end)
{
    pthread_mutex_lock(&s->lock);
    s->start = start;
    s->end = end;
    const int64_t loop_rev = ++s->loop_rev;
    pthread_mutex_unlock(&s->lock);
    return loop_rev;
}

void nmdi_loop_get(struct nmdi_loop *s, int64_t *start, int64_t *end, int64_t *loop_rev)
{
    pthread_mutex_lock(&s->lock);
    *start = s->start;
    *end = s->end;
    *loop_rev = s->loop_rev;
    pthread_mutex_unlock(&s->lock);
}

static int add_pass(struct nmdi_loop *s, int64_t pass, int64_t loop_rev)
{
    if (s->nb_passes == s->passes_size) {
        const size_t size = s->passes_size * 2;
        struct nmdi_pass_record *passes = av_realloc_array(s->passes, size, sizeof(*passes));
        if (!passes)
            return AVERROR(ENOMEM);
        s->passes = passes;
        s->passes_size = size;
    }
    s->passes[s->nb_passes++] = (struct nmdi_pass_record){
        .pass     = pass,
        .loop_rev = loop_rev,
        .end      = AV_NOPTS_VALUE,
        .drop     = INT64_MAX,
    };
    return 0;
}

static struct nmdi_pass_record *find_pass(struct nmdi_loop *s, int64_t pass)
{
    if (!s->nb_passes || pass < s->passes[0].pass || pass - s->passes[0].pass >= (int64_t)s->nb_passes)
        return NULL;
    struct nmdi_pass_record *record = &s->passes[pass - s->passes[0].pass];
    return record->pass == pass ? record : NULL;
}

/* Restart pass numbering; advancing `seek_rev` distinguishes old and new pass 0. */
int64_t nmdi_loop_reset_passes(struct nmdi_loop *s)
{
    pthread_mutex_lock(&s->lock);
    s->nb_passes = 0;
    add_pass(s, 0, -1); // within the initial allocation
    s->ended_loop_rev = -1;
    const int64_t seek_rev = ++s->seek_rev;
    pthread_mutex_unlock(&s->lock);
    return seek_rev;
}

/* Record where the previous pass stopped supplying packets, then start the next pass. */
int nmdi_loop_start_pass(struct nmdi_loop *s, int64_t pass, int64_t loop_rev, int64_t prev_end)
{
    pthread_mutex_lock(&s->lock);
    struct nmdi_pass_record *prev = find_pass(s, pass - 1);
    if (prev)
        prev->end = prev_end;
    const int ret = add_pass(s, pass, loop_rev);
    pthread_mutex_unlock(&s->lock);
    return ret;
}

/*
 * Remember the earliest frame timestamp discarded at or after the pass end.
 * Only accept reports from the current `seek_rev`, so work from before a seek
 * cannot change the records for the new sequence of passes.
 */
void nmdi_loop_drop_frame(struct nmdi_loop *s, int64_t seek_rev, int64_t pass, int64_t ts)
{
    pthread_mutex_lock(&s->lock);
    struct nmdi_pass_record *record = seek_rev == s->seek_rev ? find_pass(s, pass) : NULL;
    if (record)
        record->drop = FFMIN(record->drop, ts);
    pthread_mutex_unlock(&s->lock);
}

/* Range revision used to start a pass; -1 for pass 0 or a pass with no record. */
int64_t nmdi_loop_get_pass_loop_rev(struct nmdi_loop *s, int64_t pass)
{
    pthread_mutex_lock(&s->lock);
    const struct nmdi_pass_record *record = find_pass(s, pass);
    const int64_t loop_rev = record ? record->loop_rev : -1;
    pthread_mutex_unlock(&s->lock);
    return loop_rev;
}

/*
 * For a completed pass, find the first timestamp it cannot supply. Use the
 * earlier of the first packet not sent and the earliest frame dropped.
 * `INT64_MAX` means no known cutoff, including EOF without dropped frames.
 *
 * For example, extending [2s, 5s) to [2s, 8s) cannot recover frames already
 * discarded at 5s. The API uses this boundary to decide when it must seek.
 */
int64_t nmdi_loop_get_pass_end(struct nmdi_loop *s, int64_t pass)
{
    pthread_mutex_lock(&s->lock);
    const struct nmdi_pass_record *record = find_pass(s, pass);
    const int64_t end = record && record->end != AV_NOPTS_VALUE ? FFMIN(record->end, record->drop) : INT64_MAX;
    pthread_mutex_unlock(&s->lock);
    return end;
}

/*
 * Stop playback after a full pass with no filtered output, but only if it
 * used the current range revision. A newer range may still produce frames.
 */
int nmdi_loop_end_empty_pass(struct nmdi_loop *s, int64_t pass)
{
    pthread_mutex_lock(&s->lock);
    const struct nmdi_pass_record *record = find_pass(s, pass);
    const int64_t loop_rev = record ? record->loop_rev : s->loop_rev;
    const int end = loop_rev == s->loop_rev;
    if (end)
        s->ended_loop_rev = loop_rev;
    pthread_mutex_unlock(&s->lock);
    return end;
}

/* Range revision that stopped playback after a full pass with no filtered output; -1 if none. */
int64_t nmdi_loop_get_ended_loop_rev(struct nmdi_loop *s)
{
    pthread_mutex_lock(&s->lock);
    const int64_t loop_rev = s->ended_loop_rev;
    pthread_mutex_unlock(&s->lock);
    return loop_rev;
}

/* The API has reached `pass`, so it no longer needs records for earlier passes. */
void nmdi_loop_release_passes(struct nmdi_loop *s, int64_t pass)
{
    pthread_mutex_lock(&s->lock);
    size_t nb = 0;
    while (nb < s->nb_passes && s->passes[nb].pass < pass)
        nb++;
    if (nb) {
        s->nb_passes -= nb;
        memmove(s->passes, s->passes + nb, s->nb_passes * sizeof(*s->passes));
    }
    pthread_mutex_unlock(&s->lock);
}

void nmdi_loop_uninit(struct nmdi_loop *s)
{
    av_freep(&s->passes);
    pthread_mutex_destroy(&s->lock);
}

/*
 * Convert the exclusive loop end from microseconds to stream ticks, rounding
 * up so frames before the end remain eligible. If the loop extends beyond
 * the `end_time` option, include the frame normal playback would hold there:
 * place the cutoff one tick after the rounded `end_time`.
 */
int64_t nmdi_loop_stream_end(int64_t end, int64_t end_time, AVRational time_base)
{
    if (end_time != AV_NOPTS_VALUE && end > end_time)
        return av_rescale_q(end_time, AV_TIME_BASE_Q, time_base) + 1;
    return av_rescale_q_rnd(end, AV_TIME_BASE_Q, time_base, AV_ROUND_UP);
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

    /* Keep the offset timestamps far from overflowing, in microseconds as well */
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
