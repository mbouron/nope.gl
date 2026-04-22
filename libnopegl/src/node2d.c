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

#include "internal.h"
#include "math_utils.h"
#include "node2d.h"
#include "node_uniform.h"

static const struct ngli_node2d_opts *get_node2d_opts(const struct ngl_node *node)
{
    return (const struct ngli_node2d_opts *)((const uint8_t *)node->opts + node->cls->node2d_offset);
}

void ngli_node2d_compute_trs(const struct ngl_node *node, float *trs_matrix)
{
    const struct ngli_node2d_opts *opts = get_node2d_opts(node);
    const struct ngli_node2d_info *info = node->priv_data;

    const float *anchor_val = ngli_node_get_data_ptr(opts->anchor_node, opts->anchor);
    const float anchor[3] = {
        isnan(anchor_val[0]) ? info->aabb.center[0] : anchor_val[0],
        isnan(anchor_val[1]) ? info->aabb.center[1] : anchor_val[1],
        0.f,
    };

    const float *scale = ngli_node_get_data_ptr(opts->scale_node, opts->scale);
    const float *rotation = ngli_node_get_data_ptr(opts->rotation_node, &opts->rotation);
    const float *translate = ngli_node_get_data_ptr(opts->translate_node, opts->translate);

    struct ngli_mat4 SM;
    struct ngli_mat4 RM;
    struct ngli_mat4 TM;
    ngli_mat4_scale(SM.m, scale[0], scale[1], 1.f, anchor);
    float z_axis[3] = {0.f, 0.f, 1.f};
    ngli_mat4_rotate(RM.m, NGLI_DEG2RAD(*rotation), z_axis, anchor);
    ngli_mat4_translate(TM.m, translate[0], translate[1], 0.f);
    ngli_mat4_mul(trs_matrix, RM.m, SM.m);
    ngli_mat4_mul(trs_matrix, TM.m, trs_matrix);
}

int ngli_node2d_push_transform(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    const struct ngli_node2d_opts *opts = get_node2d_opts(node);

    struct ngli_mat4 trs;
    ngli_node2d_compute_trs(node, trs.m);

    struct ngli_mat4 *next_matrix = ngli_darray_push(&ctx->transform_2d_stack, NULL);
    if (!next_matrix)
        return NGL_ERROR_MEMORY;
    const struct ngli_mat4 *prev_matrix = next_matrix - 1;
    ngli_mat4_mul(next_matrix->m, prev_matrix->m, trs.m);

    float *next_opacity = ngli_darray_push(&ctx->opacity_2d_stack, NULL);
    if (!next_opacity)
        return NGL_ERROR_MEMORY;
    const float *prev_opacity = next_opacity - 1;
    const float opacity = *(const float *)ngli_node_get_data_ptr(opts->opacity_node, &opts->opacity);
    *next_opacity = *prev_opacity * opacity;

    return 0;
}

void ngli_node2d_pop_transform(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    ngli_darray_pop(&ctx->opacity_2d_stack);
    ngli_darray_pop(&ctx->transform_2d_stack);
}
