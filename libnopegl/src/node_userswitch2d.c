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
#include "node2d.h"
#include "params.h"

struct userswitch2d_opts {
    struct ngl_node *child;
    struct livectl live;
};

struct userswitch2d_priv {
    struct ngli_node2d_info node2d_info;
};

#define OFFSET(x) offsetof(struct userswitch2d_opts, x)
static const struct node_param userswitch2d_params[] = {
    {
        .key        = "child",
        .type       = NGLI_PARAM_TYPE_NODE,
        .offset     = OFFSET(child),
        .flags      = NGLI_PARAM_FLAG_NON_NULL,
        .node_types = NGLI_NODE2D_TYPES_LIST,
        .desc       = NGLI_DOCSTRING("scene to be rendered or not")
    },
    {
        .key       = "enabled",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(live.val.i),
        .def_value = {.i32=1},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("set if the scene should be rendered")
    },
    {
        .key       = "live_id",
        .type      = NGLI_PARAM_TYPE_STR,
        .offset    = OFFSET(live.id),
        .desc      = NGLI_DOCSTRING("live control identifier")
    },
    {NULL}
};

static int userswitch2d_visit(struct ngl_node *node, bool is_active, double t)
{
    const struct userswitch2d_opts *o = node->opts;
    const int enabled = o->live.val.i[0];
    return ngli_node_visit(o->child, is_active && enabled, t);
}

static int userswitch2d_update(struct ngl_node *node, double t)
{
    const struct userswitch2d_opts *o = node->opts;
    const int enabled = o->live.val.i[0];
    return enabled ? ngli_node_update(o->child, t) : 0;
}

static void userswitch2d_pre_draw(struct ngl_node *node)
{
    struct userswitch2d_priv *s = node->priv_data;
    const struct userswitch2d_opts *o = node->opts;
    const int enabled = o->live.val.i[0];

    if (!enabled) {
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    ngli_node_pre_draw(o->child);

    s->node2d_info.screen_aabb = ngli_node_compute_children_bounding_box(&o->child, 1);
}

static void userswitch2d_draw(struct ngl_node *node)
{
    struct userswitch2d_priv *s = node->priv_data;
    const struct userswitch2d_opts *o = node->opts;
    const int enabled = o->live.val.i[0];

    if (!enabled) {
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    ngli_node_draw(o->child);

    s->node2d_info.screen_aabb = ngli_node_compute_children_bounding_box(&o->child, 1);
}

const struct node_class ngli_userswitch2d_class = {
    .id             = NGL_NODE_USERSWITCH2D,
    .name           = "UserSwitch2D",
    .priv_size      = sizeof(struct userswitch2d_priv),
    .visit          = userswitch2d_visit,
    .update         = userswitch2d_update,
    .pre_draw       = userswitch2d_pre_draw,
    .draw           = userswitch2d_draw,
    .opts_size      = sizeof(struct userswitch2d_opts),
    .params         = userswitch2d_params,
    .flags          = NGLI_NODE_FLAG_LIVECTL | NGLI_NODE_FLAG_2D,
    .livectl_offset = OFFSET(live),
    .file           = __FILE__,
};
