/*
 * Copyright 2025 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2016-2022 GoPro Inc.
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
#include "math_utils.h"
#include "node_transform.h"
#include "node_uniform.h"
#include "nopegl/nopegl.h"
#include "transforms.h"

struct rotate_opts {
    struct ngl_node *child;
    struct ngl_node *angle_node;
    float angle;
    float axis[3];
    struct ngl_node *anchor_node;
    float anchor[3];
};

struct rotate_priv {
    struct transform trf;
    float normed_axis[3];
    int animated;
    float *angle_ptr;
    float *anchor_ptr;
};

static void update_trf_matrix(struct ngl_node *node)
{
    struct rotate_priv *s = node->priv_data;
    struct transform *trf = &s->trf;

    const float angle = NGLI_DEG2RAD(*s->angle_ptr);
    ngli_mat4_rotate(trf->matrix, angle, s->normed_axis, s->anchor_ptr);
}

static int rotate_init(struct ngl_node *node)
{
    struct rotate_priv *s = node->priv_data;
    const struct rotate_opts *o = node->opts;

    if (ngli_vec3_is_zero(o->axis)) {
        LOG(ERROR, "(0.0, 0.0, 0.0) is not a valid axis");
        return NGL_ERROR_INVALID_ARG;
    }
    ngli_vec3_norm(s->normed_axis, o->axis);

    s->angle_ptr = ngli_node_get_data_ptr(o->angle_node, (void *)&o->angle);
    s->anchor_ptr = ngli_node_get_data_ptr(o->anchor_node, (void *)o->anchor);

    update_trf_matrix(node);

    s->trf.child = o->child;
    return 0;
}

static int update_angle(struct ngl_node *node)
{
    update_trf_matrix(node);
    return 0;
}

static int update_anchor(struct ngl_node *node)
{
    update_trf_matrix(node);
    return 0;
}

static int rotate_update(struct ngl_node *node, double t)
{
    const struct rotate_opts *o = node->opts;
    int update_trf = 0;
    if (o->angle_node) {
        update_trf = 1;
        ngli_node_update(o->angle_node, t);
    }
    if (o->anchor_node) {
        update_trf = 1;
        ngli_node_update(o->anchor_node, t);
    }
    if (update_trf) {
        update_trf_matrix(node);
    }
    return ngli_node_update(o->child, t);
}

#define OFFSET(x) offsetof(struct rotate_opts, x)
static const struct node_param rotate_params[] = {
    {"child",  NGLI_PARAM_TYPE_NODE, OFFSET(child),
               .flags=NGLI_PARAM_FLAG_NON_NULL,
               .desc=NGLI_DOCSTRING("scene to rotate")},
    {"angle",  NGLI_PARAM_TYPE_F32,  OFFSET(angle_node),
               .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
               .update_func=update_angle,
               .desc=NGLI_DOCSTRING("rotation angle in degrees")},
    {"axis",   NGLI_PARAM_TYPE_VEC3, OFFSET(axis),   {.vec={0.0f, 0.0f, 1.0f}},
               .desc=NGLI_DOCSTRING("rotation axis")},
    {"anchor", NGLI_PARAM_TYPE_VEC3, OFFSET(anchor_node), {.vec={0.0f, 0.0f, 0.0f}},
               .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
               .desc=NGLI_DOCSTRING("vector to the center point of the rotation"),
               .update_func=update_anchor},
    {NULL}
};

NGLI_STATIC_ASSERT(offsetof(struct rotate_priv, trf) == 0, "trf on top of rotate");

const struct node_class ngli_rotate_class = {
    .id        = NGL_NODE_ROTATE,
    .category  = NGLI_NODE_CATEGORY_TRANSFORM,
    .name      = "Rotate",
    .init      = rotate_init,
    .update    = rotate_update,
    .draw      = ngli_transform_draw,
    .opts_size = sizeof(struct rotate_opts),
    .priv_size = sizeof(struct rotate_priv),
    .params    = rotate_params,
    .file      = __FILE__,
};
