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

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal.h"
#include "log.h"
#include "node_graph.h"
#include "nopegl/nopegl.h"
#include "params.h"
#include "utils/hmap.h"
#include "utils/utils.h"

static atomic_uint_fast64_t traversal_id_counter;

uint64_t ngli_node_graph_new_traversal_id(void)
{
    /* Zero is reserved for nodes that have not been visited. */
    uint64_t traversal_id = atomic_fetch_add_explicit(&traversal_id_counter, 1, memory_order_relaxed);
    if (traversal_id == 0)
        traversal_id = atomic_fetch_add_explicit(&traversal_id_counter, 1, memory_order_relaxed);
    return traversal_id;
}

int ngli_node_graph_foreach_child(ngli_node_graph_edge_func func, void *user_arg, struct ngl_node *node)
{
    const struct node_param *par = node->cls->params;
    if (!par)
        return 0;

    for (; par->key; par++) {
        uint8_t *parp = (uint8_t *)node->opts + par->offset;

        if (par->type == NGLI_PARAM_TYPE_NODE || (par->flags & NGLI_PARAM_FLAG_ALLOW_NODE)) {
            struct ngl_node *child = *(struct ngl_node **)parp;
            if (child) {
                const int ret = func(user_arg, node, child);
                if (ret < 0)
                    return ret;
            }
        } else if (par->type == NGLI_PARAM_TYPE_NODELIST) {
            const struct ngli_node_darray *array = (const struct ngli_node_darray *)parp;
            for (size_t i = 0; i < array->count; i++) {
                const int ret = func(user_arg, node, array->data[i]);
                if (ret < 0)
                    return ret;
            }
        } else if (par->type == NGLI_PARAM_TYPE_NODEDICT) {
            const struct hmap *hmap = *(struct hmap **)parp;
            const struct hmap_entry *entry = NULL;
            while (hmap && (entry = ngli_hmap_next(hmap, entry))) {
                const int ret = func(user_arg, node, entry->data);
                if (ret < 0)
                    return ret;
            }
        }
    }

    return 0;
}

struct ngli_node_graph_range ngli_node_graph_get_param_range(const struct ngl_node *node,
                                                             const struct node_param *par)
{
    const struct node_param *cur_par = node->cls->params;
    ngli_assert(cur_par != NULL);

    struct ngli_node_graph_range range = {0};
    for (; cur_par->key; cur_par++) {
        const uint8_t *cur_parp = (const uint8_t *)node->opts + cur_par->offset;
        size_t count = 0;
        if (cur_par->type == NGLI_PARAM_TYPE_NODE || (cur_par->flags & NGLI_PARAM_FLAG_ALLOW_NODE)) {
            count = *(struct ngl_node *const *)cur_parp != NULL;
        } else if (cur_par->type == NGLI_PARAM_TYPE_NODELIST) {
            const struct ngli_node_darray *array = (const struct ngli_node_darray *)cur_parp;
            count = array->count;
        } else if (cur_par->type == NGLI_PARAM_TYPE_NODEDICT) {
            const struct hmap *dict = *(struct hmap *const *)cur_parp;
            count = dict ? ngli_hmap_count(dict) : 0;
        }
        if (cur_par == par) {
            range.count = count;
            return range;
        }
        range.index += count;
    }
    ngli_assert(0);
}

struct find_node_arg {
    const uint64_t traversal_id;
    const struct ngl_node *target;
    bool found;
};

static int find_node(void *user_arg, struct ngl_node *parent, struct ngl_node *node)
{
    struct find_node_arg *s = user_arg;

    if (node == s->target) {
        s->found = true;
        return NGL_ERROR_GENERIC;
    }

    if (node->traversal_id == s->traversal_id)
        return 0;
    node->traversal_id = s->traversal_id;

    return ngli_node_graph_foreach_child(find_node, s, node);
}

bool ngli_node_graph_find_node(struct ngl_node *root, const struct ngl_node *target)
{
    ngli_assert(root && target);

    struct find_node_arg arg = {
        .traversal_id = ngli_node_graph_new_traversal_id(),
        .target = target,
    };
    find_node(&arg, NULL, root);
    return arg.found;
}

struct resolve_ctx_arg {
    const uint64_t traversal_id;
    struct ngl_ctx *ctx;
};

static int resolve_ctx(void *user_arg, struct ngl_node *parent, struct ngl_node *node)
{
    struct resolve_ctx_arg *arg = user_arg;
    int ret = ngli_node_check_not_traversing(node);
    if (ret < 0)
        return ret;

    if (node->traversal_id == arg->traversal_id)
        return 0;
    node->traversal_id = arg->traversal_id;

    if (node->ctx) {
        if (arg->ctx && arg->ctx != node->ctx) {
            LOG(ERROR, "subgraph resources belong to multiple contexts");
            return NGL_ERROR_INVALID_USAGE;
        }
        arg->ctx = node->ctx;
    }
    return ngli_node_graph_foreach_child(resolve_ctx, arg, node);
}

int ngli_node_graph_resolve_ctx(struct ngl_node *root, struct ngl_ctx **ctxp)
{
    ngli_assert(root && ctxp);

    *ctxp = NULL;

    struct resolve_ctx_arg arg = {
        .traversal_id = ngli_node_graph_new_traversal_id(),
    };
    const int ret = resolve_ctx(&arg, NULL, root);
    if (ret < 0)
        return ret;

    *ctxp = arg.ctx;
    return 0;
}
