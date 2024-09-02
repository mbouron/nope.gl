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

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "nopegl.h"
#include "internal.h"

struct group_opts {
    struct ngl_node **children;
    size_t nb_children;
};

struct group_priv {
    struct darray indices;
};

static int group_swap_children(struct ngl_node *node, size_t from, size_t to)
{
    struct group_priv *s = node->priv_data;

    size_t *indices = ngli_darray_data(&s->indices);
    NGLI_SWAP(size_t, indices[from], indices[to]);

    return 0;
}

#define OFFSET(x) offsetof(struct group_opts, x)
static const struct node_param group_params[] = {
    {"children", NGLI_PARAM_TYPE_NODELIST, OFFSET(children),
                 .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                 .swap_func=group_swap_children,
                 .desc=NGLI_DOCSTRING("a set of scenes")},
    {NULL}
};

static int group_init(struct ngl_node *node)
{
    struct group_priv *s = node->priv_data;

    ngli_darray_init(&s->indices, sizeof(size_t), 0);

    return 0;
}

static int group_prepare(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct group_priv *s = node->priv_data;
    const struct group_opts *o = node->opts;

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
}

static void group_uninit(struct ngl_node *node)
{
    struct group_priv *s = node->priv_data;

    ngli_darray_reset(&s->indices);
}

const struct node_class ngli_group_class = {
    .id        = NGL_NODE_GROUP,
    .name      = "Group",
    .priv_size = sizeof(struct group_priv),
    .init      = group_init,
    .prepare   = group_prepare,
    .update    = ngli_node_update_children,
    .draw      = group_draw,
    .uninit    = group_uninit,
    .opts_size = sizeof(struct group_opts),
    .params    = group_params,
    .file      = __FILE__,
};
