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
#include <stddef.h>

#include "internal.h"
#include "log.h"
#include "node_paint.h"
#include "node_stroke2d.h"
#include "node_uniform.h"
#include "nopegl/nopegl.h"

struct stroke2d_opts {
    struct ngl_node *paint;
    struct ngl_node *width_node;
    float width;
    int alignment;
};

struct stroke2d_priv {
    struct stroke2d_info info;
};

static const struct param_choices stroke2d_alignment_choices = {
    .name = "stroke2d_alignment",
    .consts = {
        {"inside",  NGLI_STROKE2D_ALIGNMENT_INSIDE,  .desc = NGLI_DOCSTRING("stroke lies inside the shape boundary")},
        {"center",  NGLI_STROKE2D_ALIGNMENT_CENTER,  .desc = NGLI_DOCSTRING("stroke is centered on the shape boundary")},
        {"outside", NGLI_STROKE2D_ALIGNMENT_OUTSIDE, .desc = NGLI_DOCSTRING("stroke lies outside the shape boundary")},
        {NULL},
    },
};

static void update_stroke2d_info(struct ngl_node *node)
{
    const struct stroke2d_opts *o = node->opts;
    struct stroke2d_priv *s = node->priv_data;
    s->info = (struct stroke2d_info) {
        .paint             = o->paint,
        .width_node        = o->width_node,
        .width             = o->width,
        .alignment         = o->alignment,
    };
}

/*
 * Only the direct value is rejected: a width driven by a variable node changes
 * over time, so an out of range value must not fail the frame.  Such values are
 * clamped by ngli_stroke2d_get_width() instead.
 */
static int validate_stroke2d(struct ngl_node *node)
{
    const struct stroke2d_opts *o = node->opts;
    if (o->width_node)
        return 0;
    if (!isfinite(o->width) || o->width < 0.f) {
        LOG(ERROR, "Stroke2D.width must be finite and non-negative");
        return NGL_ERROR_INVALID_ARG;
    }
    return 0;
}

static int stroke2d_update(struct ngl_node *node, double t)
{
    int ret = ngli_node_update_children(node, t);
    if (ret < 0)
        return ret;
    update_stroke2d_info(node);
    return 0;
}

static int stroke2d_init(struct ngl_node *node)
{
    int ret = validate_stroke2d(node);
    if (ret < 0)
        return ret;
    update_stroke2d_info(node);
    return 0;
}

#define OFFSET(x) offsetof(struct stroke2d_opts, x)
static const struct node_param stroke2d_params[] = {
    {
        .key = "paint",
        .type = NGLI_PARAM_TYPE_NODE,
        .offset = OFFSET(paint),
        .node_types = ngli_paint_node_types,
        .flags = NGLI_PARAM_FLAG_NON_NULL,
        .desc = NGLI_DOCSTRING("paint applied to the stroke geometry"),
    }, {
        .key = "width",
        .type = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(width_node),
        .def_value = {.f32 = 1.f},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .update_func = validate_stroke2d,
        .desc = NGLI_DOCSTRING("stroke width in canvas pixels"),
    }, {
        .key = "alignment",
        .type = NGLI_PARAM_TYPE_SELECT,
        .offset = OFFSET(alignment),
        .def_value = {.i32 = NGLI_STROKE2D_ALIGNMENT_CENTER},
        .choices = &stroke2d_alignment_choices,
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("placement relative to the shape boundary"),
    },
    {NULL},
};
#undef OFFSET

float ngli_stroke2d_get_width(const struct stroke2d_info *info)
{
    if (!info)
        return 0.f;
    const float width = *(const float *)ngli_node_get_data_ptr(info->width_node, &info->width);
    return isfinite(width) && width > 0.f ? width : 0.f;
}

float ngli_stroke2d_get_outer_edge(const struct stroke2d_info *info)
{
    if (!info || info->alignment == NGLI_STROKE2D_ALIGNMENT_INSIDE)
        return 0.f;

    const float width = ngli_stroke2d_get_width(info);
    return info->alignment == NGLI_STROKE2D_ALIGNMENT_CENTER ? width * 0.5f : width;
}

const struct stroke2d_info *ngli_stroke2d_get_info(const struct ngl_node *node)
{
    ngli_assert(node->cls->id == NGL_NODE_STROKE2D);
    const struct stroke2d_priv *s = node->priv_data;
    return &s->info;
}

const struct node_class ngli_stroke2d_class = {
    .id = NGL_NODE_STROKE2D,
    .name = "Stroke2D",
    .init = stroke2d_init,
    .update = stroke2d_update,
    .opts_size = sizeof(struct stroke2d_opts),
    .priv_size = sizeof(struct stroke2d_priv),
    .params = stroke2d_params,
    .flags = NGLI_NODE_FLAG_SHAREABLE,
    .file = __FILE__,
};
