/*
 * Copyright 2023 Nope Forge
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

#include <string.h>

#include "internal.h"
#include "log.h"
#include "params.h"
#include "utils/darray.h"
#include "utils/hmap.h"
#include "utils/memory.h"
#include "utils/string.h"
#include "utils/utils.h"

NGLI_RC_CHECK_STRUCT(ngl_scene);

static int check_node_unowned(void *user_arg, struct ngl_node *parent ngli_unused,
                              struct ngl_node *node)
{
    const uint64_t traversal_id = *(const uint64_t *)user_arg;

    if (node->traversal_id == traversal_id)
        return 0;
    node->traversal_id = traversal_id;

    if (node->ctx) {
        LOG(ERROR, "%s still holds resources of a rendering context; release them with "
            "ngl_release_detached_resources() or add the sub-tree back to the graph it came from",
            node->label);
        return NGL_ERROR_INVALID_USAGE;
    }

    return ngli_node_children_apply(check_node_unowned, user_arg, node);
}

static int validate_graph_unowned(struct ngl_node *root)
{
    const uint64_t traversal_id = ngli_node_new_traversal_id();
    return check_node_unowned((void *)&traversal_id, NULL, root);
}

static int reset_nodes(void *user_arg, struct ngl_node *parent, struct ngl_node *node)
{
    struct ngl_scene *s = user_arg;

    if (!node->scene)
        return 0;

    /*
     * This can happen if a failure happened during nodes association, for
     * example if part of the graph was associated with another scene. We make
     * sure to reset only the nodes we actually own.
     */
    if (node->scene != s)
        return 0;

    ngli_assert(!node->ctx);

    int ret = ngli_node_children_apply(reset_nodes, s, node);
    ngli_assert(ret == 0);

    ngli_darray_reset(&node->children);
    ngli_darray_reset(&node->parents);

    node->scene = NULL;
    return 0;
}

static void detach_root(struct ngl_scene *s)
{
    if (!s->params.root)
        return;

    ngli_darray_reset(&s->nodes);

    ngli_darray_reset(&s->files);
    ngli_darray_reset(&s->files_par);

    int ret = reset_nodes(s, NULL, s->params.root);
    ngli_assert(ret == 0);

    ngl_node_unrefp(&s->params.root);
}

static void add_scene_node(struct ngl_scene *s, struct ngl_node *node)
{
    node->scene_index = s->nodes.count;
    ngli_darray_push(&s->nodes, node);
    node->scene = s;

    const struct node_param *par = node->cls->params;
    if (!par)
        return;
    for (; par->key; par++) {
        uint8_t *parp = (uint8_t *)node->opts + par->offset;
        if (!(par->flags & NGLI_PARAM_FLAG_FILEPATH) || !*(char **)parp)
            continue;
        ngli_darray_push(&s->files, *(char **)parp);
        ngli_darray_push(&s->files_par, parp);
    }
}

static void remove_scene_node(struct ngl_scene *s, struct ngl_node *node)
{
    const size_t index = node->scene_index;
    ngli_assert(index < s->nodes.count && s->nodes.data[index] == node);

    /* Swap with the last node for an unordered O(1) removal. */
    s->nodes.data[index] = *ngli_darray_pop(&s->nodes);
    s->nodes.data[index]->scene_index = index;

    const struct node_param *par = node->cls->params;
    if (par) {
        for (; par->key; par++) {
            uint8_t *parp = (uint8_t *)node->opts + par->offset;
            if (!(par->flags & NGLI_PARAM_FLAG_FILEPATH))
                continue;
            for (size_t i = 0; i < s->files_par.count; i++) {
                if (s->files_par.data[i] != parp)
                    continue;
                ngli_darray_remove(&s->files, i);
                ngli_darray_remove(&s->files_par, i);
                break;
            }
        }
    }
    node->scene = NULL;
}

static void add_runtime_edge(struct ngl_node *parent, struct ngl_node *child, size_t index)
{
    ngli_assert(index <= parent->children.count);

    ngli_darray_insert(&parent->children, index, child);
    ngli_darray_push(&child->parents, parent);
}

static struct ngl_node *remove_runtime_edge_at(struct ngl_node *parent, size_t index)
{
    ngli_assert(index < parent->children.count);
    struct ngl_node *child = parent->children.data[index];

    ngli_darray_remove(&parent->children, index);

    const size_t parent_index = ngli_node_darray_find(&child->parents, parent);
    ngli_assert(parent_index != SIZE_MAX);
    ngli_darray_remove(&child->parents, parent_index);

    return child;
}

static void remove_runtime_edge(struct ngl_node *parent, struct ngl_node *child)
{
    const size_t index = ngli_node_darray_find(&parent->children, child);
    ngli_assert(index != SIZE_MAX);
    remove_runtime_edge_at(parent, index);
}

static void remove_scene_edge_at(struct ngl_node *parent, size_t index);
static int add_scene_edge(void *user_arg, struct ngl_node *parent, struct ngl_node *child);

static void remove_scene_children(struct ngl_node *parent)
{
    while (!ngli_darray_is_empty(&parent->children))
        remove_scene_edge_at(parent, parent->children.count - 1);
}

static int check_node_is_shareable(const struct ngl_node *node)
{
    if (!(node->cls->flags & NGLI_NODE_FLAG_SHAREABLE)) {
        LOG(ERROR, "%s (%s) can not be shared within the graph",
            node->label, node->cls->name);
        return NGL_ERROR_INVALID_USAGE;
    }
    return 0;
}

static int add_scene_edge_at(struct ngl_scene *s, struct ngl_node *parent,
                             struct ngl_node *child, size_t index)
{
    if (child->scene) {
        if (child->scene != s) {
            LOG(ERROR, "one or more nodes of the graph are associated with another scene already");
            return NGL_ERROR_INVALID_USAGE;
        }
        int ret = check_node_is_shareable(child);
        if (ret < 0)
            return ret;
        add_runtime_edge(parent, child, index);
        return 0;
    }

    add_runtime_edge(parent, child, index);
    add_scene_node(s, child);

    const int ret = ngli_node_children_apply(add_scene_edge, s, child);
    if (ret < 0) {
        remove_scene_children(child);
        remove_scene_node(s, child);
        remove_runtime_edge(parent, child);
    }
    return ret;
}

static int add_scene_edge(void *user_arg, struct ngl_node *parent, struct ngl_node *child)
{
    return add_scene_edge_at(user_arg, parent, child, parent->children.count);
}

static void remove_scene_edge(struct ngl_node *parent, struct ngli_edge_range *range, struct ngl_node *node)
{
    /*
     * Iterate backwards so removing an entry does not change the indices
     * of entries still to visit.
     */
    for (size_t i = range->index + range->count; i > range->index; i--) {
        if (parent->children.data[i - 1] != node)
            continue;
        remove_scene_edge_at(parent, i - 1);
        range->count--;
    }
}

static void remove_scene_edge_at(struct ngl_node *parent, size_t index)
{
    struct ngl_scene *s = parent->scene;
    ngli_assert(s);

    struct ngl_node *child = remove_runtime_edge_at(parent, index);
    ngli_assert(child->scene == s);
    if (!ngli_darray_is_empty(&child->parents))
        return;

    ngli_assert(child != s->params.root);
    remove_scene_children(child);
    remove_scene_node(s, child);
}

int ngli_scene_add_edges(struct ngl_node *parent, size_t index,
                         size_t nb_nodes, struct ngl_node **nodes)
{
    struct ngl_scene *s = parent->scene;
    ngli_assert(s);

    for (size_t i = 0; i < nb_nodes; i++) {
        const int ret = add_scene_edge_at(s, parent, nodes[i], index + i);
        if (ret < 0) {
            while (i--)
                remove_scene_edge_at(parent, index + i);
            return ret;
        }
    }
    return 0;
}

void ngli_scene_remove_edges(struct ngl_node *parent, struct ngli_edge_range range,
                             size_t nb_nodes, struct ngl_node * const *nodes)
{
    ngli_assert(range.index <= parent->children.count);
    ngli_assert(range.count <= parent->children.count - range.index);

    for (size_t i = 0; i < nb_nodes; i++)
        remove_scene_edge(parent, &range, nodes[i]);
}

void ngli_scene_reparent_edge(struct ngl_node *from, struct ngl_node *to,
                              struct ngl_node *child, size_t index)
{
    ngli_assert(from->scene && from->scene == to->scene && child->scene == from->scene);
    ngli_assert(index <= to->children.count);

    remove_runtime_edge(from, child);
    if (from == to)
        index = NGLI_MIN(index, to->children.count);
    add_runtime_edge(to, child, index);
}

void ngli_scene_swap_edges(struct ngl_node *parent, size_t from, size_t to)
{
    ngli_assert(parent->scene);
    ngli_assert(from < parent->children.count && to < parent->children.count);

    NGLI_SWAP(parent->children.data[from], parent->children.data[to]);
}

struct subtree_check_arg {
    const struct ngl_scene *scene;
    const struct ngli_scene_subtree_check_ctx *check_ctx;
};

static int check_subtree(void *user_arg, struct ngl_node *parent, struct ngl_node *node)
{
    const struct subtree_check_arg *arg = user_arg;
    return ngli_scene_check_subtree(arg->scene, arg->check_ctx, node);
}

/*
 * Validate node's subtree before associating it with scene s.
 *
 * Reject nodes owned by another scene. Nodes already in s, or marked with
 * check_ctx->visited_id, have already been validated: require them to be
 * shareable and do not revisit their descendants.
 *
 * Detect and reject cycles. Reaching a node already marked with
 * check_ctx->visiting_id means it is on the recursion stack and
 * forms a cycle.
 *
 * When reaching a unattached node for the first time, check mandatory
 * parameters and require node->ctx to be NULL or equal to check_ctx->ctx.
 *
 * Mark it visiting before checking its children, then visited on success.
 *
 * Use two fresh, distinct IDs in check_ctx for each operation and reuse them
 * across all input roots. Abort on the first error; fresh IDs for the next
 * operation make clearing marks unnecessary.
 *
 * Do not interleave other traversals that write node->traversal_id.
 */
int ngli_scene_check_subtree(const struct ngl_scene *s,
                             const struct ngli_scene_subtree_check_ctx *check_ctx,
                             struct ngl_node *node)
{
    const struct ngl_ctx *ctx = check_ctx->ctx;

    if (node->scene && node->scene != s) {
        LOG(ERROR, "%s belongs to another scene", node->label);
        return NGL_ERROR_INVALID_USAGE;
    }
    if (node->scene || node->traversal_id == check_ctx->visited_id) {
        return check_node_is_shareable(node);
    }

    if (node->traversal_id == check_ctx->visiting_id) {
        LOG(ERROR, "the sub-tree of %s contains a cycle", node->label);
        return NGL_ERROR_INVALID_ARG;
    }
    if (node->ctx && node->ctx != ctx) {
        if (ctx)
            LOG(ERROR, "%s holds resources of another rendering context", node->label);
        else
            LOG(ERROR, "%s still holds resources of a rendering context; release them with "
                "ngl_release_detached_resources() before associating the sub-tree with this scene",
                node->label);
        return NGL_ERROR_INVALID_USAGE;
    }

    int ret = ngli_node_check_params_sanity(node);
    if (ret < 0)
        return ret;

    node->traversal_id = check_ctx->visiting_id;

    struct subtree_check_arg arg = {
        .scene = s,
        .check_ctx = check_ctx,
    };
    ret = ngli_node_children_apply(check_subtree, &arg, node);
    if (ret < 0)
        return ret;

    node->traversal_id = check_ctx->visited_id;
    return 0;
}

static int attach_root(struct ngl_scene *s, struct ngl_node *node)
{
    const struct ngli_scene_subtree_check_ctx check_ctx = {
        .visiting_id = ngli_node_new_traversal_id(),
        .visited_id = ngli_node_new_traversal_id(),
    };
    int ret = ngli_scene_check_subtree(s, &check_ctx, node);
    if (ret < 0)
        return ret;

    s->params.root = ngl_node_ref(node);

    add_scene_node(s, s->params.root);

    ret = ngli_node_children_apply(add_scene_edge, s, s->params.root);
    if (ret < 0)
        goto fail;

    return 0;

fail:
    remove_scene_children(s->params.root);
    if (s->params.root->scene == s)
        remove_scene_node(s, s->params.root);
    ngl_node_unrefp(&s->params.root);
    return ret;
}

int ngl_scene_get_filepaths(struct ngl_scene *s, char ***filepathsp, size_t *nb_filepathsp)
{
    *filepathsp = NULL;
    *nb_filepathsp = 0;

    if (!s->params.root)
        return NGL_ERROR_INVALID_USAGE;

    *filepathsp = s->files.data;
    *nb_filepathsp = s->files.count;
    return 0;
}

static void update_filepath_ref(struct ngl_scene *s, size_t index, char *str)
{
    char **filep = ngli_darray_get(&s->files, index);
    *filep = str;
}

void ngli_scene_update_filepath_ref(struct ngl_node *node, const struct node_param *par)
{
    struct ngl_scene *s = node->scene;
    for (size_t i = 0; i < s->files_par.count; i++) {
        uint8_t *base_ptr = node->opts;
        uint8_t *parp = base_ptr + par->offset;
        if (s->files_par.data[i] == parp) {
            char *str = *(char **)parp;
            update_filepath_ref(s, i, str);
            return;
        }
    }
    ngli_assert(0);
}

int ngl_scene_update_filepath(struct ngl_scene *s, size_t index, const char *filepath)
{
    if (s->ctx) {
        LOG(ERROR, "the file paths cannot be updated when a rendering context is associated with the scene");
        return NGL_ERROR_INVALID_USAGE;
    }

    if (index >= s->files.count)
        return NGL_ERROR_INVALID_ARG;

    /* Update the node parameter with the new value */
    char *new_str = ngli_strdup(filepath);
    if (!new_str)
        return NGL_ERROR_MEMORY;
    uint8_t **parpp = ngli_darray_get(&s->files_par, index);
    char **dstp = (char **)*parpp;
    ngli_freep(dstp);
    *dstp = new_str;

    update_filepath_ref(s, index, new_str);

    return 0;
}

static void scene_freep(void **sp)
{
    struct ngl_scene *s = *sp;
    if (!s)
        return;
    detach_root(s);
    ngli_freep(sp);
}

struct ngl_scene *ngl_scene_create(void)
{
    struct ngl_scene *s = ngli_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->rc = NGLI_RC_CREATE(scene_freep);
    return s;
}

struct ngl_scene_params ngl_scene_default_params(struct ngl_node *root)
{
    const struct ngl_scene_params params = {
        .root      = root,
        .duration  = 30.0,
        .framerate = {60, 1},
    };
    return params;
}

struct ngl_scene *ngl_scene_ref(struct ngl_scene *s)
{
    return NGLI_RC_REF(s);
}

int ngl_scene_init(struct ngl_scene *s, const struct ngl_scene_params *params)
{
    if (!params->root) {
        LOG(ERROR, "cannot initialize a scene without root node");
        return NGL_ERROR_INVALID_ARG;
    }
    if (params->duration < 0.0) {
        LOG(ERROR, "invalid scene duration %g", params->duration);
        return NGL_ERROR_INVALID_ARG;
    }
    if (params->framerate[0] <= 0 || params->framerate[1] <= 0) {
        LOG(ERROR, "invalid framerate %d/%d", NGLI_ARG_VEC2(params->framerate));
        return NGL_ERROR_INVALID_ARG;
    }
    if (params->width < 0 || params->height < 0) {
        LOG(ERROR, "invalid canvas size %dx%d", params->width, params->height);
        return NGL_ERROR_INVALID_ARG;
    }
    if ((params->width == 0) != (params->height == 0)) {
        LOG(ERROR, "canvas width and height must both be zero or both be positive");
        return NGL_ERROR_INVALID_ARG;
    }

    if (s->params.root) {
        const int ret = validate_graph_unowned(s->params.root);
        if (ret < 0)
            return ret;
    }

    detach_root(s);
    s->params = *params;
    s->params.root = NULL;
    return attach_root(s, params->root);
}

int ngl_scene_init_from_str(struct ngl_scene *s, const char *str)
{
    return ngli_scene_deserialize(s, str);
}

const struct ngl_scene_params *ngl_scene_get_params(const struct ngl_scene *s)
{
    return &s->params;
}

char *ngl_scene_serialize(const struct ngl_scene *s)
{
    return ngli_scene_serialize(s);
}

char *ngl_scene_dot(const struct ngl_scene *s)
{
    return ngli_scene_dot(s);
}

static const struct livectl *get_internal_livectl(const struct ngl_node *node)
{
    const uint8_t *base_ptr = node->opts;
    const struct livectl *ctl = (struct livectl *)(base_ptr + node->cls->livectl_offset);
    return ctl;
}

static int find_livectls(struct ngl_scene *scene, struct hmap *hm)
{
    for (size_t i = 0; i < scene->nodes.count; i++) {
        const struct ngl_node *node = scene->nodes.data[i];
        if (!(node->cls->flags & NGLI_NODE_FLAG_LIVECTL))
            continue;

        const struct livectl *ref_ctl = get_internal_livectl(node);
        if (!ref_ctl->id)
            continue;

        const struct ngl_node *ctl_node = ngli_hmap_get_str(hm, ref_ctl->id);
        if (ctl_node) {
            if (ctl_node != node) {
                LOG(ERROR, "duplicated live control with name \"%s\"", ref_ctl->id);
                return NGL_ERROR_INVALID_USAGE;
            }
            ngli_assert(0); // scene->nodes is a set so each node is supposed to be present only once
        }

        int ret = ngli_hmap_try_set_str(hm, ref_ctl->id, (void *)node);
        if (ret < 0)
            return ret;
    }

    return 0;
}

int ngl_livectls_get(struct ngl_scene *scene, size_t *nb_livectlsp, struct ngl_livectl **livectlsp)
{
    struct ngl_livectl *ctls = NULL;
    *livectlsp = NULL;
    *nb_livectlsp = 0;

    if (!scene->params.root)
        return 0;

    struct hmap *livectls_index = ngli_hmap_try_create(NGLI_HMAP_TYPE_STR);
    if (!livectls_index)
        return NGL_ERROR_MEMORY;

    int ret = find_livectls(scene, livectls_index);
    if (ret < 0)
        goto end;

    const size_t nb = ngli_hmap_count(livectls_index);
    if (!nb)
        goto end;

    /* +1 so that we know when to stop in ngli_node_livectls_freep() */
    ctls = ngli_try_calloc(nb + 1, sizeof(*ctls));
    if (!ctls) {
        ret = NGL_ERROR_MEMORY;
        goto end;
    }

    /*
     * Transfer internal live controls (struct livectl) to public live controls
     * (struct ngl_livectl), with independant ownership (ref counting the node
     * and duplicating memory when needed).
     */
    size_t i = 0;
    const struct hmap_entry *entry = NULL;
    while ((entry = ngli_hmap_next(livectls_index, entry))) {
        struct ngl_node *node = entry->data;

        struct ngl_livectl *ctl = &ctls[i++];
        ctl->node_type = node->cls->id;
        ctl->node = ngl_node_ref(node);

        const struct livectl *ref_ctl = get_internal_livectl(node);
        ctl->id = ngli_strdup(ref_ctl->id);
        if (!ctl->id) {
            ret = NGL_ERROR_MEMORY;
            goto end;
        }
        memcpy(&ctl->val, &ref_ctl->val, sizeof(ctl->val));
        memcpy(&ctl->min, &ref_ctl->min, sizeof(ctl->min));
        memcpy(&ctl->max, &ref_ctl->max, sizeof(ctl->max));

        if (node->cls->id == NGL_NODE_TEXT) {
            ngli_assert(ctl->val.s);
            ngli_assert(!ctl->min.s && !ctl->max.s);
            ctl->val.s = ngli_strdup(ctl->val.s);
            if (!ctl->val.s) {
                ret = NGL_ERROR_MEMORY;
                goto end;
            }
        }
    }

    *livectlsp = ctls;
    *nb_livectlsp = nb;

end:
    if (ret < 0)
        ngl_livectls_freep(&ctls);
    ngli_hmap_freep(&livectls_index);
    return ret;
}

void ngl_livectls_freep(struct ngl_livectl **livectlsp)
{
    struct ngl_livectl *livectls = *livectlsp;
    if (!livectls)
        return;
    for (size_t i = 0; livectls[i].node; i++) {
        struct ngl_livectl *ctl = &livectls[i];
        if (livectls[i].node->cls->id == NGL_NODE_TEXT)
            ngli_freep(&livectls[i].val.s);
        ngl_node_unrefp(&ctl->node);
        ngli_freep(&ctl->id);
    }
    ngli_freep(livectlsp);
}

struct ngl_scene *ngl_scene_duplicate(const struct ngl_scene *s)
{
    if (!s->params.root) {
        LOG(ERROR, "cannot duplicate a scene without root node");
        return NULL;
    }

    /*
     * Always duplicate resources: the duplicated graph must be fully
     * independent because ngl_scene_init() will claim ownership of every
     * node, which would conflict with the original scene's nodes.
     */
    struct ngl_node *dup_root = ngl_node_duplicate(s->params.root, NGL_NODE_DUPLICATE_RESOURCES);
    if (!dup_root)
        return NULL;

    struct ngl_scene *dup = ngl_scene_create();
    if (!dup) {
        ngl_node_unrefp(&dup_root);
        return NULL;
    }

    struct ngl_scene_params params = s->params;
    params.root = dup_root;
    int ret = ngl_scene_init(dup, &params);
    ngl_node_unrefp(&dup_root);
    if (ret < 0) {
        ngl_scene_unrefp(&dup);
        return NULL;
    }

    return dup;
}

void ngl_scene_unrefp(struct ngl_scene **sp)
{
    struct ngl_scene *s = *sp;
    if (!s)
        return;
    NGLI_RC_UNREFP(sp);
}
