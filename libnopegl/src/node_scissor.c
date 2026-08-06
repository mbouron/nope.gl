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

#include <math.h>
#include <stddef.h>

#include "internal.h"
#include <ngpu/ngpu.h>
#include "nopegl/nopegl.h"
#include "params.h"
#include "utils/utils.h"

struct scissor_opts {
    struct ngl_node *child;
    struct ngli_vec4 rect;
};

static int scissor_update_rect(struct ngl_node *node)
{
    struct scissor_opts *o = node->opts;

    o->rect.v[0] = NGLI_CLAMP(o->rect.v[0], 0.f, 1.f);
    o->rect.v[1] = NGLI_CLAMP(o->rect.v[1], 0.f, 1.f);
    o->rect.v[2] = NGLI_CLAMP(o->rect.v[2], 0.f, 1.f);
    o->rect.v[3] = NGLI_CLAMP(o->rect.v[3], 0.f, 1.f);

    return 0;
}

#define OFFSET(x) offsetof(struct scissor_opts, x)
static const struct node_param scissor_params[] = {
    {
        .key = "child",
        .type = NGLI_PARAM_TYPE_NODE,
        .offset = OFFSET(child),
        .flags = NGLI_PARAM_FLAG_NON_NULL,
        .desc = NGLI_DOCSTRING("scene to which the scissor will be applied")
    },
    {
        .key = "rect",
        .type = NGLI_PARAM_TYPE_VEC4,
        .offset = OFFSET(rect.v),
        .def_value = {.vec={0.f, 0.f, 1.f, 1.f}},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("rectangular area (x, y, width, height) in normalized framebuffer "
                               "coordinates where the rendering is restricted; all pixels outside "
                               "are discarded (the default covers the whole framebuffer)"),
        .update_func = scissor_update_rect,
    },
    {NULL}
};

static int scissor_init(struct ngl_node *node)
{
    return scissor_update_rect(node);
}

static void scissor_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    const struct scissor_opts *o = node->opts;

    const uint32_t width = ngpu_rendertarget_get_width(ctx->current_rendertarget);
    const uint32_t height = ngpu_rendertarget_get_height(ctx->current_rendertarget);
    const uint32_t x = NGLI_MIN((uint32_t)lroundf(o->rect.v[0] * (float)width), width);
    const uint32_t y = NGLI_MIN((uint32_t)lroundf(o->rect.v[1] * (float)height), height);

    const struct ngpu_scissor prev_scissor = ctx->scissor;
    ctx->scissor = (struct ngpu_scissor){
        .x      = x,
        .y      = y,
        .width  = NGLI_MIN((uint32_t)lroundf(o->rect.v[2] * (float)width), width - x),
        .height = NGLI_MIN((uint32_t)lroundf(o->rect.v[3] * (float)height), height - y),
    };

    ngli_node_draw(o->child);

    ctx->scissor = prev_scissor;
}

const struct node_class ngli_scissor_class = {
    .id        = NGL_NODE_SCISSOR,
    .name      = "Scissor",
    .init      = scissor_init,
    .update    = ngli_node_update_children,
    .pre_draw  = ngli_node_pre_draw_children,
    .draw      = scissor_draw,
    .opts_size = sizeof(struct scissor_opts),
    .params    = scissor_params,
    .file      = __FILE__,
};
