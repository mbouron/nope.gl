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
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "internal.h"
#include "node_graph.h"
#include "node_graph_edit.h"
#include "node_texture.h"
#include "params.h"
#include "utils/hmap.h"
#include "resource.h"
#include "utils/memory.h"

static struct ngl_scene *create_scene(struct ngl_node *root)
{
    struct ngl_scene *scene = ngl_scene_create();
    ngli_assert(scene);
    const struct ngl_scene_params params = ngl_scene_default_params(root);
    ngli_assert(ngl_scene_init(scene, &params) == 0);
    return scene;
}

static void test_scene_cycle(void)
{
    struct ngl_node *root = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *child = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_scene *scene = ngl_scene_create();
    ngli_assert(root && child && scene);
    ngli_assert(ngl_node_param_add_nodes(root, "children", 1, &child) == 0);
    ngli_assert(ngl_node_param_add_nodes(child, "children", 1, &root) == 0);

    const struct ngl_scene_params params = ngl_scene_default_params(root);
    for (size_t i = 0; i < 2; i++) {
        ngli_assert(ngl_scene_init(scene, &params) == NGL_ERROR_INVALID_ARG);
        ngli_assert(!scene->params.root && !scene->nodes.count);
        ngli_assert(!root->scene && !child->scene);
    }

    /* A failed traversal must not prevent attaching the repaired graph. */
    ngli_assert(ngl_node_param_remove_nodes(child, "children", 1, &root) == 0);
    ngli_assert(ngl_scene_init(scene, &params) == 0);
    ngli_assert(scene->nodes.count == 2);

    ngl_scene_unrefp(&scene);
    ngl_node_unrefp(&child);
    ngl_node_unrefp(&root);
}

static void test_scene_sharing(void)
{
    const uint32_t leaf_types[] = {NGL_NODE_IDENTITY, NGL_NODE_GROUP};
    const int expected_results[] = {0, NGL_ERROR_INVALID_USAGE};
    for (size_t i = 0; i < NGLI_ARRAY_NB(leaf_types); i++) {
        struct ngl_node *root = ngl_node_create(NGL_NODE_GROUP);
        struct ngl_node *a = ngl_node_create(NGL_NODE_GROUP);
        struct ngl_node *b = ngl_node_create(NGL_NODE_GROUP);
        struct ngl_node *leaf = ngl_node_create(leaf_types[i]);
        struct ngl_scene *scene = ngl_scene_create();
        ngli_assert(root && a && b && leaf && scene);
        ngli_assert(ngl_node_param_add_nodes(a, "children", 1, &leaf) == 0);
        ngli_assert(ngl_node_param_add_nodes(b, "children", 1, &leaf) == 0);
        struct ngl_node *children[] = {a, b};
        ngli_assert(ngl_node_param_add_nodes(root, "children", 2, children) == 0);

        /* Sharing is checked across separate sub-trees of one operation too. */
        const struct ngli_scene_subtree_check_ctx check_ctx = {
            .visiting_id = ngli_node_graph_new_traversal_id(),
            .visited_id = ngli_node_graph_new_traversal_id(),
        };
        ngli_assert(ngli_scene_check_subtree(NULL, &check_ctx, a) == 0);
        ngli_assert(ngli_scene_check_subtree(NULL, &check_ctx, b) == expected_results[i]);

        const struct ngl_scene_params params = ngl_scene_default_params(root);
        ngli_assert(ngl_scene_init(scene, &params) == expected_results[i]);
        if (expected_results[i] < 0) {
            ngli_assert(!scene->params.root && !scene->nodes.count);
            ngli_assert(!root->scene && !a->scene && !b->scene && !leaf->scene);
            ngli_assert(ngl_node_param_remove_nodes(b, "children", 1, &leaf) == 0);
            ngli_assert(ngl_scene_init(scene, &params) == 0);
        }
        ngli_assert(scene->nodes.count == 4);

        ngl_scene_unrefp(&scene);
        ngl_node_unrefp(&leaf);
        ngl_node_unrefp(&b);
        ngl_node_unrefp(&a);
        ngl_node_unrefp(&root);
    }
}

struct customtexture_lifecycle {
    struct ngl_node *node;
    int step;
};

static int customtexture_init(void *reserved, void *user_data)
{
    struct customtexture_lifecycle *s = user_data;
    ngli_assert(s->step++ == 0);
    return 0;
}

static int customtexture_prepare(void *reserved, void *user_data)
{
    struct customtexture_lifecycle *s = user_data;
    ngli_assert(!s->node->prepared);
    ngli_assert(s->step++ == 1);
    return 0;
}

static void customtexture_unprepare(void *reserved, void *user_data)
{
    struct customtexture_lifecycle *s = user_data;
    ngli_assert(!s->node->prepared);
    ngli_assert(s->step++ == 2);
}

static void customtexture_uninit(void *reserved, void *user_data)
{
    struct customtexture_lifecycle *s = user_data;
    ngli_assert(s->step++ == 3);
}

static void test_customtexture_lifecycle(void)
{
    struct customtexture_lifecycle lifecycle = {0};
    struct ngl_node_funcs funcs = {
        .init = customtexture_init,
        .prepare = customtexture_prepare,
        .unprepare = customtexture_unprepare,
        .uninit = customtexture_uninit,
    };
    struct ngl_node *node = ngl_node_create(NGL_NODE_CUSTOMTEXTURE);
    ngli_assert(node);
    lifecycle.node = node;
    ngli_assert(ngl_node_set_funcs(node, &lifecycle, &funcs) == 0);

    struct ngl_ctx ctx = {0};
    ngli_assert(ngli_node_attach_ctx(node, &ctx) == 0);
    ngli_assert(lifecycle.step == 2);
    ngli_assert(node->prepared);

    ngli_ctx_release_resources(&ctx);
    ngli_assert(lifecycle.step == 4);

    ngli_darray_reset(&ctx.resource_nodes);
    ngl_node_unrefp(&node);
}

struct resource_retry {
    int init_count;
    int prepare_count;
    int unprepare_count;
    int uninit_count;
    void *allocation;
    bool fail;
};

static int retry_init(void *reserved, void *user_data)
{
    struct resource_retry *s = user_data;
    ngli_assert(!s->allocation);
    s->init_count++;
    return 0;
}

static int retry_prepare(void *reserved, void *user_data)
{
    struct resource_retry *s = user_data;
    ngli_assert(!s->allocation);
    s->allocation = ngli_malloc(1);
    s->prepare_count++;
    return s->fail ? NGL_ERROR_MEMORY : 0;
}

static void retry_unprepare(void *reserved, void *user_data)
{
    struct resource_retry *s = user_data;
    ngli_freep(&s->allocation);
    s->unprepare_count++;
}

static void retry_uninit(void *reserved, void *user_data)
{
    struct resource_retry *s = user_data;
    ngli_assert(!s->allocation);
    s->uninit_count++;
}

static int consumer_init(struct ngl_node *node)
{
    struct image_resource **resource = node->priv_data;
    const struct texture_info *info = node->children.data[0]->priv_data;
    *resource = ngli_image_resource_ref(info->resource);
    return 0;
}

static int consumer_prepare(struct ngl_node *node, const struct ngpu_rendertarget_layout *layout)
{
    ngli_assert(!node->prepared && node->children.data[0]->prepared);
    struct image_resource **resource = node->priv_data;
    const struct texture_info *info = node->children.data[0]->priv_data;
    ngli_assert(*resource == info->resource);
    return 0;
}

static void consumer_uninit(struct ngl_node *node)
{
    ngli_image_resource_unrefp(node->priv_data);
}

static void test_resources_retry(void)
{
    struct resource_retry retry = {.fail = true};
    struct ngl_node_funcs funcs = {
        .init = retry_init,
        .prepare = retry_prepare,
        .unprepare = retry_unprepare,
        .uninit = retry_uninit,
    };
    struct ngl_node *texture = ngl_node_create(NGL_NODE_CUSTOMTEXTURE);
    ngli_assert(texture);
    ngli_assert(ngl_node_set_funcs(texture, &retry, &funcs) == 0);

    /* Both consumers borrow the holder during init, before allocation fails.
     * The second has initialized but is not reached before preparation fails. */
    const struct node_class consumer_class = {
        .init = consumer_init,
        .prepare = consumer_prepare,
        .uninit = consumer_uninit,
        .priv_size = sizeof(struct image_resource *),
    };
    struct image_resource *resources[2] = {0};
    struct ngl_node consumers[2] = {0};
    for (size_t i = 0; i < 2; i++) {
        consumers[i].cls = &consumer_class;
        consumers[i].label = "consumer";
        consumers[i].priv_data = &resources[i];
        consumers[i].refcount = 1;
        ngli_darray_push(&consumers[i].children, texture);
    }

    struct ngl_ctx ctx = {0};
    struct ngl_node *roots[] = {&consumers[0], &consumers[1]};
    for (size_t attempt = 0; attempt < 2; attempt++) {
        for (size_t i = 0; i < 2; i++)
            ngli_assert(ngli_node_set_ctx(&consumers[i], &ctx) == 0);
        ngli_assert(ngli_node_prepare_nodes(&ctx, NGLI_ARRAY_NB(roots), roots,
                                            &ctx.default_rendertarget_layout) == NGL_ERROR_MEMORY);
        ngli_assert(retry.allocation == NULL && retry.unprepare_count == attempt + 1);
        ngli_assert(retry.init_count == 1 && retry.uninit_count == 0);
        ngli_assert(texture->ctx == &ctx && ctx.resource_nodes.count == 3);
        for (size_t i = 0; i < NGLI_ARRAY_NB(resources); i++) {
            const struct texture_info *info = (struct texture_info *)texture->priv_data;
            ngli_assert(consumers[i].ctx == &ctx);
            ngli_assert(resources[i] == info->resource);
        }
    }

    retry.fail = false;
    for (size_t i = 0; i < 2; i++)
        ngli_assert(ngli_node_set_ctx(&consumers[i], &ctx) == 0);
    ngli_assert(ngli_node_prepare_nodes(&ctx, NGLI_ARRAY_NB(roots), roots,
                                        &ctx.default_rendertarget_layout) == 0);
    ngli_ctx_release_resources(&ctx);
    ngli_assert(retry.allocation == NULL);
    ngli_assert(retry.init_count == 1 && retry.prepare_count == 3);
    ngli_assert(retry.unprepare_count == 3 && retry.uninit_count == 1);

    for (size_t i = 0; i < 2; i++)
        ngli_darray_reset(&consumers[i].children);
    ngli_darray_reset(&ctx.resource_nodes);
    ngl_node_unrefp(&texture);
}

static void test_add_edges_rollback(void)
{
    struct ngl_node *root = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *a = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *b = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *c = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *shared = ngl_node_create(NGL_NODE_UNIFORMFLOAT);
    struct ngl_node *foreign = ngl_node_create(NGL_NODE_GROUP);
    ngli_assert(root && a && b && c && shared && foreign);

    struct ngl_node *children[] = {a, shared, b, shared};
    ngli_assert(ngl_node_param_add_nodes(root, "children", NGLI_ARRAY_NB(children), children) == 0);

    struct ngl_scene *scene = create_scene(root);
    struct ngl_scene *foreign_scene = create_scene(foreign);

    struct ngl_node *added_children[] = {shared, c, shared, foreign};
    const int ret = ngli_scene_add_edges(root, 1, NGLI_ARRAY_NB(added_children), added_children);
    ngli_assert(ret == NGL_ERROR_INVALID_USAGE);

    ngli_assert(root->children.count == NGLI_ARRAY_NB(children));
    for (size_t i = 0; i < NGLI_ARRAY_NB(children); i++)
        ngli_assert(root->children.data[i] == children[i]);
    ngli_assert(a->parents.count == 1 && a->parents.data[0] == root);
    ngli_assert(b->parents.count == 1 && b->parents.data[0] == root);
    ngli_assert(shared->parents.count == 2);
    ngli_assert(shared->parents.data[0] == root && shared->parents.data[1] == root);
    ngli_assert(c->scene == NULL);
    ngli_assert(c->parents.count == 0);
    ngli_assert(scene->nodes.count == 4);
    ngli_assert(ngli_node_darray_find(&scene->nodes, c) == SIZE_MAX);
    ngli_assert(foreign->scene == foreign_scene);

    ngl_scene_unrefp(&foreign_scene);
    ngl_scene_unrefp(&scene);
    ngl_node_unrefp(&foreign);
    ngl_node_unrefp(&shared);
    ngl_node_unrefp(&c);
    ngl_node_unrefp(&b);
    ngl_node_unrefp(&a);
    ngl_node_unrefp(&root);
}

static void check_duplicate_release(struct ngl_node *root, struct ngl_scene *scene)
{
    struct ngl_node **children = NULL;
    size_t count = 0;
    ngli_assert(ngl_node_get_children(root, &children, &count) == 0 && count == 2);
    struct ngl_node *branch = ngl_node_ref(children[0]);
    struct ngl_node *resource = ngl_node_ref(children[1]);
    const int resource_refs = resource->refcount;
    ngl_node_children_freep(&children);
    ngli_assert(ngl_node_get_children(branch, &children, &count) == 0 && count == 1);
    struct ngl_node *leaf = ngl_node_ref(children[0]);
    const int leaf_refs = leaf->refcount;
    ngl_node_children_freep(&children);

    if (scene)
        ngl_scene_unrefp(&scene);
    else
        ngl_node_unrefp(&root);
    ngli_assert(branch->refcount == 1);
    ngli_assert(resource->refcount == resource_refs - 1);
    ngl_node_unrefp(&branch);
    ngli_assert(leaf->refcount == leaf_refs - 1);
    ngl_node_unrefp(&leaf);
    ngl_node_unrefp(&resource);
}

static void test_duplicate_release(void)
{
    struct ngl_node *leaf = ngl_node_create(NGL_NODE_IDENTITY);
    struct ngl_node *branch = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *resource = ngl_node_create(NGL_NODE_UNIFORMFLOAT);
    struct ngl_node *root = ngl_node_create(NGL_NODE_GROUP);
    ngli_assert(leaf && branch && resource && root);
    ngli_assert(ngl_node_param_add_nodes(branch, "children", 1, &leaf) == 0);
    struct ngl_node *children[] = {branch, resource};
    ngli_assert(ngl_node_param_add_nodes(root, "children", 2, children) == 0);
    for (size_t i = 0; i < 2; i++) {
        struct ngl_node *dup = ngl_node_duplicate(root, i ? NGL_NODE_DUPLICATE_RESOURCES : 0);
        ngli_assert(dup);
        check_duplicate_release(dup, NULL);
    }
    struct ngl_scene *scene = create_scene(root);
    struct ngl_scene *dup_scene = ngl_scene_duplicate(scene);
    ngli_assert(dup_scene);
    check_duplicate_release(dup_scene->params.root, dup_scene);
    ngl_scene_unrefp(&scene);
    ngl_node_unrefp(&root);
    ngl_node_unrefp(&resource);
    ngl_node_unrefp(&branch);
    ngl_node_unrefp(&leaf);
}

static void test_swap_bounds(void)
{
    struct ngl_node *group = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *a = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *b = ngl_node_create(NGL_NODE_GROUP);
    ngli_assert(group && a && b);

    struct ngl_node *children[] = {a, b};
    ngli_assert(ngl_node_param_add_nodes(group, "children", 2, children) == 0);
    ngli_assert(ngl_node_param_swap_elem(group, "children", 0, 2) == NGL_ERROR_INVALID_ARG);
    ngli_assert(ngl_node_param_swap_elem(group, "children", 2, 0) == NGL_ERROR_INVALID_ARG);

    struct ngl_node *keyframe = ngl_node_create(NGL_NODE_ANIMKEYFRAMEFLOAT);
    ngli_assert(keyframe);
    double easing_args[] = {1.0, 2.0};
    ngli_assert(ngl_node_param_add_f64s(keyframe, "easing_args", 2, easing_args) == 0);
    ngli_assert(ngl_node_param_swap_elem(keyframe, "easing_args", 0, 2) == NGL_ERROR_INVALID_ARG);
    ngli_assert(ngl_node_param_swap_elem(keyframe, "easing_args", 2, 0) == NGL_ERROR_INVALID_ARG);

    ngl_node_unrefp(&keyframe);
    ngl_node_unrefp(&b);
    ngl_node_unrefp(&a);
    ngl_node_unrefp(&group);
}

static void test_live_swap_runtime_edges(void)
{
    struct ngl_node *root = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *a = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *resource = ngl_node_create(NGL_NODE_UNIFORMFLOAT);
    struct ngl_node *b = ngl_node_create(NGL_NODE_GROUP);
    ngli_assert(root && a && resource && b);

    struct ngl_node *children[] = {a, resource, b};
    ngli_assert(ngl_node_param_add_nodes(root, "children", 3, children) == 0);
    struct ngl_scene *scene = create_scene(root);

    ngli_assert(root->children.data[0] == a);
    ngli_assert(root->children.data[1] == resource);
    ngli_assert(root->children.data[2] == b);

    ngli_assert(ngl_node_param_swap_elem(root, "children", 0, 2) == 0);
    ngli_assert(root->children.data[0] == b);
    ngli_assert(root->children.data[1] == resource);
    ngli_assert(root->children.data[2] == a);

    ngl_scene_unrefp(&scene);
    ngl_node_unrefp(&b);
    ngl_node_unrefp(&resource);
    ngl_node_unrefp(&a);
    ngl_node_unrefp(&root);
}

static void test_live_children_duplicates(void)
{
    struct ngl_node *root = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *resource = ngl_node_create(NGL_NODE_UNIFORMFLOAT);
    struct ngl_scene *scene = ngl_scene_create();
    ngli_assert(root && resource && scene);
    ngli_assert(ngl_node_is_shareable(resource));
    ngli_assert(!ngl_node_is_shareable(root));

    struct ngl_node *children[] = {resource, resource};
    ngli_assert(ngl_node_param_add_nodes(root, "children", 2, children) == 0);
    const struct ngl_scene_params params = ngl_scene_default_params(root);
    ngli_assert(ngl_scene_init(scene, &params) == 0);
    ngli_assert(root->children.count == 2 && resource->parents.count == 2);

    ngli_assert(ngl_node_param_add_nodes(root, "children", 2, children) == 0);
    ngli_assert(root->children.count == 4 && resource->parents.count == 4);
    ngli_assert(resource->refcount == 5);
    ngli_assert(ngl_node_param_remove_nodes(root, "children", 1, &resource) == 0);
    ngli_assert(root->children.count == 0 && resource->parents.count == 0);
    ngli_assert(resource->refcount == 1 && !resource->scene);

    ngli_assert(ngl_node_param_add_nodes(root, "children", 2, children) == 0);
    ngli_assert(root->children.count == 2 && resource->parents.count == 2);

    struct ngl_node *branch = ngl_node_create(NGL_NODE_GROUP);
    ngli_assert(branch);
    struct ngl_node *repeated[] = {branch, branch};
    ngli_assert(ngl_node_param_add_nodes(root, "children", 2, repeated) == NGL_ERROR_INVALID_USAGE);
    ngli_assert(!branch->scene && root->children.count == 2);
    ngli_assert(ngl_node_param_add_nodes(root, "children", 1, &branch) == 0);
    ngli_assert(ngl_node_param_add_nodes(root, "children", 1, &branch) == NGL_ERROR_INVALID_USAGE);
    ngli_assert(root->children.count == 3 && branch->parents.count == 1);

    ngl_scene_unrefp(&scene);
    ngl_node_unrefp(&branch);
    ngl_node_unrefp(&resource);
    ngl_node_unrefp(&root);
}

static void test_release_detached_without_resources(void)
{
    ngli_assert(ngl_node_release_detached_resources(NULL) == NGL_ERROR_INVALID_ARG);
    struct ngl_node *root = ngl_node_create(NGL_NODE_GROUP);
    ngli_assert(root);
    ngli_assert(ngl_node_release_detached_resources(root) == 0);
    struct ngl_scene *scene = create_scene(root);
    ngli_assert(ngl_node_release_detached_resources(root) == NGL_ERROR_INVALID_USAGE);
    ngl_scene_unrefp(&scene);

    /* Detached graphs may contain cycles while being constructed. */
    ngli_assert(ngl_node_param_add_nodes(root, "children", 1, &root) == 0);
    ngli_assert(ngl_node_release_detached_resources(root) == 0);
    ngli_assert(ngl_node_param_remove_nodes(root, "children", 1, &root) == 0);
    ngl_node_unrefp(&root);
}

struct multi_list_opts {
    struct ngl_node *prefix;
    struct ngl_node *value_node;
    float value;
    struct ngli_node_darray first;
    struct hmap *resources;
    struct ngli_node_darray second;
    int update_count;
    int invalidate_count;
    int update_ret;
};

#define OFFSET(x) offsetof(struct multi_list_opts, x)
static const struct node_param multi_list_params[] = {
    {"prefix",    NGLI_PARAM_TYPE_NODE,     OFFSET(prefix)},
    {"value",     NGLI_PARAM_TYPE_F32,      OFFSET(value_node), .flags=NGLI_PARAM_FLAG_ALLOW_NODE},
    {"first",     NGLI_PARAM_TYPE_NODELIST, OFFSET(first),      .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE},
    {"resources", NGLI_PARAM_TYPE_NODEDICT, OFFSET(resources)},
    {"second",    NGLI_PARAM_TYPE_NODELIST, OFFSET(second),     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE},
    {NULL},
};
#undef OFFSET

static const struct node_class multi_list_class = {
    .name = "MultiList",
    .params = multi_list_params,
};

static void test_graph_find(void)
{
    struct ngl_node *root = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *mid = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *leaf = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *other = ngl_node_create(NGL_NODE_GROUP);
    ngli_assert(root && mid && leaf && other);
    ngli_assert(ngl_node_param_add_nodes(root, "children", 1, &mid) == 0);
    ngli_assert(ngl_node_param_add_nodes(mid, "children", 1, &leaf) == 0);

    ngli_assert(ngli_node_graph_find_node(root, root));
    ngli_assert(ngli_node_graph_find_node(root, leaf));
    ngli_assert(!ngli_node_graph_find_node(leaf, root));
    ngli_assert(!ngli_node_graph_find_node(root, other));

    /* The search terminates on a graph that already contains a cycle */
    ngli_assert(ngl_node_param_add_nodes(leaf, "children", 1, &root) == 0);
    ngli_assert(ngli_node_graph_find_node(leaf, mid));
    ngli_assert(!ngli_node_graph_find_node(mid, other));
    ngli_assert(ngl_node_param_remove_nodes(leaf, "children", 1, &root) == 0);

    ngl_node_unrefp(&other);
    ngl_node_unrefp(&leaf);
    ngl_node_unrefp(&mid);
    ngl_node_unrefp(&root);
}

static void test_children_range(void)
{
    struct ngl_node child = {0};
    struct ngl_node *first[] = {&child, &child};
    struct multi_list_opts opts = {
        .prefix = &child,
        .first = {.data = first, .count = NGLI_ARRAY_NB(first)},
    };
    struct ngl_node node = {.cls = &multi_list_class, .opts = &opts};
    struct ngli_node_graph_range range = ngli_node_graph_get_param_range(&node, &multi_list_params[2]);
    ngli_assert(range.index == 1 && range.count == 2);
    range = ngli_node_graph_get_param_range(&node, &multi_list_params[4]);
    ngli_assert(range.index == 3 && range.count == 0);

    opts.value_node = &child;
    opts.resources = ngli_hmap_create(NGLI_HMAP_TYPE_STR);
    ngli_hmap_set_str(opts.resources, "a", &child);
    ngli_hmap_set_str(opts.resources, "b", &child);
    range = ngli_node_graph_get_param_range(&node, &multi_list_params[2]);
    ngli_assert(range.index == 2 && range.count == 2);
    range = ngli_node_graph_get_param_range(&node, &multi_list_params[3]);
    ngli_assert(range.index == 4 && range.count == 2);
    range = ngli_node_graph_get_param_range(&node, &multi_list_params[4]);
    ngli_assert(range.index == 6 && range.count == 0);
    opts.first.count = 0;
    range = ngli_node_graph_get_param_range(&node, &multi_list_params[4]);
    ngli_assert(range.index == 4 && range.count == 0);
    ngli_hmap_freep(&opts.resources);
}

static void init_multi_list(struct ngl_node *node, struct multi_list_opts *opts)
{
    *opts = (struct multi_list_opts){0};
    *node = (struct ngl_node){
        .cls = &multi_list_class,
        .opts = opts,
        .label = "multi-list",
        .refcount = 1,
        .visit_time = -1.,
        .last_update_time = -1.,
    };
    ngli_params_init(node->opts, node->cls->params);
}

static void check_multi_list_edges(struct ngl_node *node)
{
    struct ngl_node **expected = NULL;
    size_t count = 0;
    ngli_assert(ngl_node_get_children(node, &expected, &count) == 0);
    ngli_assert(node->children.count == count);
    for (size_t i = 0; i < count; i++) {
        struct ngl_node *child = expected[i];
        ngli_assert(node->children.data[i] == child && child->scene == node->scene);
        size_t refs = 0, edges = 0;
        for (size_t j = 0; j < count; j++)
            edges += expected[j] == child;
        for (size_t j = 0; j < child->parents.count; j++)
            refs += child->parents.data[j] == node;
        ngli_assert(refs == edges);
    }
    ngl_node_children_freep(&expected);
}

static void test_live_multiple_lists(void)
{
    struct multi_list_opts opts;
    struct ngl_node root;
    init_multi_list(&root, &opts);
    struct ngl_node *shared = ngl_node_create(NGL_NODE_UNIFORMFLOAT);
    struct ngl_node *a = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *b = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *c = ngl_node_create(NGL_NODE_GROUP);
    ngli_assert(shared && a && b && c);
    ngli_assert(ngl_node_param_set_node(&root, "prefix", shared) == 0);
    ngli_assert(ngl_node_param_set_node(&root, "value", shared) == 0);
    ngli_assert(ngl_node_param_set_dict(&root, "resources", "shared", shared) == 0);
    struct ngl_scene *scene = create_scene(&root);
    check_multi_list_edges(&root);

    struct ngl_node *first[] = {a, shared};
    struct ngl_node *second[] = {shared, b};
    ngli_assert(ngl_node_param_add_nodes(&root, "second", 2, second) == 0);
    ngli_assert(ngl_node_param_add_nodes(&root, "first", 2, first) == 0);
    check_multi_list_edges(&root);
    ngli_assert(ngl_node_param_add_nodes(&root, "second", 1, &c) == 0);
    ngli_assert(ngl_node_param_swap_elem(&root, "second", 0, 2) == 0);
    check_multi_list_edges(&root);
    ngli_assert(opts.second.data[0] == c && opts.second.data[1] == b && opts.second.data[2] == shared);

    ngli_assert(ngl_node_param_add_nodes(&root, "second", 1, &a) == NGL_ERROR_INVALID_USAGE);
    ngli_assert(ngl_node_param_remove_nodes(&root, "second", 1, &a) == NGL_ERROR_INVALID_ARG);
    ngli_assert(ngl_node_param_swap_elem(&root, "second", 0, 3) == NGL_ERROR_INVALID_ARG);
    ngli_assert(ngl_node_param_add_nodes(&root, "second", 1, &shared) == 0);
    ngli_assert(opts.second.count == 4 && opts.second.data[3] == shared);
    check_multi_list_edges(&root);

    ngli_assert(ngl_node_param_remove_nodes(&root, "second", 1, &shared) == 0);
    ngli_assert(opts.first.data[1] == shared && shared->parents.count == 4);
    check_multi_list_edges(&root);
    ngli_assert(ngl_node_param_remove_nodes(&root, "first", 2, first) == 0);
    check_multi_list_edges(&root);
    ngli_assert(!a->scene && shared->scene == scene);
    ngli_assert(ngl_node_param_remove_nodes(&root, "second", 1, &b) == 0);
    ngli_assert(ngl_node_param_remove_nodes(&root, "second", 1, &c) == 0);
    check_multi_list_edges(&root);

    ngl_scene_unrefp(&scene);
    ngli_params_free(root.opts, root.cls->params);
    ngl_node_unrefp(&shared);
    ngl_node_unrefp(&a);
    ngl_node_unrefp(&b);
    ngl_node_unrefp(&c);
}

static int test_dispatch(struct ngl_ctx *ctx, int (*fn)(struct ngl_ctx *, void *), void *arg)
{
    return fn(ctx, arg);
}

static int list_update(struct ngl_node *node)
{
    struct multi_list_opts *opts = node->opts;
    opts->update_count++;
    return opts->update_ret;
}

static void list_invalidate(struct ngl_node *node)
{
    struct multi_list_opts *opts = node->opts;
    opts->invalidate_count++;
}

static void check_list_wrong_type(struct ngl_node *node)
{
    ngli_assert(ngl_node_param_add_nodes(node, "label", 0, NULL) == NGL_ERROR_INVALID_ARG);
    ngli_assert(ngl_node_param_remove_nodes(node, "label", 0, NULL) == NGL_ERROR_INVALID_ARG);
    ngli_assert(ngl_node_param_add_f64s(node, "label", 0, NULL) == NGL_ERROR_INVALID_ARG);
    ngli_assert(ngl_node_param_swap_elem(node, "label", 0, 0) == NGL_ERROR_INVALID_ARG);
}

static void test_list_edit_states(void)
{
    /* Construction, scene only, attached, and detached with retained resources. */
    for (int state = 0; state < 4; state++) {
        const struct api_impl api = {.dispatch = test_dispatch};
        struct ngl_ctx ctx = {.api_impl = &api};
        struct multi_list_opts opts;
        struct ngl_node root;
        init_multi_list(&root, &opts);
        struct node_param params[NGLI_ARRAY_NB(multi_list_params)];
        memcpy(params, multi_list_params, sizeof(params));
        params[4].update_func = list_update;
        const struct node_class cls = {
            .name = "ListNotifications",
            .params = params,
            .invalidate = list_invalidate,
        };
        root.cls = &cls;
        struct ngl_node *a = ngl_node_create(NGL_NODE_UNIFORMFLOAT);
        struct ngl_node *b = ngl_node_create(NGL_NODE_GROUP);
        struct ngl_node *holder = ngl_node_create(NGL_NODE_GROUP);
        struct ngl_node *rootp = &root;
        ngli_assert(a && b && holder);
        ngli_assert(ngl_node_param_add_nodes(&root, "first", 1, &a) == 0);
        ngli_assert(ngl_node_param_add_nodes(holder, "children", 1, &rootp) == 0);
        struct ngl_scene *scene = state ? create_scene(holder) : NULL;
        if (state >= 2)
            ngli_assert(ngli_node_attach_ctx(holder, &ctx) == 0);
        if (state == 3)
            ngli_assert(ngl_node_param_remove_nodes(holder, "children", 1, &rootp) == 0);
        opts.update_count = opts.invalidate_count = 0;
        check_list_wrong_type(&root);

        struct ngl_node *added[] = {a, b, a};
        ngli_assert(ngl_node_param_add_nodes(&root, "second", 3, added) == 0);
        ngli_assert(opts.second.count == 3);
        ngli_assert(ngl_node_param_swap_elem(&root, "second", 0, 1) == 0);
        ngli_assert(opts.second.data[0] == b && opts.second.data[1] == a);
        ngli_assert(ngl_node_param_remove_nodes(&root, "second", 2, (struct ngl_node *[]){a, a}) == NGL_ERROR_INVALID_ARG);
        ngli_assert(opts.second.count == 3);
        ngli_assert(ngl_node_param_remove_nodes(&root, "second", 1, &a) == 0);
        ngli_assert(opts.second.count == 1 && opts.second.data[0] == b);
        ngli_assert(opts.update_count == (state >= 2 ? 3 : 0));
        ngli_assert(opts.invalidate_count == (state >= 2 ? 3 : 0));
        if (root.scene) {
            check_multi_list_edges(&root);
        } else {
            ngli_assert(root.children.count == 0 && b->parents.count == 0);
            ngli_assert(!b->ctx && !b->scene);
        }

        /* Empty edits and same-index swaps do not notify; bounds still apply. */
        ngli_assert(ngl_node_param_add_nodes(&root, "second", 0, NULL) == 0);
        ngli_assert(ngl_node_param_remove_nodes(&root, "second", 0, NULL) == 0);
        ngli_assert(ngl_node_param_swap_elem(&root, "second", 0, 0) == 0);
        ngli_assert(ngl_node_param_swap_elem(&root, "second", 1, 1) == NGL_ERROR_INVALID_ARG);
        ngli_assert(opts.update_count == (state >= 2 ? 3 : 0));
        ngli_assert(opts.invalidate_count == (state >= 2 ? 3 : 0));

        if (state >= 2) {
            /* Notification failures keep the edit, without invalidation. */
            opts.update_ret = NGL_ERROR_MEMORY;
            ngli_assert(ngl_node_param_add_nodes(&root, "second", 1, &a) == NGL_ERROR_MEMORY);
            ngli_assert(opts.second.count == 2 && opts.second.data[1] == a);
            ngli_assert(ngl_node_param_swap_elem(&root, "second", 0, 1) == NGL_ERROR_MEMORY);
            ngli_assert(opts.second.data[0] == a && opts.second.data[1] == b);
            ngli_assert(ngl_node_param_remove_nodes(&root, "second", 1, &a) == NGL_ERROR_MEMORY);
            ngli_assert(opts.second.count == 1 && opts.second.data[0] == b);
            ngli_assert(opts.update_count == 6 && opts.invalidate_count == 3);
            if (root.scene)
                check_multi_list_edges(&root);
        }

        ngli_ctx_release_resources(&ctx);
        ngli_darray_reset(&ctx.resource_nodes);
        ngl_scene_unrefp(&scene);
        ngl_node_unrefp(&holder);
        ngli_params_free(root.opts, root.cls->params);
        ngl_node_unrefp(&a);
        ngl_node_unrefp(&b);
    }
}

static void test_list_edit_permissions(void)
{
    struct multi_list_opts opts;
    struct ngl_node root;
    init_multi_list(&root, &opts);
    struct node_param params[NGLI_ARRAY_NB(multi_list_params)];
    memcpy(params, multi_list_params, sizeof(params));
    params[2].flags = 0;
    const struct node_class cls = {.name = "NonLiveList", .params = params};
    root.cls = &cls;
    struct ngl_node *a = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *b = ngl_node_create(NGL_NODE_GROUP);
    ngli_assert(a && b);
    /* A parameter of the wrong type is an argument error in any state */
    ngli_assert(ngl_node_param_add_nodes(&root, "missing", 0, NULL) == NGL_ERROR_NOT_FOUND);
    check_list_wrong_type(&root);
    ngli_assert(ngl_node_param_add_nodes(&root, "first", 2, (struct ngl_node *[]){a, b}) == 0);
    ngli_assert(ngl_node_param_remove_nodes(&root, "first", 1, &a) == 0);
    ngli_assert(ngl_node_param_add_nodes(&root, "first", 1, &a) == 0);
    ngli_assert(ngl_node_param_swap_elem(&root, "first", 0, 1) == 0);
    struct ngl_scene *scene = create_scene(&root);
    check_list_wrong_type(&root);
    /* A frozen list is refused before its arguments are looked at */
    ngli_assert(ngl_node_param_add_nodes(&root, "first", 0, NULL) == NGL_ERROR_INVALID_USAGE);
    ngli_assert(ngl_node_param_remove_nodes(&root, "first", 0, NULL) == NGL_ERROR_INVALID_USAGE);

    /* Reordering is frozen along with the rest of the graph */
    ngli_assert(ngl_node_param_swap_elem(&root, "first", 0, 1) == NGL_ERROR_INVALID_USAGE);
    ngli_assert(opts.first.data[0] == a && opts.first.data[1] == b);
    check_multi_list_edges(&root);

    /* Protect not-yet-initialized nodes through scene->ctx, including empty edits. */
    struct ngl_ctx ctx = {.in_node_callbacks = true};
    scene->ctx = &ctx;
    ngli_assert(!root.ctx);
    check_list_wrong_type(&root);
    ngli_assert(ngl_node_param_add_nodes(&root, "second", 0, NULL) == NGL_ERROR_INVALID_USAGE);
    ngli_assert(ngl_node_param_remove_nodes(&root, "second", 0, NULL) == NGL_ERROR_INVALID_USAGE);
    ngli_assert(ngl_node_param_swap_elem(&root, "first", 0, 0) == NGL_ERROR_INVALID_USAGE);
    scene->ctx = NULL;
    ngl_scene_unrefp(&scene);

    root.ctx = &ctx;
    ctx.in_node_callbacks = false;
    check_list_wrong_type(&root);
    ngli_assert(ngl_node_param_add_nodes(&root, "first", 0, NULL) == NGL_ERROR_INVALID_USAGE);
    ngli_assert(ngl_node_param_remove_nodes(&root, "first", 0, NULL) == NGL_ERROR_INVALID_USAGE);
    ngli_assert(ngl_node_param_swap_elem(&root, "first", 0, 0) == NGL_ERROR_INVALID_USAGE);
    root.ctx = NULL;
    ngli_params_free(root.opts, root.cls->params);
    ngl_node_unrefp(&a);
    ngl_node_unrefp(&b);
}

static void test_option_insert(void)
{
    for (int retained = 0; retained < 2; retained++) {
        struct multi_list_opts opts;
        struct ngl_node root;
        init_multi_list(&root, &opts);
        struct node_param params[NGLI_ARRAY_NB(multi_list_params)];
        memcpy(params, multi_list_params, sizeof(params));
        params[4].flags = retained ? NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE : 0;
        params[4].update_func = list_update;
        const struct node_class cls = {.name = "OptionInsert", .params = params};
        root.cls = &cls;
        const struct api_impl api = {.dispatch = test_dispatch};
        struct ngl_ctx ctx = {.api_impl = &api};
        struct ngl_node *a = ngl_node_create(NGL_NODE_UNIFORMFLOAT);
        struct ngl_node *b = ngl_node_create(NGL_NODE_GROUP);
        struct ngl_node *c = ngl_node_create(NGL_NODE_GROUP);
        ngli_assert(a && b && c);
        ngli_assert(ngl_node_param_add_nodes(&root, "second", 3, (struct ngl_node *[]){a, b, a}) == 0);
        /* A detached container dispatches its edits like any live change, but
         * has no prepared layout to synchronize the added nodes against. */
        root.ctx = retained ? &ctx : NULL;
        ngli_assert(ngli_node_check_not_traversing(&root) == 0);
        ngli_assert(ngli_node_graph_edit_insert_children(&root, &params[4], 4, 0, NULL) == NGL_ERROR_INVALID_ARG);
        ngli_assert(ngli_node_graph_edit_insert_children(&root, &params[4], 1, 0, NULL) == 0);
        ngli_assert(ngli_node_graph_edit_insert_children(&root, &params[4], 1, 2, (struct ngl_node *[]){a, c}) == 0);
        struct ngl_node *expected[] = {a, a, c, b, a};
        ngli_assert(opts.second.count == NGLI_ARRAY_NB(expected));
        for (size_t i = 0; i < opts.second.count; i++)
            ngli_assert(opts.second.data[i] == expected[i]);
        ngli_assert(a->refcount == 4 && b->refcount == 2 && c->refcount == 2);
        ngli_assert(!root.children.count && !b->ctx && !c->ctx);
        ngli_assert(opts.update_count == retained);
        root.ctx = NULL;
        ngli_params_free(root.opts, root.cls->params);
        ngl_node_unrefp(&a);
        ngl_node_unrefp(&b);
        ngl_node_unrefp(&c);
    }
}

static void test_live_multiple_lists_rollback(size_t index)
{
    const struct api_impl api = {.dispatch = test_dispatch};
    struct ngl_ctx ctx = {.api_impl = &api};
    struct multi_list_opts opts;
    struct ngl_node root;
    init_multi_list(&root, &opts);
    struct ngl_node *kept = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *shared = ngl_node_create(NGL_NODE_UNIFORMFLOAT);
    struct ngl_node *good = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *bad = ngl_node_create(NGL_NODE_CUSTOMTEXTURE);
    ngli_assert(kept && shared && good && bad);
    struct resource_retry retry = {.fail = true};
    struct ngl_node_funcs funcs = {
        .init = retry_init,
        .prepare = retry_prepare,
        .unprepare = retry_unprepare,
        .uninit = retry_uninit,
    };
    ngli_assert(ngl_node_set_funcs(bad, &retry, &funcs) == 0);
    ngli_assert(ngl_node_param_set_node(&root, "prefix", shared) == 0);
    ngli_assert(ngl_node_param_add_nodes(&root, "first", 1, &kept) == 0);
    struct ngl_node *original[] = {shared, shared};
    ngli_assert(ngl_node_param_add_nodes(&root, "second", NGLI_ARRAY_NB(original), original) == 0);
    struct ngl_scene *scene = create_scene(&root);
    ngli_assert(ngli_node_attach_ctx(&root, &ctx) == 0);
    const int shared_refs = shared->refcount;
    struct ngl_node *added[] = {shared, good, shared, bad};
    const size_t nb_added = NGLI_ARRAY_NB(added);
    ngli_assert(ngli_node_check_not_traversing(&root) == 0);
    ngli_assert(ngli_node_graph_edit_insert_children(&root, &multi_list_params[4], index, nb_added, added) == NGL_ERROR_MEMORY);
    ngli_assert(opts.first.count == 1 && opts.first.data[0] == kept);
    ngli_assert(opts.second.count == NGLI_ARRAY_NB(original));
    for (size_t i = 0; i < NGLI_ARRAY_NB(original); i++)
        ngli_assert(opts.second.data[i] == original[i]);
    ngli_assert(shared->refcount == shared_refs && shared->scene == scene);
    ngli_assert(!good->scene && !bad->scene && kept->scene == scene);
    ngli_assert(retry.allocation == NULL && retry.unprepare_count == 1);
    check_multi_list_edges(&root);
    retry.fail = false;
    ngli_assert(ngli_node_check_not_traversing(&root) == 0);
    ngli_assert(ngli_node_graph_edit_insert_children(&root, &multi_list_params[4], index, nb_added, added) == 0);
    ngli_assert(opts.second.count == NGLI_ARRAY_NB(original) + nb_added);
    for (size_t i = 0; i < index; i++)
        ngli_assert(opts.second.data[i] == original[i]);
    for (size_t i = 0; i < nb_added; i++)
        ngli_assert(opts.second.data[index + i] == added[i]);
    for (size_t i = index; i < NGLI_ARRAY_NB(original); i++)
        ngli_assert(opts.second.data[nb_added + i] == original[i]);
    ngli_assert(shared->refcount == shared_refs + 2);
    ngli_assert(retry.init_count == 1 && retry.prepare_count == 2);
    check_multi_list_edges(&root);
    ngli_ctx_release_resources(&ctx);
    ngli_darray_reset(&ctx.resource_nodes);
    ngl_scene_unrefp(&scene);
    ngli_params_free(root.opts, root.cls->params);
    ngl_node_unrefp(&kept);
    ngl_node_unrefp(&shared);
    ngl_node_unrefp(&good);
    ngl_node_unrefp(&bad);
}

int main(void)
{
    test_list_edit_states();
    test_list_edit_permissions();
    test_option_insert();
    test_graph_find();
    test_children_range();
    test_live_multiple_lists();
    test_live_multiple_lists_rollback(0);
    test_live_multiple_lists_rollback(1);
    test_live_multiple_lists_rollback(2);
    test_scene_cycle();
    test_scene_sharing();
    test_duplicate_release();
    test_customtexture_lifecycle();
    test_resources_retry();
    test_add_edges_rollback();
    test_swap_bounds();
    test_live_swap_runtime_edges();
    test_live_children_duplicates();
    test_release_detached_without_resources();
    return 0;
}
