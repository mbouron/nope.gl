/*
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

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aabb.h"
#include "internal.h"
#include "node2d.h"
#include "math_utils.h"
#include "log.h"
#include "node_uniform.h"
#include "nodes_register.h"
#include "nopegl/nopegl.h"
#include "params.h"
#include "utils/hmap.h"
#include "utils/memory.h"
#include "utils/string.h"
#include "utils/utils.h"

/* We depend on the monotonically incrementing by 1 property of these fields */
NGLI_STATIC_ASSERT(NGL_NODE_UNIFORMVEC4      - NGL_NODE_UNIFORMFLOAT       == 3, "node uniform vec flt");
NGLI_STATIC_ASSERT(NGL_NODE_ANIMKEYFRAMEVEC4 - NGL_NODE_ANIMKEYFRAMEFLOAT  == 3, "node animkf vec flt");
NGLI_STATIC_ASSERT(NGL_NODE_ANIMATEDVEC4     - NGL_NODE_ANIMATEDFLOAT      == 3, "node anim vec flt");

/* Warning: the common node parameters *must* not include any node-based parameter */
#define OFFSET(x) offsetof(struct ngl_node, x)
const struct node_param ngli_base_node_params[] = {
    {"label",     NGLI_PARAM_TYPE_STR,      OFFSET(label)},
    {NULL}
};

static void *aligned_allocz(size_t size)
{
    void *ptr = ngli_malloc_aligned(NGLI_ALIGN_VAL, size);
    if (!ptr)
        return NULL;
    memset(ptr, 0, size);
    return ptr;
}

static struct ngl_node *node_create(const struct node_class *cls)
{
    struct ngl_node *node;
    const size_t node_size = NGLI_ALIGN(sizeof(*node), NGLI_ALIGN_VAL);
    const size_t opts_size = NGLI_ALIGN(cls->opts_size, NGLI_ALIGN_VAL);
    const size_t priv_size = NGLI_ALIGN(cls->priv_size, NGLI_ALIGN_VAL);

    node = aligned_allocz(node_size + opts_size + priv_size);
    if (!node)
        return NULL;
    node->opts = ((uint8_t *)node) + node_size;
    node->priv_data = ((uint8_t *)node->opts) + opts_size;

    /* Make sure the node, opts, and its private data are properly aligned */
    ngli_assert(NGLI_IS_ALIGNED((uintptr_t)node, NGLI_ALIGN_VAL));
    ngli_assert(NGLI_IS_ALIGNED((uintptr_t)node->opts, NGLI_ALIGN_VAL));
    ngli_assert(NGLI_IS_ALIGNED((uintptr_t)node->priv_data, NGLI_ALIGN_VAL));

    node->cls = cls;
    node->last_update_time = -1.;
    node->visit_time = -1.;

    node->refcount = 1;

    node->state = NGLI_NODE_STATE_UNINITIALIZED;

    return node;
}

#define DEF_NAME_CHR(c) (((c) >= 'A' && (c) <= 'Z') ? (c) ^ 0x20 : (c))

static char *node_default_label(const char *class_name)
{
    char *label = ngli_strdup(class_name);
    if (!label)
        return NULL;
    for (size_t i = 0; label[i]; i++)
        label[i] = DEF_NAME_CHR(label[i]);
    return label;
}

int ngli_is_default_label(const char *class_name, const char *str)
{
    const size_t len = strlen(class_name);
    if (len != strlen(str))
        return 0;
    for (size_t i = 0; i < len; i++)
        if (DEF_NAME_CHR(class_name[i]) != str[i])
            return 0;
    return 1;
}

#define REGISTER_NODE(type_name, cls)           \
    case type_name: {                           \
        extern const struct node_class cls;     \
        ngli_assert(cls.id == type_name);       \
        return &cls;                            \
    }                                           \

static const struct node_class *get_node_class(uint32_t type)
{
    switch (type) {
        NODE_MAP_TYPE2CLASS(REGISTER_NODE)
        default:
            return NULL;
    }
}

struct ngl_node *ngl_node_create(uint32_t type)
{
    const struct node_class *cls = get_node_class(type);
    if (!cls) {
        LOG(ERROR, "unknown node type 0x%x", type);
        return NULL;
    }

    struct ngl_node *node = node_create(cls);
    if (!node)
        return NULL;

    if (ngli_params_set_defaults((uint8_t *)node, ngli_base_node_params) < 0 ||
        ngli_params_set_defaults(node->opts, node->cls->params) < 0) {
        ngl_node_unrefp(&node);
        return NULL;
    }

    node->label = node_default_label(node->cls->name);
    if (!node->label) {
        ngl_node_unrefp(&node);
        return NULL;
    }

    LOG(VERBOSE, "CREATED %s @ %p", node->label, node);

    return node;
}

static void node_release(struct ngl_node *node)
{
    if (node->state != NGLI_NODE_STATE_READY)
        return;

    ngli_assert(node->ctx);
    if (node->cls->release) {
        TRACE("RELEASE %s @ %p", node->label, node);
        node->cls->release(node);
    }
    node->state = NGLI_NODE_STATE_INITIALIZED;
    node->last_update_time = -1.;
}

static void node_uninit(struct ngl_node *node)
{
    if (node->state == NGLI_NODE_STATE_UNINITIALIZED)
        return;

    ngli_assert(node->ctx);
    node_release(node);

    if (node->cls->uninit) {
        LOG(VERBOSE, "UNINIT %s @ %p", node->label, node);
        node->cls->uninit(node);
    }
    memset(node->priv_data, 0, node->cls->priv_size);
    node->state = NGLI_NODE_STATE_UNINITIALIZED;
    node->prepared = false;
    node->visit_time = -1.;
}

static int node_init(struct ngl_node *node)
{
    if (node->state != NGLI_NODE_STATE_UNINITIALIZED)
        return 0;

    ngli_assert(node->ctx);
    if (node->cls->init) {
        LOG(VERBOSE, "INIT %s @ %p", node->label, node);
        int ret = node->cls->init(node);
        if (ret < 0) {
            LOG(ERROR, "initializing node %s failed: %s", node->label, NGLI_RET_STR(ret));
            node->state = NGLI_NODE_STATE_INIT_FAILED;
            node_uninit(node);
            return ret;
        }
    }

    if (node->cls->prefetch)
        node->state = NGLI_NODE_STATE_INITIALIZED;
    else
        node->state = NGLI_NODE_STATE_READY;

    return 0;
}

static int node_set_ctx(struct ngl_node *node, struct ngl_ctx *ctx)
{
    int ret;

    for (size_t i = 0; i < node->children.count; i++) {
        struct ngl_node *child = node->children.data[i];
        ret = node_set_ctx(child, ctx);
        if (ret < 0)
            return ret;
    }

    node->ctx = ctx;
    ret = node_init(node);
    if (ret < 0) {
        node->ctx = NULL;
        return ret;
    }
    node->ctx_refcount++;

    return 0;
}

static void node_reset_ctx(struct ngl_node *node, struct ngl_ctx *ctx)
{
    if (node->state > NGLI_NODE_STATE_UNINITIALIZED) {
        if (node->ctx != ctx)
            return;
        if (node->ctx_refcount-- == 1) {
            node_uninit(node);
            node->ctx = NULL;
        }
    }
    ngli_assert(node->ctx_refcount >= 0);

    for (size_t i = 0; i < node->children.count; i++) {
        struct ngl_node *child = node->children.data[i];
        node_reset_ctx(child, ctx);
    }
}

int ngli_node_attach_ctx(struct ngl_node *node, struct ngl_ctx *ctx)
{
    int ret = node_set_ctx(node, ctx);
    if (ret < 0)
        return ret;

    ret = ngli_node_prepare(node, &ctx->default_graphics_state, &ctx->default_rendertarget_layout);
    if (ret < 0)
        return ret;

    return ret;
}

void ngli_node_detach_ctx(struct ngl_node *node, struct ngl_ctx *ctx)
{
    node_reset_ctx(node, ctx);
}

int ngli_node_prepare(struct ngl_node *node,
                      const struct ngpu_graphics_state *graphics_state,
                      const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    if (node->prepared)
        return 0;
    node->prepared = true;

    /* Compute render state for children (may be overridden by this node) */
    struct ngpu_graphics_state child_graphics_state = *graphics_state;
    struct ngpu_rendertarget_layout child_rendertarget_layout = *rendertarget_layout;
    if (node->cls->get_child_render_state) {
        node->cls->get_child_render_state(node, graphics_state, rendertarget_layout,
                                          &child_graphics_state, &child_rendertarget_layout);
    }

    /* Leaf-first: prepare all children before this node */
    for (size_t i = 0; i < node->children.count; i++) {
        int ret = ngli_node_prepare(node->children.data[i], &child_graphics_state, &child_rendertarget_layout);
        if (ret < 0)
            return ret;
    }

    /* Prepare this node */
    if (node->cls->prepare) {
        TRACE("PREPARE %s @ %p", node->label, node);
        int ret = node->cls->prepare(node, graphics_state, rendertarget_layout);
        if (ret < 0) {
            LOG(ERROR, "preparing node %s failed: %s", node->label, NGLI_RET_STR(ret));
            return ret;
        }
    }

    return 0;
}

int ngli_node_visit(struct ngl_node *node, bool is_active, double t)
{
    /*
     * If a node is inactive and meant to be, there is no need
     * to check for resources below as we can assume they were already released
     * as well (unless they're shared with another branch) by
     * honor_release_prefetch().
     *
     * On the other hand, we cannot do the same if the node is active, because
     * we have to mark every node below for activity to prevent an early
     * release from another branch.
     */
    if (!is_active && !node->is_active)
        return 0;

    const int queue_node = node->visit_time != t;

    if (queue_node) {
        /*
         * If a node is active or is going to be activated but has already been
         * updated for that time previously, we need to force its update. This
         * scenario is rare but can happen with specific time sequences on time
         * filtered diamond-tree graphs.
         */
        if (is_active && node->last_update_time == t)
            node->last_update_time = -1.;
        /*
         * If we never passed through this node for that given time, the new
         * active state takes over to replace the one from a previous update.
         */
        node->is_active = is_active;
        node->visit_time = t;
    } else {
        /*
         * This is not the first time we come across that node, so if it's
         * needed in that part of the branch we mark it as active so it doesn't
         * get released.
         */
        node->is_active |= is_active;
    }

    if (node->cls->visit) {
        int ret = node->cls->visit(node, is_active, t);
        if (ret < 0)
            return ret;
    } else {
        struct ngli_node_darray *children_array = &node->children;
        struct ngl_node **children = children_array->data;
        for (size_t i = 0; i < children_array->count; i++) {
            struct ngl_node *child = children[i];
            int ret = ngli_node_visit(child, is_active, t);
            if (ret < 0)
                return ret;
        }
    }

    /* Insert children (leaves) first */
    if (queue_node &&
        (node->cls->prefetch || node->cls->release) &&
        ngli_darray_push(&node->ctx->activitycheck_nodes, node) < 0)
        return NGL_ERROR_MEMORY;

    return 0;
}

static int node_prefetch(struct ngl_node *node)
{
    if (node->state == NGLI_NODE_STATE_READY)
        return 0;

    if (node->cls->prefetch) {
        TRACE("PREFETCH %s @ %p", node->label, node);
        int ret = node->cls->prefetch(node);
        if (ret < 0) {
            LOG(ERROR, "prefetching node %s failed: %s", node->label, NGLI_RET_STR(ret));
            node->visit_time = -1.;
            if (node->cls->release) {
                LOG(VERBOSE, "RELEASE %s @ %p", node->label, node);
                node->cls->release(node);
            }
            return ret;
        }
    }
    node->state = NGLI_NODE_STATE_READY;

    return 0;
}

int ngli_node_honor_release_prefetch(struct ngl_node *scene, double t)
{
    /* Build a new list of activity checks nodes */
    struct ngli_node_darray *nodes_array = &scene->ctx->activitycheck_nodes;
    ngli_darray_clear(nodes_array);
    int ret = ngli_node_visit(scene, true, t);
    if (ret < 0)
        return ret;

    struct ngl_node **nodes = nodes_array->data;

    /* Release nodes starting from the parents (root) down to the children (leaves) */
    for (size_t i = 0; i < nodes_array->count; i++) {
        struct ngl_node *node = nodes[nodes_array->count - i - 1];
        if (!node->is_active || node->force_release_prefetch) {
            node_release(node);
            node->force_release_prefetch = false;
        }
    }

    /* Prefetch nodes starting from the children (leaves) up to the parents (root) */
    for (size_t i = 0; i < nodes_array->count; i++) {
        struct ngl_node *node = nodes[i];
        if (node->is_active) {
            ret = node_prefetch(node);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

int ngli_node_update(struct ngl_node *node, double t)
{
    ngli_assert(node->state == NGLI_NODE_STATE_READY);
    if (node->cls->update) {
        if (node->last_update_time != t) {
            TRACE("UPDATE %s @ %p with t=%g", node->label, node, t);
            int ret = node->cls->update(node, t);
            if (ret < 0) {
                LOG(ERROR, "updating node %s failed: %s", node->label, NGLI_RET_STR(ret));
                return ret;
            }
            node->last_update_time = t;
            node->draw_count = 0;
        } else {
            TRACE("%s already updated for t=%g, skip it", node->label, t);
        }
    }

    return 0;
}

int ngli_node_update_children(struct ngl_node *node, double t)
{
    for (size_t i = 0; i < node->children.count; i++) {
        struct ngl_node *child = node->children.data[i];
        int ret = ngli_node_update(child, t);
        if (ret < 0)
            return ret;
    }
    return 0;
}

void *ngli_node_get_data_ptr(const struct ngl_node *var_node, const void *data_fallback)
{
    if (!var_node)
        return (void *)data_fallback;
    ngli_assert(var_node->cls->category == NGLI_NODE_CATEGORY_VARIABLE);
    struct variable_info *var = var_node->priv_data;
    return var->data;
}

void ngli_node_pre_draw(struct ngl_node *node)
{
    if (!node->is_active)
        return;
    if (node->cls->pre_draw)
        node->cls->pre_draw(node);
    else
        ngli_node_pre_draw_children(node);
}

void ngli_node_pre_draw_children(struct ngl_node *node)
{
    for (size_t i = 0; i < node->children.count; i++) {
        struct ngl_node *child = node->children.data[i];
        ngli_node_pre_draw(child);
    }
}

void ngli_node_draw_children(struct ngl_node *node)
{
    for (size_t i = 0; i < node->children.count; i++) {
        struct ngl_node *child = node->children.data[i];
        ngli_node_draw(child);
    }
}

static bool has_bounding_box(const struct ngl_node *node)
{
    return NGLI_HAS_ALL_FLAGS(node->cls->flags, NGLI_NODE_FLAG_2D);
}

struct aabb ngli_node_compute_children_bounding_box(struct ngl_node *const *children, size_t nb_children)
{
    struct aabb aabb = NGLI_AABB_EMPTY;
    for (size_t i = 0; i < nb_children; i++) {
        const struct ngl_node *child = children[i];
        if (!has_bounding_box(child))
            continue;
        const struct ngli_node2d_info *child_info = child->priv_data;
        aabb = ngli_aabb_union(&aabb, &child_info->screen_aabb);
    }
    return aabb;
}

void ngli_node_draw(struct ngl_node *node)
{
    if (node->cls->draw) {
        TRACE("DRAW %s @ %p", node->label, node);
        node->cls->draw(node);
        node->draw_count++;
    }

    if (has_bounding_box(node))
        ngli_darray_push(&node->ctx->bounding_box_nodes, node);
}

const struct node_param *ngli_node_param_find(const struct ngl_node *node, const char *key,
                                              uint8_t **base_ptrp)
{
    const struct node_param *par = ngli_params_find(ngli_base_node_params, key);
    *base_ptrp = (uint8_t *)node;

    if (!par) {
        par = ngli_params_find(node->cls->params, key);
        *base_ptrp = (uint8_t *)node->opts;
    }
    if (!par)
        LOG(ERROR, "parameter %s not found in %s", key, node->cls->name);
    return par;
}

static int param_add(struct ngl_node *node, const char *key, size_t nb_elems, void *elems)
{
    int ret = 0;

    uint8_t *base_ptr;
    const struct node_param *par = ngli_node_param_find(node, key, &base_ptr);
    if (!par)
        return NGL_ERROR_NOT_FOUND;

    if (node->ctx && !(par->flags & NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE)) {
        LOG(ERROR, "%s.%s can not be live extended", node->label, key);
        return NGL_ERROR_INVALID_USAGE;
    }

    ret = ngli_params_add(base_ptr, par, nb_elems, elems);
    if (ret < 0) {
        LOG(ERROR, "unable to add elements to %s.%s", node->label, key);
        return ret;
    }

    if (node->ctx && par->update_func)
        ret = par->update_func(node);

    return ret;
}

int ngl_node_param_add_nodes(struct ngl_node *node, const char *key,
                             size_t nb_nodes, struct ngl_node **nodes)
{
    if (node->scene) {
        LOG(ERROR, "the nodes graph cannot be extended after being associated with a scene");
        return NGL_ERROR_INVALID_USAGE;
    }
    return param_add(node, key, nb_nodes, nodes);
}

int ngl_node_param_add_f64s(struct ngl_node *node, const char *key,
                            size_t nb_f64s, double *f64s)
{
    return param_add(node, key, nb_f64s, f64s);
}

int ngl_node_param_swap_elem(struct ngl_node *node, const char *key,
                             size_t from, size_t to)
{
    uint8_t *base_ptr;
    const struct node_param *par = ngli_node_param_find(node, key, &base_ptr);
    if (!par)
        return NGL_ERROR_NOT_FOUND;

    if (node->ctx && !(par->flags & NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE)) {
        LOG(ERROR, "%s.%s can not be live extended", node->label, key);
        return NGL_ERROR_INVALID_USAGE;
    }

    int ret = ngli_params_swap_elem(base_ptr, par, from, to);
    if (ret < 0) {
        LOG(ERROR, "unable to add elements to %s.%s", node->label, key);
        return ret;
    }

    if (!node->ctx)
        return ret;

    if (par->update_func)
        ret = par->update_func(node);

    if (par->swap_func)
        ret = par->swap_func(node, from, to);

    return ret;
}

int ngli_node_invalidate_branch(struct ngl_node *node)
{
    node->visit_time = -1.;
    node->last_update_time = -1;
    if (node->cls->invalidate) {
        int ret = node->cls->invalidate(node);
        if (ret < 0)
            return ret;
    }
    for (size_t i = 0; i < node->parents.count; i++) {
        int ret = ngli_node_invalidate_branch(node->parents.data[i]);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int node_param_is_value_allowed(struct ngl_node *node, const char *key,
                                       const uint8_t *ptr, const struct node_param *par)
{
    if (!node->ctx)
        return 0;

    if (!(par->flags & NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE)) {
        LOG(ERROR, "%s.%s can not be live changed", node->label, key);
        return NGL_ERROR_INVALID_USAGE;
    }

    if (par->flags & NGLI_PARAM_FLAG_ALLOW_NODE) {
        const struct ngl_node *pnode = *(struct ngl_node **)ptr;
        if (pnode) {
            LOG(ERROR, "%s.%s can not be live changed because it is associated with a node", node->label, key);
            return NGL_ERROR_INVALID_USAGE;
        }
    }

    return 0;
}

struct node_param_update_arg {
    struct ngl_node *node;
    const struct node_param *par;
};

static int node_param_update_cb(struct ngl_ctx *ctx, void *arg)
{
    const struct node_param_update_arg *a = arg;
    if (a->par->update_func) {
        int ret = a->par->update_func(a->node);
        if (ret < 0)
            return ret;
    }
    return ngli_node_invalidate_branch(a->node);
}

static int node_param_update(struct ngl_node *node, const struct node_param *par)
{
    if (node->scene && par->flags & NGLI_PARAM_FLAG_FILEPATH)
        ngli_scene_update_filepath_ref(node, par);

    if (!node->ctx)
        return 0;

    struct node_param_update_arg arg = { .node = node, .par = par };
    return node->ctx->api_impl->dispatch(node->ctx, node_param_update_cb, &arg);
}

#define FORWARD_TO_PARAM(type, ...)                                     \
    int ret;                                                            \
    uint8_t *base_ptr;                                                  \
    const struct node_param *par =                                      \
        ngli_node_param_find(node, key, &base_ptr);                     \
    if (!par)                                                           \
        return NGL_ERROR_NOT_FOUND;                                     \
    uint8_t *dst = base_ptr + par->offset;                              \
    if ((ret = node_param_is_value_allowed(node, key, dst, par)) < 0 || \
        (ret = ngli_params_set_##type(dst, par, __VA_ARGS__)) < 0 ||    \
        (ret = node_param_update(node, par)) < 0)                       \
        return ret;                                                     \
    return 0

int ngl_node_param_set_bool(struct ngl_node *node, const char *key, int value)
{
    FORWARD_TO_PARAM(bool, value);
}

int ngl_node_param_set_data(struct ngl_node *node, const char *key, size_t size, const void *data)
{
    FORWARD_TO_PARAM(data, size, data);
}

int ngl_node_param_set_f32(struct ngl_node *node, const char *key, float value)
{
    FORWARD_TO_PARAM(f32, value);
}

int ngl_node_param_set_f64(struct ngl_node *node, const char *key, double value)
{
    FORWARD_TO_PARAM(f64, value);
}

int ngl_node_param_set_flags(struct ngl_node *node, const char *key, const char *value)
{
    FORWARD_TO_PARAM(flags, value);
}

int ngl_node_param_set_i32(struct ngl_node *node, const char *key, int32_t value)
{
    FORWARD_TO_PARAM(i32, value);
}

int ngl_node_param_set_ivec2(struct ngl_node *node, const char *key, const int32_t *value)
{
    FORWARD_TO_PARAM(ivec2, value);
}

int ngl_node_param_set_ivec3(struct ngl_node *node, const char *key, const int32_t *value)
{
    FORWARD_TO_PARAM(ivec3, value);
}

int ngl_node_param_set_ivec4(struct ngl_node *node, const char *key, const int32_t *value)
{
    FORWARD_TO_PARAM(ivec4, value);
}

int ngl_node_param_set_mat4(struct ngl_node *node, const char *key, const float *value)
{
    FORWARD_TO_PARAM(mat4, value);
}

int ngl_node_param_set_node(struct ngl_node *node, const char *key, struct ngl_node *value)
{
    if (node->ctx) {
        LOG(ERROR, "%s.%s node can not be live changed", node->label, key);
        return NGL_ERROR_INVALID_USAGE;
    }
    if (node->scene) {
        LOG(ERROR, "the structure of the graph cannot be changed after being associated with a scene");
        return NGL_ERROR_INVALID_USAGE;
    }
    FORWARD_TO_PARAM(node, value);
}

int ngl_node_param_set_rational(struct ngl_node *node, const char *key, int32_t num, int32_t den)
{
    FORWARD_TO_PARAM(rational, num, den);
}

int ngl_node_param_set_select(struct ngl_node *node, const char *key, const char *value)
{
    FORWARD_TO_PARAM(select, value);
}

int ngl_node_param_set_str(struct ngl_node *node, const char *key, const char *value)
{
    FORWARD_TO_PARAM(str, value);
}

int ngl_node_param_set_u32(struct ngl_node *node, const char *key, const uint32_t value)
{
    FORWARD_TO_PARAM(u32, value);
}

int ngl_node_param_set_uvec2(struct ngl_node *node, const char *key, const uint32_t *value)
{
    FORWARD_TO_PARAM(uvec2, value);
}

int ngl_node_param_set_uvec3(struct ngl_node *node, const char *key, const uint32_t *value)
{
    FORWARD_TO_PARAM(uvec3, value);
}

int ngl_node_param_set_uvec4(struct ngl_node *node, const char *key, const uint32_t *value)
{
    FORWARD_TO_PARAM(uvec4, value);
}

int ngl_node_param_set_vec2(struct ngl_node *node, const char *key, const float *value)
{
    FORWARD_TO_PARAM(vec2, value);
}

int ngl_node_param_set_vec3(struct ngl_node *node, const char *key, const float *value)
{
    FORWARD_TO_PARAM(vec3, value);
}

int ngl_node_param_set_vec4(struct ngl_node *node, const char *key, const float *value)
{
    FORWARD_TO_PARAM(vec4, value);
}

int ngl_node_param_set_dict(struct ngl_node *node, const char *key, const char *name, struct ngl_node *value)
{
    if (node->scene) {
        LOG(ERROR, "the nodes graph cannot be extended after being associated with a scene");
        return NGL_ERROR_INVALID_USAGE;
    }
    FORWARD_TO_PARAM(dict, name, value);
}

int ngl_node_get_type(struct ngl_node *node, uint32_t *type)
{
    if (!node)
        return NGL_ERROR_INVALID_ARG;

    *type = node->cls->id;

    return 0;
}

const char *ngl_node_get_type_name(const struct ngl_node *node)
{
    if (!node || !node->cls)
        return NULL;

    return node->cls->name;
}

int ngl_node_get_label(struct ngl_node *node, const char **label)
{
    if (!node)
        return NGL_ERROR_INVALID_ARG;

    *label = node->label;

    return 0;
}

static int append_child(struct ngl_node ***arrp, size_t *countp, size_t *capp, struct ngl_node *child)
{
    if (*countp >= *capp) {
        const size_t new_cap = *capp ? *capp * 2 : 8;
        struct ngl_node **tmp = ngli_realloc(*arrp, new_cap, sizeof(*tmp));
        if (!tmp)
            return NGL_ERROR_MEMORY;
        *arrp = tmp;
        *capp = new_cap;
    }
    (*arrp)[(*countp)++] = child;
    return 0;
}

int ngl_node_get_children(const struct ngl_node *node,
                          struct ngl_node ***childrenp, size_t *nb_childrenp)
{
    *childrenp = NULL;
    *nb_childrenp = 0;

    if (!node || !node->cls || !node->cls->params)
        return 0;

    struct ngl_node **children = NULL;
    size_t count = 0, capacity = 0;

    const struct node_param *par = node->cls->params;
    const uint8_t *opts = node->opts;

    while (par->key) {
        if (par->type == NGLI_PARAM_TYPE_NODE ||
            (par->flags & NGLI_PARAM_FLAG_ALLOW_NODE)) {
            struct ngl_node *child = *(struct ngl_node **)(opts + par->offset);
            if (child) {
                int ret = append_child(&children, &count, &capacity, child);
                if (ret < 0) {
                    ngli_free(children);
                    return ret;
                }
            }
        } else if (par->type == NGLI_PARAM_TYPE_NODELIST) {
            struct ngl_node **elems = *(struct ngl_node ***)(opts + par->offset);
            const size_t nb_elems = *(const size_t *)(opts + par->offset + sizeof(struct ngl_node **));
            for (size_t i = 0; i < nb_elems; i++) {
                int ret = append_child(&children, &count, &capacity, elems[i]);
                if (ret < 0) {
                    ngli_free(children);
                    return ret;
                }
            }
        }
        par++;
    }

    *childrenp = children;
    *nb_childrenp = count;
    return 0;
}

void ngl_node_children_freep(struct ngl_node ***childrenp)
{
    ngli_freep(childrenp);
}

int ngl_node_get_params_info(const struct ngl_node *node,
                             const struct ngl_param_info **infosp,
                             size_t *nb_infosp)
{
    *infosp = NULL;
    *nb_infosp = 0;

    if (!node || !node->cls || !node->cls->params)
        return 0;

    size_t count = 0;
    const struct node_param *par = node->cls->params;
    while (par[count].key)
        count++;

    if (!count)
        return 0;

    struct ngl_param_info *infos = ngli_calloc(count, sizeof(*infos));
    if (!infos)
        return NGL_ERROR_MEMORY;

    for (size_t i = 0; i < count; i++) {
        infos[i].key               = par[i].key;
        infos[i].type              = (enum ngl_param_type)par[i].type;
        infos[i].allow_live_change = NGLI_HAS_ALL_FLAGS(par[i].flags, NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE);
        infos[i].allow_node        = par[i].type == NGLI_PARAM_TYPE_NODE
                                  || NGLI_HAS_ALL_FLAGS(par[i].flags, NGLI_PARAM_FLAG_ALLOW_NODE);
        infos[i].desc              = par[i].desc;

        if (par[i].choices) {
            size_t nb_choices = 0;
            while (par[i].choices->consts[nb_choices].key)
                nb_choices++;

            const char **choice_names = ngli_calloc(nb_choices + 1, sizeof(*choice_names));
            if (!choice_names) {
                ngl_node_params_info_freep((const struct ngl_param_info **)&infos, i);
                return NGL_ERROR_MEMORY;
            }
            for (size_t j = 0; j < nb_choices; j++)
                choice_names[j] = par[i].choices->consts[j].key;
            choice_names[nb_choices] = NULL;
            infos[i].choices = choice_names;
        }
    }

    *infosp = infos;
    *nb_infosp = count;
    return 0;
}

void ngl_node_params_info_freep(const struct ngl_param_info **infosp, size_t nb_infos)
{
    if (!infosp || !*infosp)
        return;
    struct ngl_param_info *infos = (struct ngl_param_info *)*infosp;
    for (size_t i = 0; i < nb_infos; i++)
        ngli_freep(&infos[i].choices);
    ngli_freep(infosp);
}

#define FORWARD_TO_PARAM_GET(type, ...)                  \
    uint8_t *base_ptr;                                   \
    const struct node_param *par =                       \
        ngli_node_param_find(node, key, &base_ptr);      \
    if (!par)                                            \
        return NGL_ERROR_NOT_FOUND;                      \
    const uint8_t *src = base_ptr + par->offset;         \
    return ngli_params_get_##type(src, par, __VA_ARGS__)

int ngl_node_param_get_bool(struct ngl_node *node, const char *key, int *value)
{
    FORWARD_TO_PARAM_GET(bool, value);
}

int ngl_node_param_get_f32(struct ngl_node *node, const char *key, float *value)
{
    FORWARD_TO_PARAM_GET(f32, value);
}

int ngl_node_param_get_f64(struct ngl_node *node, const char *key, double *value)
{
    FORWARD_TO_PARAM_GET(f64, value);
}

int ngl_node_param_get_flags(struct ngl_node *node, const char *key, const char **value)
{
    FORWARD_TO_PARAM_GET(flags, value);
}

int ngl_node_param_get_i32(struct ngl_node *node, const char *key, int32_t *value)
{
    FORWARD_TO_PARAM_GET(i32, value);
}

int ngl_node_param_get_ivec2(struct ngl_node *node, const char *key, int32_t *value)
{
    FORWARD_TO_PARAM_GET(ivec2, value);
}

int ngl_node_param_get_ivec3(struct ngl_node *node, const char *key, int32_t *value)
{
    FORWARD_TO_PARAM_GET(ivec3, value);
}

int ngl_node_param_get_ivec4(struct ngl_node *node, const char *key, int32_t *value)
{
    FORWARD_TO_PARAM_GET(ivec4, value);
}

int ngl_node_param_get_mat4(struct ngl_node *node, const char *key, float *value)
{
    FORWARD_TO_PARAM_GET(mat4, value);
}

int ngl_node_param_get_node(struct ngl_node *node, const char *key, struct ngl_node **value)
{
    FORWARD_TO_PARAM_GET(node, value);
}

int ngl_node_param_get_rational(struct ngl_node *node, const char *key, int32_t *num, int32_t *den)
{
    FORWARD_TO_PARAM_GET(rational, num, den);
}

int ngl_node_param_get_select(struct ngl_node *node, const char *key, const char **value)
{
    FORWARD_TO_PARAM_GET(select, value);
}

int ngl_node_param_get_str(struct ngl_node *node, const char *key, const char **value)
{
    FORWARD_TO_PARAM_GET(str, value);
}

int ngl_node_param_get_u32(struct ngl_node *node, const char *key, uint32_t *value)
{
    FORWARD_TO_PARAM_GET(u32, value);
}

int ngl_node_param_get_uvec2(struct ngl_node *node, const char *key, uint32_t *value)
{
    FORWARD_TO_PARAM_GET(uvec2, value);
}

int ngl_node_param_get_uvec3(struct ngl_node *node, const char *key, uint32_t *value)
{
    FORWARD_TO_PARAM_GET(uvec3, value);
}

int ngl_node_param_get_uvec4(struct ngl_node *node, const char *key, uint32_t *value)
{
    FORWARD_TO_PARAM_GET(uvec4, value);
}

int ngl_node_param_get_vec2(struct ngl_node *node, const char *key, float *value)
{
    FORWARD_TO_PARAM_GET(vec2, value);
}

int ngl_node_param_get_vec3(struct ngl_node *node, const char *key, float *value)
{
    FORWARD_TO_PARAM_GET(vec3, value);
}

int ngl_node_param_get_vec4(struct ngl_node *node, const char *key, float *value)
{
    FORWARD_TO_PARAM_GET(vec4, value);
}

NGL_API int ngl_node_get_bounding_box(struct ngl_node *node, struct ngl_bounding_box *box)
{
    if (!node)
        return NGL_ERROR_INVALID_ARG;

    if (!has_bounding_box(node))
        return NGL_ERROR_INVALID_ARG;

    const struct ngli_node2d_info *info = node->priv_data;

    memset(box, 0, sizeof(*box));
    memcpy(box->center, info->screen_aabb.center, sizeof(box->center));
    memcpy(box->extent, info->screen_aabb.extent, sizeof(box->extent));

    return 0;
}

NGL_API int ngl_node_get_global_transform_matrix(struct ngl_node *node, float *matrix)
{
    if (!node || !matrix)
        return NGL_ERROR_INVALID_ARG;

    if (!has_bounding_box(node))
        return NGL_ERROR_INVALID_ARG;

    const struct ngli_node2d_info *info = node->priv_data;
    memcpy(matrix, info->transform_matrix.m, sizeof(info->transform_matrix.m));

    return 0;
}

NGL_API int ngl_node_get_global_position(struct ngl_node *node, float *position)
{
    if (!node || !position)
        return NGL_ERROR_INVALID_ARG;

    if (!has_bounding_box(node))
        return NGL_ERROR_INVALID_ARG;

    const struct ngli_node2d_info *info = node->priv_data;
    const float *m = info->transform_matrix.m;
    position[0] = m[12];
    position[1] = m[13];

    return 0;
}

NGL_API int ngl_node_get_global_rotation(struct ngl_node *node, float *rotation)
{
    if (!node || !rotation)
        return NGL_ERROR_INVALID_ARG;

    if (!has_bounding_box(node))
        return NGL_ERROR_INVALID_ARG;

    const struct ngli_node2d_info *info = node->priv_data;
    const float *m = info->transform_matrix.m;
    *rotation = NGLI_RAD2DEG(atan2f(m[1], m[0]));

    return 0;
}

NGL_API int ngl_node_get_global_scale(struct ngl_node *node, float *scale)
{
    if (!node || !scale)
        return NGL_ERROR_INVALID_ARG;

    if (!has_bounding_box(node))
        return NGL_ERROR_INVALID_ARG;

    const struct ngli_node2d_info *info = node->priv_data;
    const float *m = info->transform_matrix.m;
    scale[0] = sqrtf(m[0] * m[0] + m[1] * m[1]);
    scale[1] = sqrtf(m[4] * m[4] + m[5] * m[5]);

    return 0;
}

static void duplicate_hmap_free(void *user_arg, void *data)
{
    struct ngl_node *node = data;
    ngl_node_unrefp(&node);
}

static struct ngl_node *duplicate_node(struct hmap *dupmap, const struct ngl_node *node, uint32_t flags)
{
    if (!(flags & NGL_NODE_DUPLICATE_RESOURCES) && (node->cls->flags & NGLI_NODE_FLAG_SHAREABLE))
        return ngl_node_ref((struct ngl_node *)node);

    /* Check if this node was already duplicated */
    const uint64_t key = (uint64_t)(uintptr_t)node;
    struct ngl_node *dup = ngli_hmap_get_u64(dupmap, key);
    if (dup)
        return ngl_node_ref(dup);

    dup = node_create(node->cls);
    if (!dup)
        return NULL;

    int ret = ngli_hmap_set_u64(dupmap, key, dup);
    if (ret < 0)
        goto fail;

    /* Free the default label set by node_create() */
    ngli_freep(&dup->label);

    /* Copy label */
    dup->label = ngli_strdup(node->label);
    if (!dup->label)
        goto fail;

    /* Copy all parameters */
    const struct node_param *par = node->cls->params;
    if (par) {
        while (par->key) {
            const uint8_t *src_base = node->opts;
            uint8_t *dst_base = dup->opts;
            const uint8_t *srcp = src_base + par->offset;
            uint8_t *dstp = dst_base + par->offset;

            if (par->flags & NGLI_PARAM_FLAG_ALLOW_NODE) {
                /* Layout: [node_ptr][scalar_value] at offset */
                const struct ngl_node *src_node = *(const struct ngl_node *const *)srcp;
                if (src_node) {
                    struct ngl_node *dup_child = duplicate_node(dupmap, src_node, flags);
                    if (!dup_child)
                        goto fail;
                    memcpy(dstp, &dup_child, sizeof(struct ngl_node *));
                }
                /* Copy the scalar value after the node pointer */
                const size_t scalar_offset = sizeof(struct ngl_node *);
                memcpy(dstp + scalar_offset, srcp + scalar_offset,
                       ngli_params_get_type_specs(par->type)->size);
            } else {
                switch (par->type) {
                case NGLI_PARAM_TYPE_NODE: {
                    const struct ngl_node *src_child = *(const struct ngl_node *const *)srcp;
                    if (src_child) {
                        struct ngl_node *dup_child = duplicate_node(dupmap, src_child, flags);
                        if (!dup_child)
                            goto fail;
                        memcpy(dstp, &dup_child, sizeof(struct ngl_node *));
                    }
                    break;
                }
                case NGLI_PARAM_TYPE_NODELIST: {
                    struct ngl_node *const *src_elems = *(struct ngl_node *const *const *)srcp;
                    const size_t nb_elems = *(const size_t *)(srcp + sizeof(struct ngl_node **));
                    if (nb_elems) {
                        struct ngl_node **dst_elems = ngli_calloc(nb_elems, sizeof(*dst_elems));
                        if (!dst_elems)
                            goto fail;
                        for (size_t i = 0; i < nb_elems; i++) {
                            dst_elems[i] = duplicate_node(dupmap, src_elems[i], flags);
                            if (!dst_elems[i]) {
                                for (size_t j = 0; j < i; j++)
                                    ngl_node_unrefp(&dst_elems[j]);
                                ngli_free(dst_elems);
                                goto fail;
                            }
                        }
                        memcpy(dstp, &dst_elems, sizeof(struct ngl_node **));
                        memcpy(dstp + sizeof(struct ngl_node **), &nb_elems, sizeof(nb_elems));
                    }
                    break;
                }
                case NGLI_PARAM_TYPE_F64LIST: {
                    const double *src_elems = *(const double *const *)srcp;
                    const size_t nb_elems = *(const size_t *)(srcp + sizeof(double *));
                    if (nb_elems) {
                        double *dst_elems = ngli_memdup(src_elems, nb_elems * sizeof(*dst_elems));
                        if (!dst_elems)
                            goto fail;
                        memcpy(dstp, &dst_elems, sizeof(double *));
                        memcpy(dstp + sizeof(double *), &nb_elems, sizeof(nb_elems));
                    }
                    break;
                }
                case NGLI_PARAM_TYPE_NODEDICT: {
                    const struct hmap *src_hmap = *(const struct hmap *const *)srcp;
                    if (src_hmap) {
                        struct hmap *dst_hmap = ngli_hmap_create(NGLI_HMAP_TYPE_STR);
                        if (!dst_hmap)
                            goto fail;
                        ngli_hmap_set_free_func(dst_hmap, duplicate_hmap_free, NULL);
                        const struct hmap_entry *entry = NULL;
                        while ((entry = ngli_hmap_next(src_hmap, entry))) {
                            struct ngl_node *dup_child = duplicate_node(dupmap, entry->data, flags);
                            if (!dup_child) {
                                ngli_hmap_freep(&dst_hmap);
                                goto fail;
                            }
                            ret = ngli_hmap_set_str(dst_hmap, entry->key.str, dup_child);
                            if (ret < 0) {
                                ngl_node_unrefp(&dup_child);
                                ngli_hmap_freep(&dst_hmap);
                                goto fail;
                            }
                        }
                        memcpy(dstp, &dst_hmap, sizeof(struct hmap *));
                    }
                    break;
                }
                case NGLI_PARAM_TYPE_STR: {
                    const char *src_str = *(const char *const *)srcp;
                    if (src_str) {
                        char *dst_str = ngli_strdup(src_str);
                        if (!dst_str)
                            goto fail;
                        memcpy(dstp, &dst_str, sizeof(char *));
                    }
                    break;
                }
                case NGLI_PARAM_TYPE_DATA: {
                    const uint8_t *src_data = *(const uint8_t *const *)srcp;
                    const size_t size = *(const size_t *)(srcp + sizeof(void *));
                    if (src_data && size) {
                        uint8_t *dst_data = ngli_memdup(src_data, size);
                        if (!dst_data)
                            goto fail;
                        memcpy(dstp, &dst_data, sizeof(uint8_t *));
                        memcpy(dstp + sizeof(void *), &size, sizeof(size));
                    }
                    break;
                }
                default:
                    /* Scalar types: direct memory copy */
                    memcpy(dstp, srcp, ngli_params_get_type_specs(par->type)->size);
                    break;
                }
            }
            par++;
        }
    }

    return dup;

fail:
    ngl_node_unrefp(&dup);
    return NULL;
}

struct ngl_node *ngl_node_duplicate(struct ngl_node *node, uint32_t flags)
{
    if (!node)
        return NULL;

    struct hmap *dupmap = ngli_hmap_create(NGLI_HMAP_TYPE_U64);
    if (!dupmap)
        return NULL;

    struct ngl_node *dup = duplicate_node(dupmap, node, flags);
    ngli_hmap_freep(&dupmap);
    return dup;
}

struct ngl_node *ngl_node_ref(struct ngl_node *node)
{
    node->refcount++;
    return node;
}

void ngl_node_unrefp(struct ngl_node **nodep)
{
    struct ngl_node *node = *nodep;

    if (!node)
        return;
    const int delete = node->refcount-- == 1;
    if (delete) {
        LOG(VERBOSE, "DELETE %s @ %p", node->label, node);
        ngli_assert(!node->ctx);
        if (node->cls->free)
            node->cls->free(node);
        ngli_params_free((uint8_t *)node, ngli_base_node_params);
        ngli_params_free(node->opts, node->cls->params);
        ngli_free_aligned(node);
    }
    *nodep = NULL;
}
