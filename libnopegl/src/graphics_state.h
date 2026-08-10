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

#ifndef GRAPHICS_STATE_H
#define GRAPHICS_STATE_H

#include <ngpu/ngpu.h>

#include "blend_mode.h"

enum ngli_depth_mode {
    NGLI_DEPTH_MODE_DISABLED,
    NGLI_DEPTH_MODE_READ_WRITE,
    NGLI_DEPTH_MODE_READ_ONLY,
    NGLI_DEPTH_MODE_WRITE_ONLY,
    NGLI_DEPTH_MODE_MAX_ENUM = 0x7FFFFFFF
};

enum ngli_stencil_mode {
    NGLI_STENCIL_MODE_DISABLED,
    NGLI_STENCIL_MODE_WRITE,
    NGLI_STENCIL_MODE_READ,
    NGLI_STENCIL_MODE_READ_INVERTED,
    NGLI_STENCIL_MODE_MAX_ENUM = 0x7FFFFFFF
};

extern const struct param_choices ngli_depth_mode_choices;
extern const struct param_choices ngli_stencil_mode_choices;
extern const struct param_choices ngli_cull_mode_choices;
extern const struct param_choices ngli_component_choices;

struct ngli_graphics_state_opts {
    enum ngli_blend_mode blend_mode;
    enum ngli_depth_mode depth_mode;
    enum ngli_stencil_mode stencil_mode;
    enum ngpu_cull_mode cull_mode;
    int color_write_mask;
};

#define NGLI_GRAPHICS_STATE_PARAMS(field)                                                                                                      \
    {                                                                                                                                          \
        .key       = "blend_mode",                                                                                                             \
        .type      = NGLI_PARAM_TYPE_SELECT,                                                                                                   \
        .offset    = OFFSET(field.blend_mode),                                                                                                 \
        .choices   = &ngli_blend_mode_choices,                                                                                                 \
        .desc      = NGLI_DOCSTRING("define how this node is composited with the current framebuffer"),                                        \
    },                                                                                                                                         \
    {                                                                                                                                          \
        .key       = "depth_mode",                                                                                                             \
        .type      = NGLI_PARAM_TYPE_SELECT,                                                                                                   \
        .offset    = OFFSET(field.depth_mode),                                                                                                 \
        .choices   = &ngli_depth_mode_choices,                                                                                                 \
        .desc      = NGLI_DOCSTRING("define how this node interacts with the depth buffer"),                                                   \
    },                                                                                                                                         \
    {                                                                                                                                          \
        .key       = "stencil_mode",                                                                                                           \
        .type      = NGLI_PARAM_TYPE_SELECT,                                                                                                   \
        .offset    = OFFSET(field.stencil_mode),                                                                                               \
        .choices   = &ngli_stencil_mode_choices,                                                                                               \
        .desc      = NGLI_DOCSTRING("define how this node interacts with the stencil buffer"),                                                 \
    },                                                                                                                                         \
    {                                                                                                                                          \
        .key       = "cull_mode",                                                                                                              \
        .type      = NGLI_PARAM_TYPE_SELECT,                                                                                                   \
        .offset    = OFFSET(field.cull_mode),                                                                                                  \
        .choices   = &ngli_cull_mode_choices,                                                                                                  \
        .desc      = NGLI_DOCSTRING("face culling mode"),                                                                                      \
    },                                                                                                                                         \
    {                                                                                                                                          \
        .key       = "color_write_mask",                                                                                                       \
        .type      = NGLI_PARAM_TYPE_FLAGS,                                                                                                    \
        .offset    = OFFSET(field.color_write_mask),                                                                                           \
        .def_value = {.i32=NGPU_COLOR_COMPONENT_R_BIT | NGPU_COLOR_COMPONENT_G_BIT | NGPU_COLOR_COMPONENT_B_BIT | NGPU_COLOR_COMPONENT_A_BIT}, \
        .choices   = &ngli_component_choices,                                                                                                  \
        .desc      = NGLI_DOCSTRING("color write mask"),                                                                                       \
    }

int ngli_graphics_state_init_from_opts(struct ngpu_ctx *gpu_ctx,
                                       struct ngpu_graphics_state *state,
                                       const struct ngli_graphics_state_opts *opts);

int ngli_graphics_state_check_rendertarget_layout(const struct ngpu_graphics_state *state,
                                                  const struct ngpu_rendertarget_layout *rendertarget_layout);

uint32_t ngli_graphics_state_get_renderpass_usage(const struct ngli_graphics_state_opts *opts);

#endif
