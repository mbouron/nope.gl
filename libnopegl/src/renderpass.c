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

enum {
    RENDER_PASS_STATE_NONE,
    RENDER_PASS_STATE_STARTED,
    RENDER_PASS_STATE_STOPPED,
};

static uint32_t get_renderpass_usage(const struct ngl_node *node)
{
    if (!node->cls->get_renderpass_usage)
        return 0;

    return node->cls->get_renderpass_usage(node);
}

static int get_renderpass_info(const struct ngl_node *node, int state, struct renderpass_info *info)
{
    info->usage |= get_renderpass_usage(node);

    for (size_t i = 0; i < node->children.count; i++) {
        const struct ngl_node *child = node->children.data[i];
        if (child->cls->id == NGL_NODE_RENDERTOTEXTURE ||
            child->cls->id == NGL_NODE_COMPUTE) {
            if (state == RENDER_PASS_STATE_STARTED)
                state = RENDER_PASS_STATE_STOPPED;
        } else if (child->cls->id == NGL_NODE_TEXTURE2D) {
            struct texture_info *texture_info = child->priv_data;
            if (texture_info->rtt && state == RENDER_PASS_STATE_STARTED)
                state = RENDER_PASS_STATE_STOPPED;
        } else if (child->cls->category == NGLI_NODE_CATEGORY_DRAW) {
            state = get_renderpass_info(child, state, info);
            if (state == RENDER_PASS_STATE_STOPPED)
                info->nb_interruptions++;
            state = RENDER_PASS_STATE_STARTED;
        } else {
            state = get_renderpass_info(child, state, info);
        }
    }
    return state;
}

void ngli_node_get_renderpass_info(const struct ngl_node *node, struct renderpass_info *info)
{
    get_renderpass_info(node, RENDER_PASS_STATE_NONE, info);
}
