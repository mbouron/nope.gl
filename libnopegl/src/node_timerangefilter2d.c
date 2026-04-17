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

#include <stddef.h>
#include <string.h>

#include "internal.h"
#include "log.h"
#include "node2d.h"
#include "nopegl/nopegl.h"
#include "params.h"

struct timerangefilter2d_opts {
    struct ngl_node *child;
    double start_time;
    double end_time;
    double render_time;
    double prefetch_time;
};

struct timerangefilter2d_priv {
    struct ngli_node2d_info node2d_info;
    int updated;
    int drawme;
};

static int update_params(struct ngl_node *node);

#define OFFSET(x) offsetof(struct timerangefilter2d_opts, x)
static const struct node_param timerangefilter2d_params[] = {
    {
        .key        = "child",
        .type       = NGLI_PARAM_TYPE_NODE,
        .offset     = OFFSET(child),
        .flags      = NGLI_PARAM_FLAG_NON_NULL,
        .node_types = NGLI_NODE2D_TYPES_LIST,
        .desc       = NGLI_DOCSTRING("time filtered scene")
    },
    {
        .key         = "start",
        .type        = NGLI_PARAM_TYPE_F64,
        .offset      = OFFSET(start_time),
        .flags       = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .update_func = update_params,
        .desc        = NGLI_DOCSTRING("start time (included) for the scene to be drawn")
    },
    {
        .key         = "end",
        .type        = NGLI_PARAM_TYPE_F64,
        .offset      = OFFSET(end_time),
        .def_value   = {.f64=-1.0},
        .flags       = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .update_func = update_params,
        .desc        = NGLI_DOCSTRING("end time (excluded) for the scene to be drawn, a negative value implies forever")
    },
    {
        .key         = "render_time",
        .type        = NGLI_PARAM_TYPE_F64,
        .offset      = OFFSET(render_time),
        .def_value   = {.f64=-1.0},
        .flags       = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .update_func = update_params,
        .desc        = NGLI_DOCSTRING("chosen time to draw for a \"once\" mode, negative to ignore")
    },
    {
        .key         = "prefetch_time",
        .type        = NGLI_PARAM_TYPE_F64,
        .offset      = OFFSET(prefetch_time),
        .def_value   = {.f64=1.0},
        .desc        = NGLI_DOCSTRING("`child` is prefetched `prefetch_time` seconds in advance")
    },
    {NULL}
};

static void reset_children_timings(struct ngl_node *node)
{
    node->visit_time = -1;
    node->last_update_time = -1;
    struct ngl_node **children = ngli_darray_data(&node->children);
    for (size_t i = 0; i < ngli_darray_count(&node->children); i++) {
        struct ngl_node *child = children[i];
        reset_children_timings(child);
    }
}

static int update_params(struct ngl_node *node)
{
    struct timerangefilter2d_opts *o = node->opts;

    if (o->start_time < 0.0) {
        LOG(WARNING, "start time cannot be negative, clamping");
        o->start_time = 0;
    }

    if (o->end_time >= 0.0 && o->end_time < o->start_time) {
        LOG(ERROR, "end time must be after start time, clamping");
        o->end_time = o->start_time;
    }

    /*
     * Ensure children are prefetched/released during the next draw if the
     * graph time hasn't changed.
     */
    reset_children_timings(node);

    return 0;
}

int ngl_timerangefilter2d_set_range(struct ngl_node *node, double start, double end)
{
    if (!node)
        return NGL_ERROR_INVALID_ARG;

    if (node->cls->id != NGL_NODE_TIMERANGEFILTER2D)
        return NGL_ERROR_UNSUPPORTED;

    struct timerangefilter2d_opts *o = node->opts;

    o->start_time = start;
    o->end_time = end;

    if (!node->ctx)
        return 0;

    int ret = update_params(node);
    if (ret < 0)
        return ret;

    return ngli_node_invalidate_branch(node);
}

static int timerangefilter2d_init(struct ngl_node *node)
{
    const struct timerangefilter2d_opts *o = node->opts;

    if (o->end_time >= 0.0 && o->end_time < o->start_time) {
        LOG(ERROR, "end time must be after start time");
        return NGL_ERROR_INVALID_ARG;
    }

    if (o->start_time < 0.0) {
        LOG(ERROR, "start time cannot be negative");
        return NGL_ERROR_INVALID_ARG;
    }

    if (o->prefetch_time < 0) {
        LOG(ERROR, "prefetch time must be positive");
        return NGL_ERROR_INVALID_ARG;
    }

    return 0;
}

static int timerangefilter2d_visit(struct ngl_node *node, bool is_active, double t)
{
    struct timerangefilter2d_priv *s = node->priv_data;
    const struct timerangefilter2d_opts *o = node->opts;
    struct ngl_node *child = o->child;

    /*
     * The life of the parent takes over the life of its children: if the
     * parent is dead, the children are likely dead as well. However, a living
     * children from a dead parent can be revealed by another living branch.
     */
    if (is_active) {
        if (t < o->start_time - o->prefetch_time || (o->end_time >= 0.0 && t >= o->end_time))
            is_active = false;

        // If the child of the current once range is inactive, meaning
        // it has been previously released, we need to force an update
        // otherwise the child will stay uninitialized.
        if (!child->is_active)
            s->updated = 0;
    }

    return ngli_node_visit(child, is_active, t);
}

static int timerangefilter2d_update(struct ngl_node *node, double t)
{
    struct timerangefilter2d_priv *s = node->priv_data;
    const struct timerangefilter2d_opts *o = node->opts;

    s->drawme = 0;

    if (t < o->start_time || (o->end_time >= 0.0 && t >= o->end_time))
        return 0;

    if (o->render_time >= 0.0) {
        if (s->updated)
            return 0;
        t = o->render_time;
        s->updated = 1;
    }

    s->drawme = 1;

    return ngli_node_update(o->child, t);
}

static void timerangefilter2d_pre_draw(struct ngl_node *node)
{
    struct timerangefilter2d_priv *s = node->priv_data;
    const struct timerangefilter2d_opts *o = node->opts;

    if (!s->drawme) {
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    ngli_node_pre_draw(o->child);

    s->node2d_info.screen_aabb = ngli_node_compute_children_bounding_box(&o->child, 1);
}

static void timerangefilter2d_draw(struct ngl_node *node)
{
    struct timerangefilter2d_priv *s = node->priv_data;
    const struct timerangefilter2d_opts *o = node->opts;

    if (!s->drawme) {
        TRACE("%s @ %p with range [%f,%f) not marked for drawing, skip it", node->label, node, o->start_time, o->end_time);
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    ngli_node_draw(o->child);

    s->node2d_info.screen_aabb = ngli_node_compute_children_bounding_box(&o->child, 1);
}

const struct node_class ngli_timerangefilter2d_class = {
    .id        = NGL_NODE_TIMERANGEFILTER2D,
    .name      = "TimeRangeFilter2D",
    .init      = timerangefilter2d_init,
    .visit     = timerangefilter2d_visit,
    .update    = timerangefilter2d_update,
    .pre_draw  = timerangefilter2d_pre_draw,
    .draw      = timerangefilter2d_draw,
    .opts_size = sizeof(struct timerangefilter2d_opts),
    .priv_size = sizeof(struct timerangefilter2d_priv),
    .params    = timerangefilter2d_params,
    .flags     = NGLI_NODE_FLAG_2D,
    .file      = __FILE__,
};
