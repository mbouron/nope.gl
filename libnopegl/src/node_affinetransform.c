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

#include "internal.h"
#include "log.h"
#include "math_utils.h"
#include "node_transform.h"
#include "node_uniform.h"
#include "nopegl/nopegl.h"

struct affinetransform_opts {
    struct ngl_node *translate_node;
    float translate[3];
    struct ngl_node *rotate_angle_node;
    float rotate_angle;
    float rotate_axis[3];
    struct ngl_node *scale_factors_node;
    float scale_factors[3];
    struct ngl_node *anchor_node;
    float anchor[3];
};

struct affinetransform_priv {
    struct transform trf;
    float normed_axis[3];
    float *translate;
    float *angle;
    float *scale_factors;
    float *anchor;
};

static void update_trf_matrix(struct ngl_node *node)
{
    struct affinetransform_priv *s = node->priv_data;
    struct transform *trf = &s->trf;

    const float *translate = s->translate;
    const float angle = NGLI_DEG2RAD(*s->angle);
    const float *factors = s->scale_factors;

    NGLI_ALIGNED_MAT(s_mat);
    NGLI_ALIGNED_MAT(r_mat);
    NGLI_ALIGNED_MAT(t_mat);
    NGLI_ALIGNED_MAT(tmp);

    ngli_mat4_scale(s_mat, factors[0], factors[1], factors[2], s->anchor);
    ngli_mat4_rotate(r_mat, angle, s->normed_axis, s->anchor);
    ngli_mat4_translate(t_mat, translate[0], translate[1], translate[2]);

    ngli_mat4_mul(tmp, r_mat, s_mat);
    ngli_mat4_mul(trf->matrix, t_mat, tmp);
}

static int affinetransform_init(struct ngl_node *node)
{
    struct affinetransform_priv *s = node->priv_data;
    const struct affinetransform_opts *o = node->opts;

    if (ngli_vec3_is_zero(o->rotate_axis)) {
        LOG(ERROR, "(0.0, 0.0, 0.0) is not a valid rotation axis");
        return NGL_ERROR_INVALID_ARG;
    }
    ngli_vec3_norm(s->normed_axis, o->rotate_axis);

    s->translate     = ngli_node_get_data_ptr(o->translate_node, (void *)o->translate);
    s->angle         = ngli_node_get_data_ptr(o->rotate_angle_node, (void *)&o->rotate_angle);
    s->scale_factors = ngli_node_get_data_ptr(o->scale_factors_node, (void *)o->scale_factors);
    s->anchor        = ngli_node_get_data_ptr(o->anchor_node, (void *)o->anchor);

    if (!o->anchor_node && ngli_vec3_is_zero(o->anchor))
        s->anchor = NULL;

    update_trf_matrix(node);

    s->trf.child = NULL;
    return 0;
}

static int update_translate(struct ngl_node *node)
{
    update_trf_matrix(node);
    return 0;
}

static int update_rotate_angle(struct ngl_node *node)
{
    update_trf_matrix(node);
    return 0;
}

static int update_scale_factors(struct ngl_node *node)
{
    update_trf_matrix(node);
    return 0;
}

static int update_anchor(struct ngl_node *node)
{
    update_trf_matrix(node);
    return 0;
}

static int affinetransform_update(struct ngl_node *node, double t)
{
    const struct affinetransform_opts *o = node->opts;
    int update = 0;

    if (o->translate_node) {
        int ret = ngli_node_update(o->translate_node, t);
        if (ret < 0)
            return ret;
        update = 1;
    }
    if (o->rotate_angle_node) {
        int ret = ngli_node_update(o->rotate_angle_node, t);
        if (ret < 0)
            return ret;
        update = 1;
    }
    if (o->scale_factors_node) {
        int ret = ngli_node_update(o->scale_factors_node, t);
        if (ret < 0)
            return ret;
        update = 1;
    }
    if (o->anchor_node) {
        int ret = ngli_node_update(o->anchor_node, t);
        if (ret < 0)
            return ret;
        update = 1;
    }

    if (update)
        update_trf_matrix(node);

    return 0;
}

#define OFFSET(x) offsetof(struct affinetransform_opts, x)
static const struct node_param affinetransform_params[] = {
    {
        .key = "translate",
        .type = NGLI_PARAM_TYPE_VEC3,
        .offset = OFFSET(translate_node),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc = NGLI_DOCSTRING("translation vector"),
        .update_func = update_translate,
    },
    {"rotate_angle",  NGLI_PARAM_TYPE_F32, OFFSET(rotate_angle_node),
                      .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                      .update_func=update_rotate_angle,
                      .desc=NGLI_DOCSTRING("rotation angle in degrees")},
    {"rotate_axis",   NGLI_PARAM_TYPE_VEC3, OFFSET(rotate_axis), {.vec={0.0f, 0.0f, 1.0f}},
                      .desc=NGLI_DOCSTRING("rotation axis")},
    {"scale_factors", NGLI_PARAM_TYPE_VEC3, OFFSET(scale_factors), {.vec={1.0f, 1.0f, 1.0f}},
                      .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                      .update_func=update_scale_factors,
                      .desc=NGLI_DOCSTRING("scaling factors")},
    {"anchor",        NGLI_PARAM_TYPE_VEC3, OFFSET(anchor_node),
                      .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                      .update_func=update_anchor,
                      .desc=NGLI_DOCSTRING("anchor point for rotation and scaling")},
    {NULL}
};

NGLI_STATIC_ASSERT(offsetof(struct affinetransform_priv, trf) == 0, "trf on top of affinetransform");

const struct node_class ngli_affinetransform_class = {
    .id        = NGL_NODE_AFFINETRANSFORM,
    .category  = NGLI_NODE_CATEGORY_TRANSFORM,
    .name      = "AffineTransform",
    .init      = affinetransform_init,
    .update    = affinetransform_update,
    .opts_size = sizeof(struct affinetransform_opts),
    .priv_size = sizeof(struct affinetransform_priv),
    .params    = affinetransform_params,
    .file      = __FILE__,
};
