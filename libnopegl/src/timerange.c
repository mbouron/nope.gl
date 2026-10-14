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

#include <math.h>

#include "log.h"
#include "timerange.h"

struct ngli_timerange ngli_timerange_sanitize(double start, double end)
{
    if (start < 0.0) {
        LOG(DEBUG, "start time %g is negative, evaluated as 0", start);
        start = 0.0;
    }
    if (end < 0.0) {
        end = INFINITY;
    } else if (end < start) {
        LOG(DEBUG, "end time %g is before start time %g, the range is empty", end, start);
        end = start;
    }
    return (struct ngli_timerange){start, end};
}

bool ngli_timerange_contains(const struct ngli_timerange *range, double t)
{
    return t >= range->start && t < range->end;
}

void ngli_timerange_filter_prefetch(struct ngli_prefetch_range *range, const struct ngli_timerange *filter_range,
                                    double render_time)
{
    struct ngli_timerange *update = &range->update;
    if (range->render_time >= 0.0) {
        /*
         * The parent passes a fixed timestamp to this filter. The child can
         * only be updated if that timestamp falls within this filter's range.
         */
        if (!ngli_timerange_contains(filter_range, range->render_time))
            update->end = update->start;
    } else {
        if (filter_range->start > update->start)
            update->start = filter_range->start;
        if (filter_range->end < update->end)
            update->end = filter_range->end;
    }
    if (render_time >= 0.0)
        range->render_time = render_time;
}

bool ngli_timerange_get_prefetch_time(const struct ngli_prefetch_range *range,
                                     double t, const struct ngli_timerange *loop,
                                     double *time, double *delay)
{
    const struct ngli_timerange *update = &range->update;
    if (update->end <= update->start)
        return false;

    double next = t > update->start ? t : update->start;
    double wait = next - t;
    const bool can_loop = loop->end > loop->start && t < loop->end;
    if (next >= update->end || (can_loop && next >= loop->end)) {
        next = loop->start > update->start ? loop->start : update->start;
        if (!can_loop || next >= loop->end || next >= update->end)
            return false;
        wait = loop->end - t + next - loop->start;
    }
    *time = range->render_time >= 0.0 ? range->render_time : next;
    *delay = wait;
    return true;
}

/*
 * Keep ranges covering loop->start active to retain their resources across
 * the wrap. Other ranges reached by the loop are prefetched before playback
 * returns to them, accounting for the time left before the wrap.
 */
bool ngli_timerange_is_active(const struct ngli_timerange *range, double t, double prefetch_time,
                              const struct ngli_timerange *loop)
{
    const bool looping = loop->end > loop->start && t < loop->end;
    if (looping && range->start >= loop->end)
        return false;
    if (t >= range->start - prefetch_time && t < range->end)
        return true;
    if (!looping || range->end <= loop->start)
        return false;
    if (range->start <= loop->start && t >= loop->start)
        return true;
    const double back_time = loop->end + (range->start > loop->start ? range->start - loop->start : 0.0);
    return t >= back_time - prefetch_time;
}

void ngli_timerange_filter_init(struct ngli_timerange_filter *s, double start, double end,
                                double render_time, double prefetch_time)
{
    s->range = ngli_timerange_sanitize(start, end);
    s->render_time = render_time;
    s->prefetch_time = prefetch_time;
}

bool ngli_timerange_filter_visit(struct ngli_timerange_filter *s, double t, bool is_active,
                                 bool child_is_active, const struct ngli_timerange *loop,
                                 struct ngli_prefetch_range *child_range)
{
    /*
     * The life of the parent takes over the life of its children: if the
     * parent is dead, the children are likely dead as well. However, a living
     * children from a dead parent can be revealed by another living branch.
     */
    if (is_active) {
        /* Use parent `render_time` if set */
        const double render_time = child_range->render_time;
        if (render_time >= 0.0)
            is_active = ngli_timerange_contains(&s->range, render_time);
        else
            is_active = ngli_timerange_is_active(&s->range, t, s->prefetch_time, loop);

        /* A released child needs an update when prefetched again. */
        if (!child_is_active)
            s->updated = 0;
    }

    ngli_timerange_filter_prefetch(child_range, &s->range, s->render_time);

    /*
     * Reset the once state when playback leaves the range so the child can be
     * updated again on return, even if it stayed prefetched.
     *
     * We do this during visits because a parent can skip updates outside its
     * own range.
     */
    if (!is_active || !ngli_timerange_contains(&child_range->update, t))
        s->updated = 0;

    return is_active;
}

bool ngli_timerange_filter_update(struct ngli_timerange_filter *s, double *t)
{
    if (!ngli_timerange_contains(&s->range, *t))
        return false;

    if (s->render_time >= 0.0) {
        if (s->updated)
            return false;
        *t = s->render_time;
        s->updated = 1;
    }

    return true;
}
