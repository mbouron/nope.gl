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

#include "nopegl/nopegl.h"
#include "graphics_state.h"
#include "log.h"
#include "params.h"
#include "renderpass.h"

const struct param_choices ngli_depth_mode_choices = {
    .name = "depth_mode",
    .consts = {
        {"disabled",   NGLI_DEPTH_MODE_DISABLED,   .desc=NGLI_DOCSTRING("disable depth testing and writing")},
        {"read_write", NGLI_DEPTH_MODE_READ_WRITE, .desc=NGLI_DOCSTRING("draw fragments whose depth is less than or equal to the stored depth, then update it")},
        {"read_only",  NGLI_DEPTH_MODE_READ_ONLY,  .desc=NGLI_DOCSTRING("draw fragments whose depth is less than or equal to the stored depth, without updating it")},
        {"write_only", NGLI_DEPTH_MODE_WRITE_ONLY, .desc=NGLI_DOCSTRING("disable depth comparison and update the stored depth")},
        {NULL}
    }
};

const struct param_choices ngli_stencil_mode_choices = {
    .name = "stencil_mode",
    .consts = {
        {"disabled",      NGLI_STENCIL_MODE_DISABLED,      .desc=NGLI_DOCSTRING("disable stencil testing and writing")},
        {"write",         NGLI_STENCIL_MODE_WRITE,         .desc=NGLI_DOCSTRING("mark fragments in the stencil buffer after they pass the depth test")},
        {"read",          NGLI_STENCIL_MODE_READ,          .desc=NGLI_DOCSTRING("draw only where the stencil buffer is marked")},
        {"read_inverted", NGLI_STENCIL_MODE_READ_INVERTED, .desc=NGLI_DOCSTRING("draw only where the stencil buffer is not marked")},
        {NULL}
    }
};

const struct param_choices ngli_cull_mode_choices = {
    .name = "cull_mode",
    .consts = {
        {"none",  NGPU_CULL_MODE_NONE,      .desc=NGLI_DOCSTRING("no facets are discarded")},
        {"front", NGPU_CULL_MODE_FRONT_BIT, .desc=NGLI_DOCSTRING("cull front-facing facets")},
        {"back",  NGPU_CULL_MODE_BACK_BIT,  .desc=NGLI_DOCSTRING("cull back-facing facets")},
        {NULL}
    }
};

const struct param_choices ngli_component_choices = {
    .name = "component",
    .consts = {
        {"r", NGPU_COLOR_COMPONENT_R_BIT, .desc=NGLI_DOCSTRING("red")},
        {"g", NGPU_COLOR_COMPONENT_G_BIT, .desc=NGLI_DOCSTRING("green")},
        {"b", NGPU_COLOR_COMPONENT_B_BIT, .desc=NGLI_DOCSTRING("blue")},
        {"a", NGPU_COLOR_COMPONENT_A_BIT, .desc=NGLI_DOCSTRING("alpha")},
        {NULL}
    }
};

static int depth_mode_apply(struct ngpu_graphics_state *state, enum ngli_depth_mode mode)
{
    switch (mode) {
    case NGLI_DEPTH_MODE_DISABLED:
        state->depth_test  = 0;
        state->depth_write = 0;
        break;
    case NGLI_DEPTH_MODE_READ_WRITE:
        state->depth_test  = 1;
        state->depth_write = 1;
        state->depth_func  = NGPU_COMPARE_OP_LESS_OR_EQUAL;
        break;
    case NGLI_DEPTH_MODE_READ_ONLY:
        state->depth_test  = 1;
        state->depth_write = 0;
        state->depth_func  = NGPU_COMPARE_OP_LESS_OR_EQUAL;
        break;
    case NGLI_DEPTH_MODE_WRITE_ONLY:
        state->depth_test  = 1;
        state->depth_write = 1;
        state->depth_func  = NGPU_COMPARE_OP_ALWAYS;
        break;
    default:
        return NGL_ERROR_INVALID_ARG;
    }

    return 0;
}

static int stencil_mode_apply(struct ngpu_graphics_state *state, enum ngli_stencil_mode mode)
{
    struct ngpu_stencil_op_state op;

    switch (mode) {
    case NGLI_STENCIL_MODE_DISABLED:
        state->stencil_test = 0;
        return 0;
    case NGLI_STENCIL_MODE_WRITE:
        op = (struct ngpu_stencil_op_state){
            .write_mask = 0xff,
            .func       = NGPU_COMPARE_OP_ALWAYS,
            .ref        = 1,
            .read_mask  = 0xff,
            .fail       = NGPU_STENCIL_OP_KEEP,
            .depth_fail = NGPU_STENCIL_OP_KEEP,
            .depth_pass = NGPU_STENCIL_OP_REPLACE,
        };
        break;
    case NGLI_STENCIL_MODE_READ:
    case NGLI_STENCIL_MODE_READ_INVERTED:
        op = (struct ngpu_stencil_op_state){
            .write_mask = 0,
            .func       = mode == NGLI_STENCIL_MODE_READ ? NGPU_COMPARE_OP_EQUAL
                                                         : NGPU_COMPARE_OP_NOT_EQUAL,
            .ref        = 1,
            .read_mask  = 0xff,
            .fail       = NGPU_STENCIL_OP_KEEP,
            .depth_fail = NGPU_STENCIL_OP_KEEP,
            .depth_pass = NGPU_STENCIL_OP_KEEP,
        };
        break;
    default:
        return NGL_ERROR_INVALID_ARG;
    }

    state->stencil_test  = 1;
    state->stencil_front = op;
    state->stencil_back  = op;

    return 0;
}

int ngli_graphics_state_init_from_opts(struct ngpu_ctx *gpu_ctx,
                                       struct ngpu_graphics_state *state,
                                       const struct ngli_graphics_state_opts *opts)
{
    *state = NGPU_GRAPHICS_STATE_DEFAULTS;

    int ret = ngli_blend_mode_apply(state, opts->blend_mode);
    if (ret < 0)
        return ret;

    ret = depth_mode_apply(state, opts->depth_mode);
    if (ret < 0)
        return ret;

    ret = stencil_mode_apply(state, opts->stencil_mode);
    if (ret < 0)
        return ret;

    state->cull_mode = ngpu_ctx_get_cull_mode(gpu_ctx, opts->cull_mode);

    state->color_write_mask = (uint32_t)opts->color_write_mask;

    return 0;
}

int ngli_graphics_state_check_rendertarget_layout(const struct ngpu_graphics_state *state,
                                                  const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    const enum ngpu_format format = rendertarget_layout->depth_stencil.format;

    if (state->depth_test && !ngpu_format_has_depth(format)) {
        LOG(ERROR, "depth testing is not supported on rendertargets with no depth attachment");
        return NGL_ERROR_INVALID_USAGE;
    }

    if (state->stencil_test && !ngpu_format_has_stencil(format)) {
        LOG(ERROR, "stencil operations are not supported on rendertargets with no stencil attachment");
        return NGL_ERROR_INVALID_USAGE;
    }

    return 0;
}

uint32_t ngli_graphics_state_get_renderpass_usage(const struct ngli_graphics_state_opts *opts)
{
    uint32_t usage = 0;
    if (opts->depth_mode != NGLI_DEPTH_MODE_DISABLED)
        usage |= NGLI_RENDERPASS_USAGE_DEPTH;
    if (opts->stencil_mode != NGLI_STENCIL_MODE_DISABLED)
        usage |= NGLI_RENDERPASS_USAGE_STENCIL;
    return usage;
}
