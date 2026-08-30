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

static struct ngl_scene *create_scene(struct ngl_node *root)
{
    struct ngl_scene *scene = ngl_scene_create();
    ngli_assert(scene);
    const struct ngl_scene_params params = ngl_scene_default_params(root);
    ngli_assert(ngl_scene_init(scene, &params) == 0);
    return scene;
}

struct customtexture_lifecycle {
    int step;
};

static int customtexture_init(void *reserved, void *user_data)
{
    struct customtexture_lifecycle *s = user_data;
    ngli_assert(s->step++ == 0);
    return 0;
}

static int customtexture_init_resources(void *reserved, void *user_data)
{
    struct customtexture_lifecycle *s = user_data;
    ngli_assert(s->step++ == 1);
    return 0;
}

static int customtexture_prepare(void *reserved, void *user_data)
{
    struct customtexture_lifecycle *s = user_data;
    ngli_assert(s->step++ == 2);
    return 0;
}

static void customtexture_unprepare(void *reserved, void *user_data)
{
    struct customtexture_lifecycle *s = user_data;
    ngli_assert(s->step++ == 3);
}

static void customtexture_uninit(void *reserved, void *user_data)
{
    struct customtexture_lifecycle *s = user_data;
    ngli_assert(s->step++ == 4);
}

static void test_customtexture_lifecycle(void)
{
    struct customtexture_lifecycle lifecycle = {0};
    struct ngl_node_funcs funcs = {
        .init = customtexture_init,
        .init_resources = customtexture_init_resources,
        .prepare = customtexture_prepare,
        .unprepare = customtexture_unprepare,
        .uninit = customtexture_uninit,
    };
    struct ngl_node *node = ngl_node_create(NGL_NODE_CUSTOMTEXTURE);
    ngli_assert(node);
    ngli_assert(ngl_node_set_funcs(node, &lifecycle, &funcs) == 0);

    struct ngl_ctx ctx = {0};
    ngli_assert(ngli_node_attach_ctx(node, &ctx) == 0);
    ngli_assert(lifecycle.step == 3);

    ngli_node_detach_ctx(node, &ctx);
    ngli_assert(lifecycle.step == 5);

    ngl_node_unrefp(&node);
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
    const int ret = ngli_scene_add_edges(scene, root, root->children.count,
                                         2, added_children);
    ngli_assert(ret == NGL_ERROR_INVALID_USAGE);

    ngli_assert(root->children.count == 2);
    ngli_assert(root->children.data[0] == a);
    ngli_assert(root->children.data[1] == b);
    ngli_assert(root->draw_children.count == 2);
    ngli_assert(root->draw_children.data[0] == a);
    ngli_assert(root->draw_children.data[1] == b);
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

int main(void)
{
    test_customtexture_lifecycle();
    test_add_edges_rollback();
    return 0;
}
