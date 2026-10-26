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

#include <math.h>

#include "internal.h"
#include "utils/memory.h"

struct prefetch_probe {
    double time;
    int count;
};

static int record_prefetch(struct ngl_node *node)
{
    struct prefetch_probe *s = node->priv_data;
    s->time = node->prefetch_time;
    s->count++;
    return 0;
}

static const struct node_class probe_class = {
    .id = NGL_NODE_GROUP2D,
    .name = "prefetch_probe",
    .prefetch = record_prefetch,
    .flags = NGLI_NODE_FLAG_SHAREABLE,
};

struct range {
    double start;
    double end;
    double render_time;
};

static struct ngl_node *create_filter(uint32_t type, struct ngl_node *child, struct range range)
{
    struct ngl_node *node = ngl_node_create(type);
    ngli_assert(node);
    ngli_assert(ngl_node_param_set_node(node, "child", child) == 0);
    ngli_assert(ngl_node_param_set_f64(node, "start", range.start) == 0);
    ngli_assert(ngl_node_param_set_f64(node, "end", range.end) == 0);
    ngli_assert(ngl_node_param_set_f64(node, "render_time", range.render_time) == 0);
    ngli_assert(ngl_node_param_set_f64(node, "prefetch_time", 10.0) == 0);
    return node;
}

static struct prefetch_probe visit(uint32_t type, double t,
                                   struct range first, const struct range *second, bool nested)
{
    struct ngl_ctx ctx = {0};
    struct prefetch_probe probe = {.time = -1.0};
    struct ngl_node leaf = {
        .cls = &probe_class,
        .label = "prefetch_probe",
        .priv_data = &probe,
        .refcount = 1,
        .visit_time = -1.0,
        .last_update_time = -1.0,
    };
    struct ngl_node *root = ngl_node_create(NGL_NODE_GROUP);
    struct ngl_node *a = create_filter(type, &leaf, first);
    struct ngl_node *b = second ? create_filter(type, nested ? a : &leaf, *second) : NULL;
    struct ngl_node *branches[] = {nested ? b : a, b};
    ngli_assert(ngl_node_param_add_nodes(root, "children", second && !nested ? 2 : 1, branches) == 0);
    struct ngl_scene *scene = ngl_scene_create();
    ngli_assert(scene);
    const struct ngl_scene_params params = ngl_scene_default_params(root);
    ngli_assert(ngl_scene_init(scene, &params) == 0);
    ngli_assert(ngli_node_set_ctx(root, &ctx) == 0);

    ngli_assert(ngli_node_honor_release_prefetch(root, t) == 0);
    const struct prefetch_probe ret = probe;
    /* Advancing scene time must not call prefetch again while resources are ready. */
    ngli_assert(ngli_node_honor_release_prefetch(root, t + 0.01) == 0);
    ngli_assert(probe.count == ret.count);

    ngli_ctx_release_resources(&ctx);
    ngli_darray_reset(&ctx.resource_nodes);
    ngli_darray_reset(&ctx.activitycheck_nodes);
    ngl_scene_unrefp(&scene);
    ngl_node_unrefp(&root);
    ngl_node_unrefp(&a);
    ngl_node_unrefp(&b);
    ngli_assert(leaf.refcount == 1);
    ngli_darray_reset(&leaf.children);
    ngli_darray_reset(&leaf.parents);
    return ret;
}

static void test_prefetch(uint32_t type, double t,
                          struct range first, const struct range *second, bool nested,
                          double expected)
{
    const struct prefetch_probe probe = visit(type, t, first, second, nested);
    ngli_assert(expected < 0.0 || probe.count == 1);
    ngli_assert(fabs(probe.time - expected) < 1e-9);
}

int main(void)
{
    const uint32_t types[] = {NGL_NODE_TIMERANGEFILTER, NGL_NODE_TIMERANGEFILTER2D};
    for (size_t i = 0; i < NGLI_ARRAY_NB(types); i++) {
        const uint32_t type = types[i];
        /* Already visible: prepare for the current scene time, 2.5s. */
        test_prefetch(type, 2.5, (struct range){0, -1, -1}, NULL, false, 2.5);
        /* Visible from 3s: prepare for 3s even though prefetch runs at 2.5s. */
        test_prefetch(type, 2.5, (struct range){3, 5, -1}, NULL, false, 3);
    }
    return 0;
}
