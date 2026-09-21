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
#include <stdint.h>
#include <string.h>

#include <ngpu/ngpu.h>

#include "internal.h"
#include "log.h"
#include "node_graph.h"
#include "node_graph_edit.h"
#include "nopegl/nopegl.h"
#include "params.h"
#include "renderpass.h"
#include "utils/utils.h"

static struct ngli_node_darray *get_node_list(const struct ngl_node *node, const struct node_param *par)
{
    ngli_assert(par->type == NGLI_PARAM_TYPE_NODELIST);
    return (struct ngli_node_darray *)((uint8_t *)node->opts + par->offset);
}

static int check_new_subtrees(struct ngl_node *node, const struct node_param *par,
                              size_t nb_nodes, struct ngl_node **nodes)
{
    int ret = ngli_params_check_nodes(par, nb_nodes, nodes);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < nb_nodes; i++) {
        if (ngli_node_graph_find_node(nodes[i], node)) {
            LOG(ERROR, "cannot add %s to %s.%s: it would form a graph cycle",
                nodes[i]->label, node->label, par->key);
            return NGL_ERROR_INVALID_ARG;
        }
    }

    const struct ngli_scene_subtree_check_ctx check_ctx = {
        .ctx = node->ctx,
        .visiting_id = ngli_node_graph_new_traversal_id(),
        .visited_id = ngli_node_graph_new_traversal_id(),
    };
    for (size_t i = 0; i < nb_nodes; i++) {
        ret = ngli_scene_check_subtree(node->scene, &check_ctx, nodes[i]);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int check_subtree_renderpass_requirements(const struct ngl_node *container,
                                                 const struct ngpu_rendertarget_layout *layout,
                                                 size_t nb_nodes, struct ngl_node **nodes)
{
    const enum ngpu_format format = layout->depth_stencil.format;
    if (ngpu_format_has_depth(format) && ngpu_format_has_stencil(format))
        return 0;

    for (size_t i = 0; i < nb_nodes; i++) {
        struct renderpass_reqs info = {0};
        ngli_node_get_renderpass_reqs(&nodes[i], 1, &info);

        if ((info.usage & NGLI_RENDERPASS_USAGE_DEPTH) && !ngpu_format_has_depth(format)) {
            LOG(ERROR, "cannot add %s to %s: its subgraph uses depth testing, "
                "which requires a render target with a depth attachment",
                nodes[i]->label, container->label);
            return NGL_ERROR_INVALID_USAGE;
        }

        if ((info.usage & NGLI_RENDERPASS_USAGE_STENCIL) && !ngpu_format_has_stencil(format)) {
            LOG(ERROR, "cannot add %s to %s: its subgraph uses stencil operations, "
                "which requires a render target with a stencil attachment",
                nodes[i]->label, container->label);
            return NGL_ERROR_INVALID_USAGE;
        }
    }

    return 0;
}

static int attach_new_subtrees(struct ngl_node *container, size_t nb_nodes, struct ngl_node **nodes)
{
    struct ngl_ctx *ctx = container->ctx;

    struct ngpu_rendertarget_layout layout;
    ngli_node_get_rendertarget_layout(container, &layout);

    int ret = check_subtree_renderpass_requirements(container, &layout, nb_nodes, nodes);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < nb_nodes; i++) {
        ret = ngli_node_set_ctx(nodes[i], ctx);
        if (ret < 0)
            return ret;
    }

    return ngli_node_prepare_nodes(ctx, nb_nodes, nodes, &layout);
}

static void invalidate_subtree(struct ngl_node *node, uint64_t traversal_id)
{
    if (node->traversal_id == traversal_id)
        return;
    node->traversal_id = traversal_id;

    for (size_t i = 0; i < node->children.count; i++)
        invalidate_subtree(node->children.data[i], traversal_id);

    node->visit_time = -1.;
    node->last_update_time = -1.;
    if (node->cls->invalidate)
        node->cls->invalidate(node);
}

static void invalidate_subtrees(size_t nb_nodes, struct ngl_node **nodes)
{
    const uint64_t traversal_id = ngli_node_graph_new_traversal_id();
    for (size_t i = 0; i < nb_nodes; i++)
        invalidate_subtree(nodes[i], traversal_id);
}

static int apply_insert_children(struct ngl_node *node, const struct node_param *par,
                                 size_t index, size_t nb_nodes, struct ngl_node **nodes)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngl_scene *scene = node->scene;
    struct ngli_node_darray *list = get_node_list(node, par);

    if (scene) {
        int ret = check_new_subtrees(node, par, nb_nodes, nodes);
        if (ret < 0)
            return ret;
    }

    /* Save the edge positions before the node-list parameter is modified */
    struct ngli_node_graph_range range = {0};
    if (scene)
        range = ngli_node_graph_get_param_range(node, par);

    int ret = ngli_params_insert_nodes((uint8_t *)list, par, index, nb_nodes, nodes);
    if (ret < 0)
        return ret;

    if (scene) {
        ret = ngli_scene_add_edges(node, range.index + index, nb_nodes, nodes);
        if (ret < 0)
            goto revert;
        if (ctx) {
            /* If the container have a context, attach the new subtrees to it. */
            ret = attach_new_subtrees(node, nb_nodes, nodes);
            if (ret < 0)
                goto revert_edges;
            invalidate_subtrees(nb_nodes, nodes);
        }
    }

    return 0;

revert_edges:
    /* Rollback newly inserted edges */
    ngli_scene_remove_edges_at(node, range.index + index, nb_nodes);
    /* Rollback newly inserted nodes */
    ngli_darray_remove_range(list, index, nb_nodes);
    /*
     * Re-prepare the restored scene as shared subgraphs could have been
     * prepared against a different render state
     */
    if (ngli_node_prepare(scene->params.root, &ctx->default_rendertarget_layout) < 0)
        ngli_ctx_drop_scene(ctx);
    return ret;

revert:
    /* Rollback newly inserted nodes */
    ngli_darray_remove_range(list, index, nb_nodes);
    return ret;
}

static int apply_remove_children(struct ngl_node *node, const struct node_param *par,
                                 size_t nb_nodes, struct ngl_node **nodes)
{
    struct ngl_scene *scene = node->scene;
    struct ngli_node_darray *list = get_node_list(node, par);

    /* Save the edge positions before the node-list parameter is modified */
    struct ngli_node_graph_range range = {0};
    if (scene)
        range = ngli_node_graph_get_param_range(node, par);

    /*
     * The `nodes` pointers may be borrowed and the node-list parameter may hold
     * the last references; take a reference on the `nodes` so they remain valid
     * until the scene edges are removed.
     */
    for (size_t i = 0; i < nb_nodes; i++)
      ngl_node_ref(nodes[i]);

    /*
     * First, remove the nodes from the node-list (which validates the
     * membership of the whole batch). Then, remove the corresponding edges from
     * the scene.
     */
    int ret = ngli_params_remove_nodes((uint8_t *)list, par, nb_nodes, nodes);
    if (ret >= 0 && scene)
        ngli_scene_remove_edges(node, range, nb_nodes, nodes);

    for (size_t i = 0; i < nb_nodes; i++) {
        struct ngl_node *child = nodes[i];
        ngl_node_unrefp(&child);
    }
    return ret;
}

static void apply_swap_children(struct ngl_node *node, const struct node_param *par,
                                size_t from, size_t to)
{
    struct ngli_node_darray *list = get_node_list(node, par);
    if (node->scene) {
        const struct ngli_node_graph_range range = ngli_node_graph_get_param_range(node, par);
        ngli_scene_swap_edges(node, range.index + from, range.index + to);
    }
    NGLI_SWAP(list->data[from], list->data[to]);
}

struct node_edit_arg {
    struct ngl_node *node;
    const struct node_param *par;
    size_t nb_nodes;
    struct ngl_node **nodes;
    size_t from_index;
    size_t to_index;
};

static int run_node_edit(struct ngl_node *node, int (*fn)(struct ngl_ctx *, void *), void *arg)
{
    if (node->ctx)
        return ngli_ctx_dispatch(node->ctx, fn, arg);
    return fn(NULL, arg);
}

static int insert_children_cb(struct ngl_ctx *ctx, void *user_arg)
{
    const struct node_edit_arg *arg = user_arg;
    int ret = apply_insert_children(arg->node, arg->par, arg->to_index, arg->nb_nodes, arg->nodes);
    if (ret < 0)
        return ret;
    return ngli_node_param_notify(arg->node, arg->par);
}

static int remove_children_cb(struct ngl_ctx *ctx, void *user_arg)
{
    const struct node_edit_arg *arg = user_arg;
    int ret = apply_remove_children(arg->node, arg->par, arg->nb_nodes, arg->nodes);
    if (ret < 0)
        return ret;
    return ngli_node_param_notify(arg->node, arg->par);
}

static int swap_children_cb(struct ngl_ctx *ctx, void *user_arg)
{
    const struct node_edit_arg *arg = user_arg;
    apply_swap_children(arg->node, arg->par, arg->from_index, arg->to_index);
    return ngli_node_param_notify(arg->node, arg->par);
}

int ngli_node_graph_edit_add_children(struct ngl_node *node, const struct node_param *par,
                                      size_t nb_nodes, struct ngl_node **nodes)
{
    return ngli_node_graph_edit_insert_children(node, par, get_node_list(node, par)->count, nb_nodes, nodes);
}

int ngli_node_graph_edit_insert_children(struct ngl_node *node, const struct node_param *par,
                                         size_t index, size_t nb_nodes, struct ngl_node **nodes)
{
    if (index > get_node_list(node, par)->count)
        return NGL_ERROR_INVALID_ARG;

    if (!nb_nodes)
        return 0;

    struct node_edit_arg arg = {
        .node     = node,
        .par      = par,
        .nb_nodes = nb_nodes,
        .nodes    = nodes,
        .to_index = index,
    };
    return run_node_edit(node, insert_children_cb, &arg);
}

int ngli_node_graph_edit_remove_children(struct ngl_node *node, const struct node_param *par,
                                         size_t nb_nodes, struct ngl_node **nodes)
{
    if (!nb_nodes)
        return 0;

    struct node_edit_arg arg = {
        .node     = node,
        .par      = par,
        .nb_nodes = nb_nodes,
        .nodes    = nodes,
    };
    return run_node_edit(node, remove_children_cb, &arg);
}

int ngli_node_graph_edit_swap_children(struct ngl_node *node, const struct node_param *par,
                                       size_t from, size_t to)
{
    const struct ngli_node_darray *list = get_node_list(node, par);
    if (from >= list->count || to >= list->count)
        return NGL_ERROR_INVALID_ARG;

    if (from == to)
        return 0;

    struct node_edit_arg arg = {
        .node       = node,
        .par        = par,
        .from_index = from,
        .to_index   = to,
    };
    return run_node_edit(node, swap_children_cb, &arg);
}
