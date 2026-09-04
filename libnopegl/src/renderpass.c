/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include "internal.h"
#include "node_texture.h"
#include "nopegl/nopegl.h"
#include "renderpass.h"

static uint32_t get_renderpass_usage(const struct ngl_node *node)
{
    if (!node->cls->get_renderpass_usage)
        return 0;

    return node->cls->get_renderpass_usage(node);
}

static void get_renderpass_reqs(const struct ngl_node *node, struct renderpass_reqs *reqs)
{
    reqs->usage |= get_renderpass_usage(node);

    for (size_t i = 0; i < node->children.count; i++) {
        const struct ngl_node *child = node->children.data[i];
        if (child->cls->id == NGL_NODE_RENDERTOTEXTURE ||
            child->cls->id == NGL_NODE_COMPUTE) {
            continue;
        } else if (child->cls->id == NGL_NODE_TEXTURE2D) {
            struct texture_info *texture_info = child->priv_data;
            if (texture_info->rtt)
                continue;
        }

        get_renderpass_reqs(child, reqs);
    }
}

void ngli_node_get_renderpass_reqs(struct ngl_node * const *children, size_t nb_children,
                                   struct renderpass_reqs *reqs)
{
    for (size_t i = 0; i < nb_children; i++)
        get_renderpass_reqs(children[i], reqs);
}
