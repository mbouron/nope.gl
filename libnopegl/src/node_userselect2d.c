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
#include "node2d.h"
#include "params.h"

struct userselect2d_opts {
    struct ngl_node **branches;
    size_t nb_branches;
    struct livectl live;
};

struct userselect2d_priv {
    struct ngli_node2d_info node2d_info;
};

static int branch_update_func(struct ngl_node *node)
{
    struct userselect2d_opts *o = node->opts;
    if (!o->live.id)
        return 0;
    if (o->live.val.i[0] < o->live.min.i[0]) {
        LOG(WARNING, "value (%d) is smaller than live_min (%d), clamping", o->live.val.i[0], o->live.min.i[0]);
        o->live.val.i[0] = o->live.min.i[0];
    }
    if (o->live.val.i[0] > o->live.max.i[0]) {
        LOG(WARNING, "value (%d) is larger than live_max (%d), clamping", o->live.val.i[0], o->live.max.i[0]);
        o->live.val.i[0] = o->live.max.i[0];
    }
    return 0;
}

#define OFFSET(x) offsetof(struct userselect2d_opts, x)
static const struct node_param userselect2d_params[] = {
    {
        .key        = "branches",
        .type       = NGLI_PARAM_TYPE_NODELIST,
        .offset     = OFFSET(branches),
        .node_types = NGLI_NODE2D_TYPES_LIST,
        .desc       = NGLI_DOCSTRING("a set of branches to pick from")
    },
    {
        .key         = "branch",
        .type        = NGLI_PARAM_TYPE_I32,
        .offset      = OFFSET(live.val.i),
        .def_value   = {.i32=0},
        .flags       = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .update_func = branch_update_func,
        .desc        = NGLI_DOCSTRING("controls which branch is taken")
    },
    {
        .key       = "live_id",
        .type      = NGLI_PARAM_TYPE_STR,
        .offset    = OFFSET(live.id),
        .desc      = NGLI_DOCSTRING("live control identifier")
    },
    {
        .key       = "live_min",
        .type      = NGLI_PARAM_TYPE_I32,
        .offset    = OFFSET(live.min.i),
        .def_value = {.i32=0},
        .desc      = NGLI_DOCSTRING("minimum value allowed during live change (only honored when live_id is set)")
    },
    {
        .key       = "live_max",
        .type      = NGLI_PARAM_TYPE_I32,
        .offset    = OFFSET(live.max.i),
        .def_value = {.i32=10},
        .desc      = NGLI_DOCSTRING("maximum value allowed during live change (only_honored when live_id is set)")
    },
    {NULL}
};

static int userselect2d_visit(struct ngl_node *node, bool is_active, double t)
{
    const struct userselect2d_opts *o = node->opts;

    const int branch_id = o->live.val.i[0];
    for (size_t i = 0; i < o->nb_branches; i++) {
        struct ngl_node *branch = o->branches[i];
        int ret = ngli_node_visit(branch, is_active && i == branch_id, t);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int userselect2d_update(struct ngl_node *node, double t)
{
    const struct userselect2d_opts *o = node->opts;

    const int branch_id = o->live.val.i[0];
    if (branch_id < 0 || branch_id >= o->nb_branches)
        return 0;
    return ngli_node_update(o->branches[branch_id], t);
}

static void userselect2d_pre_draw(struct ngl_node *node)
{
    struct userselect2d_priv *s = node->priv_data;
    const struct userselect2d_opts *o = node->opts;

    const int branch_id = o->live.val.i[0];
    if (branch_id < 0 || branch_id >= o->nb_branches) {
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    ngli_node_pre_draw(o->branches[branch_id]);

    s->node2d_info.screen_aabb = ngli_node_compute_children_bounding_box(&o->branches[branch_id], 1);
}

static void userselect2d_draw(struct ngl_node *node)
{
    struct userselect2d_priv *s = node->priv_data;
    const struct userselect2d_opts *o = node->opts;

    const int branch_id = o->live.val.i[0];
    if (branch_id < 0 || branch_id >= o->nb_branches) {
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    ngli_node_draw(o->branches[branch_id]);

    s->node2d_info.screen_aabb = ngli_node_compute_children_bounding_box(&o->branches[branch_id], 1);
}

const struct node_class ngli_userselect2d_class = {
    .id             = NGL_NODE_USERSELECT2D,
    .name           = "UserSelect2D",
    .priv_size      = sizeof(struct userselect2d_priv),
    .visit          = userselect2d_visit,
    .update         = userselect2d_update,
    .pre_draw       = userselect2d_pre_draw,
    .draw           = userselect2d_draw,
    .opts_size      = sizeof(struct userselect2d_opts),
    .params         = userselect2d_params,
    .flags          = NGLI_NODE_FLAG_LIVECTL | NGLI_NODE_FLAG_2D,
    .livectl_offset = OFFSET(live),
    .file           = __FILE__,
};
