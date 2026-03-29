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
#include <string.h>

#include "internal.h"
#include "math_utils.h"
#include "node_uniform.h"
#include "nopegl/nopegl.h"

struct group2d_opts {
    struct ngl_node **children;
    size_t nb_children;
    struct ngl_node *translate_node;
    float translate[2];
    struct ngl_node *rotation_node;
    float rotation;
    struct ngl_node *scale_node;
    float scale[2];
    struct ngl_node *anchor_node;
    float anchor[2];
    struct ngl_node *opacity_node;
    float opacity;
};

struct group2d_priv {
    struct draw_info draw_info;
    struct darray indices;
};

static int group2d_swap_children(struct ngl_node *node, size_t from, size_t to)
{
    struct group2d_priv *s = node->priv_data;

    size_t *indices = ngli_darray_data(&s->indices);
    NGLI_SWAP(size_t, indices[from], indices[to]);

    return 0;
}

#define OFFSET(x) offsetof(struct group2d_opts, x)
static const struct node_param group2d_params[] = {
    {
        .key       = "children",
        .type      = NGLI_PARAM_TYPE_NODELIST,
        .offset    = OFFSET(children),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .swap_func = group2d_swap_children,
        .desc      = NGLI_DOCSTRING("2D scenes to draw"),
    }, {
        .key       = "translate",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(translate_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("translation in pixels"),
    }, {
        .key       = "rotation",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(rotation_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("rotation angle in degrees"),
    }, {
        .key       = "scale",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(scale_node),
        .def_value = {.vec={1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("scale factors"),
    }, {
        .key       = "anchor",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(anchor_node),
        .def_value = {.vec={NAN, NAN}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("anchor point in pixels (default: center of children)"),
    }, {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(opacity_node),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("opacity applied to all children"),
    },
    {NULL}
};

static int group2d_init(struct ngl_node *node)
{
    struct group2d_priv *s = node->priv_data;

    ngli_darray_init(&s->indices, sizeof(size_t), 0);

    return 0;
}

static int group2d_prepare(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct group2d_priv *s = node->priv_data;
    const struct group2d_opts *o = node->opts;

    int ret = 0;
    struct rnode *rnode_pos = ctx->rnode_pos;
    for (size_t i = 0; i < o->nb_children; i++) {
        struct rnode *rnode = ngli_rnode_add_child(rnode_pos);
        if (!rnode)
            return NGL_ERROR_MEMORY;

        if (!ngli_darray_push(&s->indices, &i))
            return NGL_ERROR_MEMORY;

        struct ngl_node *child = o->children[i];

        ctx->rnode_pos = rnode;
        ret = ngli_node_prepare(child);
        ctx->rnode_pos = rnode_pos;

        if (ret < 0)
            return ret;
    }

    return 0;
}

static void group2d_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct group2d_priv *s = node->priv_data;
    const struct group2d_opts *o = node->opts;

    /* Compute TRS matrix in pixel space */
    const float *anchor_val = ngli_node_get_data_ptr(o->anchor_node, o->anchor);
    const float anchor[3] = {
        isnan(anchor_val[0]) ? 0.f : anchor_val[0],
        isnan(anchor_val[1]) ? 0.f : anchor_val[1],
        0.f,
    };

    const float *scale = ngli_node_get_data_ptr(o->scale_node, o->scale);
    const float *rotation = ngli_node_get_data_ptr(o->rotation_node, &o->rotation);
    const float *translate = ngli_node_get_data_ptr(o->translate_node, o->translate);

    NGLI_ALIGNED_MAT(SM);
    NGLI_ALIGNED_MAT(RM);
    NGLI_ALIGNED_MAT(TM);
    ngli_mat4_scale(SM, scale[0], scale[1], 1.f, anchor);
    float z_axis[3] = {0.f, 0.f, 1.f};
    ngli_mat4_rotate(RM, NGLI_DEG2RAD(*rotation), z_axis, anchor);
    ngli_mat4_translate(TM, translate[0], translate[1], 0.f);
    NGLI_ALIGNED_MAT(trs_matrix);
    ngli_mat4_mul(trs_matrix, RM, SM);
    ngli_mat4_mul(trs_matrix, TM, trs_matrix);

    /* Push composed transform onto the 2D stack */
    float *next_matrix = ngli_darray_push(&ctx->transform_2d_stack, NULL);
    if (!next_matrix)
        return;
    const float *prev_matrix = next_matrix - 4 * 4;
    ngli_mat4_mul(next_matrix, prev_matrix, trs_matrix);

    /* Push composed opacity onto the 2D opacity stack */
    float *next_opacity = ngli_darray_push(&ctx->opacity_2d_stack, NULL);
    if (!next_opacity)
        return;
    const float *prev_opacity = next_opacity - 1;
    const float opacity = *(const float *)ngli_node_get_data_ptr(o->opacity_node, &o->opacity);
    *next_opacity = *prev_opacity * opacity;

    /* Draw children */
    struct rnode *rnode_pos = ctx->rnode_pos;
    struct rnode *rnodes = ngli_darray_data(&rnode_pos->children);
    const size_t *indices = ngli_darray_data(&s->indices);
    for (size_t i = 0; i < o->nb_children; i++) {
        const size_t index = indices[i];
        ctx->rnode_pos = &rnodes[index];
        struct ngl_node *child = o->children[i];
        ngli_node_draw(child);
    }
    ctx->rnode_pos = rnode_pos;

    /* Compute union bounding box from children */
    struct draw_info *draw_info = &s->draw_info;
    draw_info->screen_aabb = ngli_node_compute_children_bounding_box(o->children, o->nb_children);

    memcpy(draw_info->transform_matrix, next_matrix, sizeof(draw_info->transform_matrix));

    /* Pop the 2D stacks */
    ngli_darray_pop(&ctx->opacity_2d_stack);
    ngli_darray_pop(&ctx->transform_2d_stack);
}

static void group2d_uninit(struct ngl_node *node)
{
    struct group2d_priv *s = node->priv_data;

    ngli_darray_reset(&s->indices);
}

const struct node_class ngli_group2d_class = {
    .id        = NGL_NODE_GROUP2D,
    .name      = "Group2D",
    .priv_size = sizeof(struct group2d_priv),
    .init      = group2d_init,
    .prepare   = group2d_prepare,
    .update    = ngli_node_update_children,
    .draw      = group2d_draw,
    .uninit    = group2d_uninit,
    .opts_size = sizeof(struct group2d_opts),
    .params    = group2d_params,
    .flags     = NGLI_NODE_FLAG_BOUNDS,
    .file      = __FILE__,
};
