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
#include "nopegl/nopegl.h"
#include "internal.h"

struct group_opts {
    struct ngl_node **children;
    size_t nb_children;
};

#define OFFSET(x) offsetof(struct group_opts, x)
static const struct node_param group_params[] = {
    {"children", NGLI_PARAM_TYPE_NODELIST, OFFSET(children),
                 .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                 .desc=NGLI_DOCSTRING("a set of scenes")},
    {NULL}
};

static void group_draw(struct ngl_node *node)
{
    const struct group_opts *o = node->opts;
    for (size_t i = 0; i < o->nb_children; i++)
        ngli_node_draw(o->children[i]);
}

const struct node_class ngli_group_class = {
    .id        = NGL_NODE_GROUP,
    .name      = "Group",
    .update    = ngli_node_update_children,
    .pre_draw  = ngli_node_pre_draw_children,
    .draw      = group_draw,
    .opts_size = sizeof(struct group_opts),
    .params    = group_params,
    .file      = __FILE__,
};
