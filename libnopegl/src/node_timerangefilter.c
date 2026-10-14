/*
 * Copyright 2024-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023 Nope Forge
 * Copyright 2017-2022 GoPro Inc.
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

#include <stddef.h>
#include <string.h>

#include "log.h"
#include "nopegl/nopegl.h"
#include "internal.h"
#include "params.h"
#include "timerange.h"

struct timerangefilter_opts {
    struct ngl_node *child;
    double start_time;
    double end_time;
    double render_time;
    double prefetch_time;
};

struct timerangefilter_priv {
    struct ngli_timerange_filter range;
    int drawme;
};

static int update_params(struct ngl_node *node);

#define OFFSET(x) offsetof(struct timerangefilter_opts, x)
static const struct node_param timerangefilter_params[] = {
    {"child", NGLI_PARAM_TYPE_NODE, OFFSET(child), .flags=NGLI_PARAM_FLAG_NON_NULL,
              .desc=NGLI_DOCSTRING("time filtered scene")},
    {"start",         NGLI_PARAM_TYPE_F64, OFFSET(start_time),
                      .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                      .update_func=update_params,
                      .desc=NGLI_DOCSTRING("start time (included) for the scene to be drawn, a negative value is evaluated as 0")},
    {"end",           NGLI_PARAM_TYPE_F64, OFFSET(end_time), {.f64=-1.0},
                      .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                      .update_func=update_params,
                      .desc=NGLI_DOCSTRING("end time (excluded) for the scene to be drawn, a negative value implies forever; a value before `start` means an empty range")},
    {"render_time",   NGLI_PARAM_TYPE_F64, OFFSET(render_time), {.f64=-1.0},
                      .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                      .update_func=update_params,
                      .desc=NGLI_DOCSTRING("chosen time to draw for a \"once\" mode, negative to ignore")},
    {"prefetch_time", NGLI_PARAM_TYPE_F64, OFFSET(prefetch_time), {.f64=1.0},
                      .desc=NGLI_DOCSTRING("`child` is prefetched `prefetch_time` seconds in advance")},
    {NULL}
};

static void reset_children_timings(struct ngl_node *node)
{
    node->visit_time = -1;
    node->last_update_time = -1;
    for (size_t i = 0; i < node->children.count; i++) {
        struct ngl_node *child = node->children.data[i];
        reset_children_timings(child);
    }
}

static int update_params(struct ngl_node *node)
{
    struct timerangefilter_priv *s = node->priv_data;
    const struct timerangefilter_opts *o = node->opts;

    ngli_timerange_filter_init(&s->range, o->start_time, o->end_time, o->render_time, o->prefetch_time);

    /*
     * Ensure children are prefetched/released during the next draw if the
     * graph time hasn't changed.
     */
    reset_children_timings(node);

    return 0;
}

int ngl_timerangefilter_set_range(struct ngl_node *node, double start, double end)
{
    if (!node)
        return NGL_ERROR_INVALID_ARG;

    if (node->cls->id != NGL_NODE_TIMERANGEFILTER)
        return NGL_ERROR_UNSUPPORTED;

    int ret = ngli_node_check_not_traversing(node);
    if (ret < 0)
        return ret;

    struct timerangefilter_opts *o = node->opts;

    o->start_time = start;
    o->end_time = end;

    if (!node->ctx)
        return 0;

    ret = update_params(node);
    if (ret < 0)
        return ret;

    ngli_node_invalidate_branch(node);
    return 0;
}

static int timerangefilter_init(struct ngl_node *node)
{
    struct timerangefilter_priv *s = node->priv_data;
    const struct timerangefilter_opts *o = node->opts;

    ngli_timerange_filter_init(&s->range, o->start_time, o->end_time, o->render_time, o->prefetch_time);

    if (o->prefetch_time < 0) {
        LOG(ERROR, "prefetch time must be positive");
        return NGL_ERROR_INVALID_ARG;
    }

    return 0;
}

static int timerangefilter_visit(struct ngl_node *node, bool is_active, double t, const struct ngli_prefetch_range *range)
{
    struct timerangefilter_priv *s = node->priv_data;
    const struct timerangefilter_opts *o = node->opts;
    const struct ngl_ctx *ctx = node->ctx;

    struct ngli_prefetch_range child_range = *range;
    is_active = ngli_timerange_filter_visit(&s->range, t, is_active, o->child->is_active,
                                            &ctx->loop_range, &child_range);
    return ngli_node_visit(o->child, is_active, t, &child_range);
}

static int timerangefilter_update(struct ngl_node *node, double t)
{
    struct timerangefilter_priv *s = node->priv_data;
    const struct timerangefilter_opts *o = node->opts;

    s->drawme = ngli_timerange_filter_update(&s->range, &t);
    if (!s->drawme)
        return 0;

    return ngli_node_update(o->child, t);
}

static void timerangefilter_pre_draw(struct ngl_node *node)
{
    struct timerangefilter_priv *s = node->priv_data;
    const struct timerangefilter_opts *o = node->opts;

    if (!s->drawme)
        return;

    ngli_node_pre_draw(o->child);
}

static void timerangefilter_draw(struct ngl_node *node)
{
    struct timerangefilter_priv *s = node->priv_data;
    const struct timerangefilter_opts *o = node->opts;

    if (!s->drawme) {
        TRACE("%s @ %p with range [%f,%f) not marked for drawing, skip it", node->label, node, o->start_time, o->end_time);
        return;
    }

    ngli_node_draw(o->child);
}

const struct node_class ngli_timerangefilter_class = {
    .id        = NGL_NODE_TIMERANGEFILTER,
    .name      = "TimeRangeFilter",
    .init      = timerangefilter_init,
    .visit     = timerangefilter_visit,
    .update    = timerangefilter_update,
    .pre_draw  = timerangefilter_pre_draw,
    .draw      = timerangefilter_draw,
    .opts_size = sizeof(struct timerangefilter_opts),
    .priv_size = sizeof(struct timerangefilter_priv),
    .params    = timerangefilter_params,
    .file      = __FILE__,
};
