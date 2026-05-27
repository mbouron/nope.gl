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
#include <string.h>

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

    const struct ngli_mat4 *prev_matrix_ptr = ngli_darray_tail(&ctx->transform_2d_stack);
    const struct ngli_mat4 prev_matrix = *prev_matrix_ptr;

    struct ngli_mat4 next_matrix;
    ngli_mat4_mul(next_matrix.m, prev_matrix.m, trs.m);

    if (ngli_darray_push(&ctx->transform_2d_stack, next_matrix) < 0)
        return NGL_ERROR_MEMORY;

    const float opacity = *(const float *)ngli_node_get_data_ptr(opts->opacity_node, &opts->opacity);

    const float *prev_opacity_ptr = ngli_darray_tail(&ctx->opacity_2d_stack);
    const float prev_opacity = *prev_opacity_ptr;

    const float next_opacity = prev_opacity * opacity;

    if (ngli_darray_push(&ctx->opacity_2d_stack, next_opacity) < 0)
        return NGL_ERROR_MEMORY;

    return 0;
}

void ngli_node2d_pop_transform(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    ngli_darray_pop(&ctx->opacity_2d_stack);
    ngli_darray_pop(&ctx->transform_2d_stack);
}

bool ngli_node2d_compute_clip(const struct ngli_mat4 *modelview,
                              const float clip_rect[4], const float corner_radius[2],
                              struct ngli_clip2d *out)
{
    if (clip_rect[2] <= 0.f || clip_rect[3] <= 0.f)
        return false;

    const float half_w = clip_rect[2] * 0.5f;
    const float half_h = clip_rect[3] * 0.5f;

    /* Clip center (local -> canvas pixels). */
    const float center_local[4] = {clip_rect[0] + half_w, clip_rect[1] + half_h, 0.f, 1.f};
    float center[4];
    ngli_mat4_mul_vec4(center, modelview->m, center_local);

    /*
     * Invert the modelview's 2D linear part [[a, b], [c, d]] (canvas = L * local)
     * so the fragment shader can map a canvas delta back into local space.
     */
    const float a = modelview->m[0], b = modelview->m[4], c = modelview->m[1], d = modelview->m[5];
    const float det = a * d - b * c;
    if (fabsf(det) < 1e-9f)
        return false;
    const float idet = 1.f / det;

    /* Corner radii, clamped to half the rectangle so they stay well-formed. */
    const float rx = NGLI_MIN(NGLI_MAX(corner_radius[0], 0.f), half_w);
    const float ry = NGLI_MIN(NGLI_MAX(corner_radius[1], 0.f), half_h);

    out->inv    = (struct ngli_vec4){.v = {d * idet, -b * idet, -c * idet, a * idet}};
    out->rect   = (struct ngli_vec4){.v = {center[0], center[1], half_w, half_h}};
    out->radius = (struct ngli_vec4){.v = {rx, ry, 0.f, 0.f}};

    return true;
}
