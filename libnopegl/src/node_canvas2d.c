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

struct canvas2d_opts {
    struct ngl_node **children;
    size_t nb_children;
    int32_t width;
    int32_t height;
};

struct canvas2d_priv {
    struct ngli_node2d_info node2d_info;
    struct darray indices;
};

static int canvas2d_swap_children(struct ngl_node *node, size_t from, size_t to)
{
    struct canvas2d_priv *s = node->priv_data;

    size_t *indices = ngli_darray_data(&s->indices);
    NGLI_SWAP(size_t, indices[from], indices[to]);

    return 0;
}

#define OFFSET(x) offsetof(struct canvas2d_opts, x)
static const struct node_param canvas2d_params[] = {
    {
        .key       = "children",
        .type      = NGLI_PARAM_TYPE_NODELIST,
        .offset    = OFFSET(children),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .node_types = NGLI_NODE2D_TYPES_LIST,
        .desc      = NGLI_DOCSTRING("2D scenes to draw"),
        .swap_func = canvas2d_swap_children,
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

static int canvas2d_init(struct ngl_node *node)
{
    struct canvas2d_priv *s = node->priv_data;

    ngli_darray_init(&s->indices, sizeof(size_t), 0);

    return 0;
}

static int canvas2d_prepare(struct ngl_node *node,
                            const struct ngpu_graphics_state *graphics_state,
                            const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct canvas2d_priv *s = node->priv_data;
    const struct canvas2d_opts *o = node->opts;

    for (size_t i = 0; i < o->nb_children; i++) {
        if (!ngli_darray_push(&s->indices, &i))
            return NGL_ERROR_MEMORY;
    }

    return 0;
}

static void canvas2d_pre_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct canvas2d_priv *s = node->priv_data;
    const struct canvas2d_opts *o = node->opts;

    /* Save previous 2D state */
    struct darray prev_transform_2d_stack = ctx->transform_2d_stack;
    struct darray prev_opacity_2d_stack = ctx->opacity_2d_stack;
    const float prev_canvas_2d_width = ctx->canvas_2d_width;
    const float prev_canvas_2d_height = ctx->canvas_2d_height;

    /* Initialize fresh stacks for bbox computation */
    ngli_darray_init(&ctx->transform_2d_stack, sizeof(struct ngli_mat4), NGLI_DARRAY_FLAG_ALIGNED);
    ngli_darray_init(&ctx->opacity_2d_stack, sizeof(float), 0);

    const float w = o->width  > 0 ? (float)o->width  : ctx->viewport.width;
    const float h = o->height > 0 ? (float)o->height : ctx->viewport.height;
    ctx->canvas_2d_width = w;
    ctx->canvas_2d_height = h;

    static const struct ngli_mat4 id_matrix = {.m = NGLI_MAT4_IDENTITY};
    const float default_opacity = 1.f;
    if (!ngli_darray_push(&ctx->transform_2d_stack, &id_matrix) ||
        !ngli_darray_push(&ctx->opacity_2d_stack, &default_opacity))
        goto restore;

    /* Pre-draw children (computes bboxes) */
    for (size_t i = 0; i < o->nb_children; i++)
        ngli_node_pre_draw(o->children[i]);

    /* Compute canvas bbox from children */
    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    node2d_info->screen_aabb = ngli_node_compute_children_bounding_box(o->children, o->nb_children);

restore:
    ngli_darray_reset(&ctx->transform_2d_stack);
    ngli_darray_reset(&ctx->opacity_2d_stack);
    ctx->transform_2d_stack = prev_transform_2d_stack;
    ctx->opacity_2d_stack = prev_opacity_2d_stack;
    ctx->canvas_2d_width = prev_canvas_2d_width;
    ctx->canvas_2d_height = prev_canvas_2d_height;
}

static void canvas2d_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct canvas2d_priv *s = node->priv_data;
    const struct canvas2d_opts *o = node->opts;

    /* Save previous 2D state so nested Canvas2D (e.g. via Texture2D RTT) works */
    struct ngli_mat4 prev_projection_2d = ctx->projection_2d_matrix;
    struct darray prev_transform_2d_stack = ctx->transform_2d_stack;
    struct darray prev_opacity_2d_stack = ctx->opacity_2d_stack;

    /* Initialize fresh stacks for this Canvas2D */
    ngli_darray_init(&ctx->transform_2d_stack, sizeof(struct ngli_mat4), NGLI_DARRAY_FLAG_ALIGNED);
    ngli_darray_init(&ctx->opacity_2d_stack, sizeof(float), 0);

    /* Compute canvas dimensions */
    const float prev_canvas_2d_width = ctx->canvas_2d_width;
    const float prev_canvas_2d_height = ctx->canvas_2d_height;
    const float w = o->width  > 0 ? (float)o->width  : ctx->viewport.width;
    const float h = o->height > 0 ? (float)o->height : ctx->viewport.height;
    ctx->canvas_2d_width = w;
    ctx->canvas_2d_height = h;

    /* Build the 2D orthographic projection and store it in ctx */
    struct ngli_mat4 base_projection_matrix;
    ngpu_ctx_get_projection_matrix(gpu_ctx, base_projection_matrix.m);
    ngli_mat4_orthographic(ctx->projection_2d_matrix.m, -0.5f, w - 0.5f, h - 0.5f, -0.5f, -1.f, 1.f);
    ngli_mat4_mul(ctx->projection_2d_matrix.m, base_projection_matrix.m, ctx->projection_2d_matrix.m);

    /* Push identity transform and default opacity */
    static const struct ngli_mat4 id_matrix = {.m = NGLI_MAT4_IDENTITY};
    const float default_opacity = 1.f;
    if (!ngli_darray_push(&ctx->transform_2d_stack, &id_matrix) ||
        !ngli_darray_push(&ctx->opacity_2d_stack, &default_opacity))
        goto restore;

    /* Draw children */
    const size_t *indices = ngli_darray_data(&s->indices);
    for (size_t i = 0; i < o->nb_children; i++) {
        const size_t index = indices[i];
        ngli_node_draw(o->children[index]);
    }

    /* Compute union bounding box from children */
    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    node2d_info->screen_aabb = ngli_node_compute_children_bounding_box(o->children, o->nb_children);

    node2d_info->aabb = node2d_info->screen_aabb;
    node2d_info->transform_matrix = id_matrix;

restore:
    /* Restore previous 2D state */
    ngli_darray_reset(&ctx->transform_2d_stack);
    ngli_darray_reset(&ctx->opacity_2d_stack);
    ctx->transform_2d_stack = prev_transform_2d_stack;
    ctx->opacity_2d_stack = prev_opacity_2d_stack;
    ctx->projection_2d_matrix = prev_projection_2d;
    ctx->canvas_2d_width = prev_canvas_2d_width;
    ctx->canvas_2d_height = prev_canvas_2d_height;
}

static void canvas2d_uninit(struct ngl_node *node)
{
    struct canvas2d_priv *s = node->priv_data;

    ngli_darray_reset(&s->indices);
}

const struct node_class ngli_canvas2d_class = {
    .id        = NGL_NODE_CANVAS2D,
    .name      = "Canvas2D",
    .priv_size = sizeof(struct canvas2d_priv),
    .init      = canvas2d_init,
    .prepare   = canvas2d_prepare,
    .update    = ngli_node_update_children,
    .pre_draw  = canvas2d_pre_draw,
    .draw      = canvas2d_draw,
    .uninit    = canvas2d_uninit,
    .opts_size = sizeof(struct canvas2d_opts),
    .params    = canvas2d_params,
    .flags     = NGLI_NODE_FLAG_2D,
    .file      = __FILE__,
};
