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
#include "node_uniform.h"
#include "nopegl/nopegl.h"

struct group2d_opts {
    struct ngl_node **children;
    size_t nb_children;
    struct ngli_node2d_opts node2d;
    struct ngl_node *clip_rect_node;
    float clip_rect[4];
    struct ngl_node *clip_corner_radius_node;
    float clip_corner_radius[2];
};

struct group2d_priv {
    struct ngli_node2d_info node2d_info;
    int clip_overflow_warned;
};

#define OFFSET(x) offsetof(struct group2d_opts, x)
static const struct node_param group2d_params[] = {
    {
        .key       = "children",
        .type      = NGLI_PARAM_TYPE_NODELIST,
        .offset    = OFFSET(children),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .node_types = NGLI_NODE2D_TYPES_LIST,
        .desc      = NGLI_DOCSTRING("2D scenes to draw"),
    }, {
        .key       = "translate",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.translate_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("translation in pixels"),
    }, {
        .key       = "rotation",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(node2d.rotation_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("rotation angle in degrees"),
    }, {
        .key       = "scale",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.scale_node),
        .def_value = {.vec={1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("scale factors"),
    }, {
        .key       = "anchor",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.anchor_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("anchor/pivot point in pixels"),
    }, {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(node2d.opacity_node),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("opacity applied to all children"),
    }, {
        .key       = "visible",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(node2d.visible),
        .def_value = {.i32=1},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("whether the group and its children are visible"),
    }, {
        .key       = "clip_rect",
        .type      = NGLI_PARAM_TYPE_VEC4,
        .offset    = OFFSET(clip_rect_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("clipping rectangle (x, y, width, height) in the group's local "
                                    "pixel coordinates that cascades to all children; it is "
                                    "transformed by the group (so it follows the group's rotation, "
                                    "scale and translation) and intersected with any clip inherited "
                                    "from ancestor groups. When width or height is 0 clipping is "
                                    "disabled. The clip edge is anti-aliased and stays correct under "
                                    "rotation"),
    }, {
        .key       = "clip_corner_radius",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(clip_corner_radius_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("corner radii (x, y) of clip_rect in local pixels, for rounded "
                                    "clipping; (0, 0) gives sharp corners. Each radius is clamped to "
                                    "half the corresponding clip_rect side"),
    },
    {NULL}
};

static int group2d_init(struct ngl_node *node)
{
    return 0;
}

static int group2d_prepare(struct ngl_node *node,
                           const struct ngpu_graphics_state *graphics_state,
                           const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    return 0;
}

static void group2d_pre_draw(struct ngl_node *node)
{
    struct group2d_priv *s = node->priv_data;
    const struct group2d_opts *o = node->opts;

    if (!o->node2d.visible) {
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    /* Push composed transform + opacity */
    int ret = ngli_node2d_push_transform(node);
    if (ret < 0)
        return;

    /* Save local transform */
    struct ngli_mat4 local_transform_matrix = *(const struct ngli_mat4 *)ngli_darray_tail(&node->ctx->transform_2d_stack);

    /* Pre-draw children */
    for (size_t i = 0; i < o->nb_children; i++)
        ngli_node_pre_draw(o->children[i]);

    /* Compute bounding box from children */
    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    node2d_info->screen_aabb = ngli_node_compute_children_bounding_box(o->children, o->nb_children);
    node2d_info->transform_matrix = local_transform_matrix;

    /* Pop stacks */
    ngli_node2d_pop_transform(node);
}

static void group2d_push_clip(struct ngl_node *node)
{
    struct group2d_priv *s = node->priv_data;
    const struct group2d_opts *o = node->opts;
    struct ngl_ctx *ctx = node->ctx;

    const float *rect = ngli_node_get_data_ptr(o->clip_rect_node, o->clip_rect);
    if (rect[2] <= 0.f || rect[3] <= 0.f)
        return;

    const float *corner_radius = ngli_node_get_data_ptr(o->clip_corner_radius_node, o->clip_corner_radius);

    if (ctx->nb_clips_2d >= NGLI_MAX_CLIPS_2D) {
        if (!s->clip_overflow_warned) {
            LOG(WARNING, "too many nested Group2D clip rectangles "
                "(at most %d supported); ignoring this clip", NGLI_MAX_CLIPS_2D);
            s->clip_overflow_warned = 1;
        }
        return;
    }

    const struct ngli_mat4 *trs = ngli_darray_tail(&ctx->transform_2d_stack);

    const size_t k = ctx->nb_clips_2d;
    if (!ngli_node2d_compute_clip(trs, rect, corner_radius, &ctx->clips_2d[k]))
        return;

    ctx->nb_clips_2d++;
}

static void group2d_draw(struct ngl_node *node)
{
    struct group2d_priv *s = node->priv_data;
    const struct group2d_opts *o = node->opts;
    struct ngl_ctx *ctx = node->ctx;

    if (!o->node2d.visible) {
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    /* Push composed transform */
    int ret = ngli_node2d_push_transform(node);
    if (ret < 0)
        return;

    /* Save local transform */
    struct ngli_mat4 local_transform_matrix = *(const struct ngli_mat4 *)ngli_darray_tail(&node->ctx->transform_2d_stack);

    /* Push clip rectangle */
    const size_t saved_nb_clip = ctx->nb_clips_2d;
    group2d_push_clip(node);

    /* Draw children */
    for (size_t i = 0; i < o->nb_children; i++) {
        ngli_node_draw(o->children[i]);
    }

    /* Compute union bounding box from children */
    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    node2d_info->screen_aabb = ngli_node_compute_children_bounding_box(o->children, o->nb_children);
    node2d_info->transform_matrix = local_transform_matrix;

    /* Pop clip rectangle */
    ctx->nb_clips_2d = saved_nb_clip;

    /* Pop the 2D stacks */
    ngli_node2d_pop_transform(node);
}

static void group2d_uninit(struct ngl_node *node)
{
}

const struct node_class ngli_group2d_class = {
    .id        = NGL_NODE_GROUP2D,
    .name      = "Group2D",
    .priv_size = sizeof(struct group2d_priv),
    .init      = group2d_init,
    .prepare   = group2d_prepare,
    .update    = ngli_node_update_children,
    .pre_draw  = group2d_pre_draw,
    .draw      = group2d_draw,
    .uninit    = group2d_uninit,
    .opts_size = sizeof(struct group2d_opts),
    .params    = group2d_params,
    .flags     = NGLI_NODE_FLAG_2D,
    .node2d_offset = offsetof(struct group2d_opts, node2d),
    .file      = __FILE__,
};
