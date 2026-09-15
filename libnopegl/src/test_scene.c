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
#include "node_texture.h"
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
            .visiting_id = ngli_node_new_traversal_id(),
            .visited_id = ngli_node_new_traversal_id(),
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
    ngli_image_resource_freep(node->priv_data);
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
    struct ngl_node *foreign = ngl_node_create(NGL_NODE_GROUP);
    ngli_assert(root && a && b && c && foreign);

    struct ngl_node *children[] = {a, b};
    ngli_assert(ngl_node_param_add_nodes(root, "children", 2, children) == 0);

    struct ngl_scene *scene = create_scene(root);
    struct ngl_scene *foreign_scene = create_scene(foreign);

    struct ngl_node *added_children[] = {c, foreign};
    const int ret = ngli_scene_add_edges(root, root->children.count, 2, added_children);
    ngli_assert(ret == NGL_ERROR_INVALID_USAGE);

    ngli_assert(root->children.count == 2);
    ngli_assert(root->children.data[0] == a);
    ngli_assert(root->children.data[1] == b);
    ngli_assert(a->parents.count == 1 && a->parents.data[0] == root);
    ngli_assert(b->parents.count == 1 && b->parents.data[0] == root);
    ngli_assert(c->scene == NULL);
    ngli_assert(c->parents.count == 0);
    ngli_assert(scene->nodes.count == 3);
    ngli_assert(ngli_node_darray_find(&scene->nodes, c) == SIZE_MAX);
    ngli_assert(foreign->scene == foreign_scene);

    ngl_scene_unrefp(&foreign_scene);
    ngl_scene_unrefp(&scene);
    ngl_node_unrefp(&foreign);
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

int main(void)
{
    test_scene_cycle();
    test_scene_sharing();
    test_swap_bounds();
    test_duplicate_release();
    test_customtexture_lifecycle();
    test_resources_retry();
    test_add_edges_rollback();
    return 0;
}
