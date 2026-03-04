/*
 * Copyright Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "math_utils.h"
#include "nopegl/nopegl.h"
#include "internal.h"
#include "rnode.h"
#include "utils/memory.h"

struct group_opts {
    struct ngl_node **children;
    size_t nb_children;
    float translate[3];
    float scale[3];
    float rotate_angle;
    float anchor[3];
};

struct group_priv {
    struct darray indices;
    NGLI_ALIGNED_MAT(matrix);
    struct rnode *rnode_pos; /* group's own rnode, set during prepare */
};

static void rebuild_matrix(struct ngl_node *node, const float *anchor)
{
    struct group_priv *s = node->priv_data;
    const struct group_opts *o = node->opts;

    NGLI_ALIGNED_MAT(SM);
    NGLI_ALIGNED_MAT(RM);
    NGLI_ALIGNED_MAT(TM);

    ngli_mat4_scale(SM, o->scale[0], o->scale[1], o->scale[2], anchor);

    float z_axis[3] = {0.f, 0.f, 1.f};
    ngli_mat4_rotate(RM, NGLI_DEG2RAD(o->rotate_angle), z_axis, anchor);

    ngli_mat4_translate(TM, o->translate[0], o->translate[1], o->translate[2]);

    /* M = T * R * S  (scale first, then rotate, then translate) */
    ngli_mat4_mul(s->matrix, RM, SM);
    ngli_mat4_mul(s->matrix, TM, s->matrix);
}

static int group_swap_children(struct ngl_node *node, size_t from, size_t to)
{
    struct group_priv *s = node->priv_data;

    size_t *indices = ngli_darray_data(&s->indices);
    NGLI_SWAP(size_t, indices[from], indices[to]);

    return 0;
}

#define OFFSET(x) offsetof(struct group_opts, x)
static const struct node_param group_params[] = {
    {"children",     NGLI_PARAM_TYPE_NODELIST, OFFSET(children),
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .swap_func=group_swap_children,
                     .desc=NGLI_DOCSTRING("a set of scenes")},
    {"translate",    NGLI_PARAM_TYPE_VEC3, OFFSET(translate),
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .desc=NGLI_DOCSTRING("translation vector")},
    {"scale",        NGLI_PARAM_TYPE_VEC3, OFFSET(scale), {.vec={1.f, 1.f, 1.f}},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .desc=NGLI_DOCSTRING("scale factors")},
    {"rotate_angle", NGLI_PARAM_TYPE_F32, OFFSET(rotate_angle),
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .desc=NGLI_DOCSTRING("rotation angle in degrees (around the Z axis)")},
    {"anchor",       NGLI_PARAM_TYPE_VEC3, OFFSET(anchor), {.vec={NAN, NAN, NAN}},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .desc=NGLI_DOCSTRING("pivot point for rotation and scale; defaults to the center of the screen")},
    {NULL}
};

static int group_init(struct ngl_node *node)
{
    struct group_priv *s = node->priv_data;
    ngli_darray_init(&s->indices, sizeof(size_t), 0);
    return 0;
}

static int group_update(struct ngl_node *node, double t)
{
    return ngli_node_update_children(node, t);
}

static int group_prepare(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct group_priv *s = node->priv_data;
    const struct group_opts *o = node->opts;

    s->rnode_pos = ctx->rnode_pos;

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

static void group_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct group_priv *s = node->priv_data;
    const struct group_opts *o = node->opts;

    const struct ngl_scene_params *scene_params = ngl_scene_get_params(node->scene);
    const float canvas_w = scene_params->width  > 0 ? (float)scene_params->width  : (float)ctx->viewport.width;
    const float canvas_h = scene_params->height > 0 ? (float)scene_params->height : (float)ctx->viewport.height;
    const float anchor[3] = {
        isnan(o->anchor[0]) ? canvas_w * 0.5f : o->anchor[0],
        isnan(o->anchor[1]) ? canvas_h * 0.5f : o->anchor[1],
        isnan(o->anchor[2]) ? 0.f             : o->anchor[2],
    };
    rebuild_matrix(node, anchor);

    float *next_matrix = ngli_darray_push(&ctx->modelview_matrix_stack, NULL);
    if (!next_matrix)
        return;

    /* We cannot use ngli_darray_tail() before calling ngli_darray_push() as
     * ngli_darray_push() can potentially perform a re-allocation on the
     * underlying matrix stack buffer */
    const float *prev_matrix = next_matrix - 4 * 4;
    ngli_mat4_mul(next_matrix, prev_matrix, s->matrix);

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

    ngli_darray_pop(&ctx->modelview_matrix_stack);
}

static void group_uninit(struct ngl_node *node)
{
    struct group_priv *s = node->priv_data;

    ngli_darray_reset(&s->indices);
}

int ngl_node_group_add_child(struct ngl_node *node, struct ngl_node *child)
{
    if (node->cls->id != NGL_NODE_GROUP) {
        LOG(ERROR, "node is not a Group");
        return NGL_ERROR_INVALID_ARG;
    }

    struct group_opts *o = node->opts;
    struct group_priv *s = node->priv_data;

    /* Extend the opts children array */
    struct ngl_node **new_children = ngli_realloc(o->children,
                                                  o->nb_children + 1,
                                                  sizeof(*o->children));
    if (!new_children)
        return NGL_ERROR_MEMORY;
    o->children = new_children;
    o->children[o->nb_children] = ngl_node_ref(child);

    /* If the group is not live yet, we're done */
    if (!node->ctx) {
        o->nb_children++;
        return 0;
    }

    /* Update scene membership and parent/child tracking */
    int ret = ngli_scene_attach_child(node->scene, node, child);
    if (ret < 0) {
        ngl_node_unrefp(&o->children[o->nb_children]);
        return ret;
    }

    /* Init the child subtree in the rendering context */
    ret = ngli_node_set_ctx(child, node->ctx);
    if (ret < 0) {
        ngli_scene_detach_child(node->scene, node, child);
        ngl_node_unrefp(&o->children[o->nb_children]);
        return ret;
    }

    /* Add a new rnode for this child and run its prepare phase */
    struct rnode *rnode = ngli_rnode_add_child(s->rnode_pos);
    if (!rnode) {
        ngli_node_detach_ctx(child, node->ctx);
        ngli_scene_detach_child(node->scene, node, child);
        ngl_node_unrefp(&o->children[o->nb_children]);
        return NGL_ERROR_MEMORY;
    }
    const size_t new_idx = ngli_darray_count(&s->rnode_pos->children) - 1;

    struct ngl_ctx *ctx = node->ctx;
    struct rnode *saved = ctx->rnode_pos;
    ctx->rnode_pos = rnode;
    ret = ngli_node_prepare(child);
    ctx->rnode_pos = saved;
    if (ret < 0) {
        /* rnode is already appended; clear it by removing from children darray */
        ngli_darray_remove(&s->rnode_pos->children, new_idx);
        ngli_node_detach_ctx(child, node->ctx);
        ngli_scene_detach_child(node->scene, node, child);
        ngl_node_unrefp(&o->children[o->nb_children]);
        return ret;
    }

    if (!ngli_darray_push(&s->indices, &new_idx)) {
        ngli_darray_remove(&s->rnode_pos->children, new_idx);
        ngli_node_detach_ctx(child, node->ctx);
        ngli_scene_detach_child(node->scene, node, child);
        ngl_node_unrefp(&o->children[o->nb_children]);
        return NGL_ERROR_MEMORY;
    }

    o->nb_children++;
    return 0;
}

int ngl_node_group_remove_child(struct ngl_node *node, struct ngl_node *child)
{
    if (node->cls->id != NGL_NODE_GROUP) {
        LOG(ERROR, "node is not a Group");
        return NGL_ERROR_INVALID_ARG;
    }

    struct group_opts *o = node->opts;
    struct group_priv *s = node->priv_data;

    /* Find the child's position in o->children */
    size_t pos = o->nb_children;
    for (size_t i = 0; i < o->nb_children; i++) {
        if (o->children[i] == child) {
            pos = i;
            break;
        }
    }
    if (pos == o->nb_children) {
        LOG(ERROR, "node is not a child of this group");
        return NGL_ERROR_INVALID_ARG;
    }

    if (node->ctx) {
        /* Get the rnode index for this child before removing anything */
        const size_t *indices = ngli_darray_data(&s->indices);
        const size_t rnode_idx = indices[pos];

        /* Uninit the child subtree */
        ngli_node_detach_ctx(child, node->ctx);

        /* Update scene membership tracking */
        ngli_scene_detach_child(node->scene, node, child);

        /* Remove the rnode; fix up all indices pointing past it */
        ngli_darray_remove(&s->rnode_pos->children, rnode_idx);
        size_t *idx_data = ngli_darray_data(&s->indices);
        const size_t nb = ngli_darray_count(&s->indices);
        for (size_t i = 0; i < nb; i++) {
            if (idx_data[i] > rnode_idx)
                idx_data[i]--;
        }
        ngli_darray_remove(&s->indices, pos);
    }

    /* Remove from opts children array */
    ngl_node_unrefp(&o->children[pos]);
    const size_t tail = o->nb_children - pos - 1;
    if (tail)
        memmove(&o->children[pos], &o->children[pos + 1],
                tail * sizeof(*o->children));
    o->nb_children--;

    return 0;
}

const struct node_class ngli_group_class = {
    .id        = NGL_NODE_GROUP,
    .name      = "Group",
    .priv_size = sizeof(struct group_priv),
    .init      = group_init,
    .prepare   = group_prepare,
    .update    = group_update,
    .draw      = group_draw,
    .uninit    = group_uninit,
    .opts_size = sizeof(struct group_opts),
    .params    = group_params,
    .file      = __FILE__,
};
