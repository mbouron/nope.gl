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

static void test_add_edges_rollback(void)
{
    struct ngl_node *root = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *a = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *b = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *foreign = ngl_node_create(NGL_NODE_GROUP);
    ngli_assert(root && a && b && foreign);

    struct ngl_node *children[] = {a, b};
    ngli_assert(ngl_node_param_add_nodes(root, "children", 2, children) == 0);

    struct ngl_scene *scene = create_scene(root);
    struct ngl_scene *foreign_scene = create_scene(foreign);

    struct ngl_node *added_children[] = {a, foreign};
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

    ngl_scene_unrefp(&foreign_scene);
    ngl_scene_unrefp(&scene);
    ngl_node_unrefp(&foreign);
    ngl_node_unrefp(&b);
    ngl_node_unrefp(&a);
    ngl_node_unrefp(&root);
}

int main(void)
{
    test_add_edges_rollback();
    return 0;
}
