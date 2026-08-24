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
#include "node2d.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "nopegl/nopegl.h"
#include "utils/darray.h"
#include "utils/utils.h"

struct canvas2d_opts {
    struct ngl_node **children;
    size_t nb_children;
    int32_t width;
    int32_t height;
};

struct canvas2d_priv {
    struct ngli_node2d_info node2d_info;
};

#define OFFSET(x) offsetof(struct canvas2d_opts, x)
static const struct node_param canvas2d_params[] = {
    {
        .key       = "children",
        .type      = NGLI_PARAM_TYPE_NODELIST,
        .offset    = OFFSET(children),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .node_types = NGLI_NODE2D_TYPES_LIST,
        .desc      = NGLI_DOCSTRING("2D scenes to draw"),
    }, {
        .key    = "width",
        .type   = NGLI_PARAM_TYPE_I32,
        .offset = OFFSET(width),
        .desc   = NGLI_DOCSTRING("canvas width in pixels (0 uses viewport width)"),
    }, {
        .key    = "height",
        .type   = NGLI_PARAM_TYPE_I32,
        .offset = OFFSET(height),
        .desc   = NGLI_DOCSTRING("canvas height in pixels (0 uses viewport height)"),
    },
    {NULL}
};

static void canvas2d_get_dimensions(const struct ngl_node *node, float *width, float *height)
{
    const struct ngl_ctx *ctx = node->ctx;
    const struct canvas2d_opts *o = node->opts;

    *width  = o->width  > 0 ? (float)o->width  : ctx->viewport.width;
    *height = o->height > 0 ? (float)o->height : ctx->viewport.height;
}

static void canvas2d_pre_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct canvas2d_priv *s = node->priv_data;
    const struct canvas2d_opts *o = node->opts;

    const float prev_canvas_2d_width = ctx->canvas_2d_width;
    const float prev_canvas_2d_height = ctx->canvas_2d_height;

    float w, h;
    canvas2d_get_dimensions(node, &w, &h);
    ctx->canvas_2d_width = w;
    ctx->canvas_2d_height = h;

    const struct ngli_mat4 prev_transform_2d = ctx->transform_2d_matrix;
    const float prev_opacity_2d = ctx->opacity_2d;
    ngli_node2d_apply_default_transform(ctx);

    /* Pre-draw children (computes bboxes) */
    for (size_t i = 0; i < o->nb_children; i++)
        ngli_node_pre_draw(o->children[i]);

    /* Compute canvas bbox from children */
    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    node2d_info->screen_aabb = ngli_node_compute_children_bounding_box(o->children, o->nb_children);
    node2d_info->effect_margin = ngli_node_compute_children_effect_margin(o->children, o->nb_children);

    ctx->transform_2d_matrix = prev_transform_2d;
    ctx->opacity_2d = prev_opacity_2d;
    ctx->canvas_2d_width = prev_canvas_2d_width;
    ctx->canvas_2d_height = prev_canvas_2d_height;
}

static void canvas2d_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct canvas2d_priv *s = node->priv_data;
    const struct canvas2d_opts *o = node->opts;

    /* Save 2D state so nested Canvas2D (e.g. via Texture2D RTT) works */
    const struct ngli_mat4 prev_projection_2d = ctx->projection_2d_matrix;

    /* Compute canvas dimensions */
    const float prev_canvas_2d_width = ctx->canvas_2d_width;
    const float prev_canvas_2d_height = ctx->canvas_2d_height;
    float w, h;
    canvas2d_get_dimensions(node, &w, &h);
    ctx->canvas_2d_width = w;
    ctx->canvas_2d_height = h;

    /* Build the 2D orthographic projection and store it in ctx */
    struct ngli_mat4 base_projection_matrix;
    ngpu_ctx_get_projection_matrix(gpu_ctx, base_projection_matrix.m);
    ngli_mat4_orthographic(ctx->projection_2d_matrix.m, -0.5f, w - 0.5f, h - 0.5f, -0.5f, -1.f, 1.f);
    ngli_mat4_mul(ctx->projection_2d_matrix.m, base_projection_matrix.m, ctx->projection_2d_matrix.m);

    const struct ngli_mat4 prev_transform_2d = ctx->transform_2d_matrix;
    const float prev_opacity_2d = ctx->opacity_2d;
    ngli_node2d_apply_default_transform(ctx);

    /* Draw children */
    for (size_t i = 0; i < o->nb_children; i++) {
        ngli_node_draw(o->children[i]);
    }

    /* Compute union bounding box from children */
    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    node2d_info->screen_aabb = ngli_node_compute_children_bounding_box(o->children, o->nb_children);
    node2d_info->effect_margin = ngli_node_compute_children_effect_margin(o->children, o->nb_children);

    static const struct ngli_mat4 id_matrix = {.m = NGLI_MAT4_IDENTITY};
    node2d_info->aabb = node2d_info->screen_aabb;
    node2d_info->transform_matrix = id_matrix;

    /* Restore previous 2D state */
    ctx->transform_2d_matrix = prev_transform_2d;
    ctx->opacity_2d = prev_opacity_2d;
    ctx->projection_2d_matrix = prev_projection_2d;
    ctx->canvas_2d_width = prev_canvas_2d_width;
    ctx->canvas_2d_height = prev_canvas_2d_height;
}

static void canvas2d_uninit(struct ngl_node *node)
{
}

const struct node_class ngli_canvas2d_class = {
    .id        = NGL_NODE_CANVAS2D,
    .name      = "Canvas2D",
    .priv_size = sizeof(struct canvas2d_priv),
    .update    = ngli_node_update_children,
    .pre_draw  = canvas2d_pre_draw,
    .draw      = canvas2d_draw,
    .uninit    = canvas2d_uninit,
    .opts_size = sizeof(struct canvas2d_opts),
    .params    = canvas2d_params,
    .flags     = NGLI_NODE_FLAG_2D,
    .file      = __FILE__,
};
