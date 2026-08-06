/*
 * Copyright 2021-2022 GoPro Inc.
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
#include "blending.h"
#include "params.h"

/*
 * The compositing modes come from "Compositing Digital Images", July 1984,
 * by Thomas Porter and Tom Duff. The remaining modes are the common artistic
 * modes found in 2D compositing software, expressed for premultiplied
 * sources. darken and lighten are the exception: the min/max blend operations
 * ignore the blend factors, so the source coverage cannot participate on the
 * color channels; they are meant for opaque or geometry-shaped sources.
 */
static const struct {
    enum ngpu_blend_factor srcf;
    enum ngpu_blend_factor dstf;
    enum ngpu_blend_op op;
} blend_presets[] = {
    [NGLI_BLENDING_SRC_OVER] = {NGPU_BLEND_FACTOR_ONE,                 NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_DST_OVER] = {NGPU_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, NGPU_BLEND_FACTOR_ONE,                 NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_SRC_OUT]  = {NGPU_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, NGPU_BLEND_FACTOR_ZERO,                NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_DST_OUT]  = {NGPU_BLEND_FACTOR_ZERO,                NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_SRC_IN]   = {NGPU_BLEND_FACTOR_DST_ALPHA,           NGPU_BLEND_FACTOR_ZERO,                NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_DST_IN]   = {NGPU_BLEND_FACTOR_ZERO,                NGPU_BLEND_FACTOR_SRC_ALPHA,           NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_SRC_ATOP] = {NGPU_BLEND_FACTOR_DST_ALPHA,           NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_DST_ATOP] = {NGPU_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, NGPU_BLEND_FACTOR_SRC_ALPHA,           NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_XOR]      = {NGPU_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_ADD]      = {NGPU_BLEND_FACTOR_ONE,                 NGPU_BLEND_FACTOR_ONE,                 NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_MULTIPLY] = {NGPU_BLEND_FACTOR_DST_COLOR,           NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_SCREEN]   = {NGPU_BLEND_FACTOR_ONE,                 NGPU_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, NGPU_BLEND_OP_ADD},
    [NGLI_BLENDING_DARKEN]   = {NGPU_BLEND_FACTOR_ONE,                 NGPU_BLEND_FACTOR_ONE,                 NGPU_BLEND_OP_MIN},
    [NGLI_BLENDING_LIGHTEN]  = {NGPU_BLEND_FACTOR_ONE,                 NGPU_BLEND_FACTOR_ONE,                 NGPU_BLEND_OP_MAX},
    [NGLI_BLENDING_SUBTRACT] = {NGPU_BLEND_FACTOR_ONE,                 NGPU_BLEND_FACTOR_ONE,                 NGPU_BLEND_OP_REVERSE_SUBTRACT},
};

const struct param_choices ngli_blending_choices = {
    .name = "blend_preset",
    .consts = {
        {"default",  NGLI_BLENDING_DEFAULT,  .desc=NGLI_DOCSTRING("unchanged current graphics state")},
        {"src_over", NGLI_BLENDING_SRC_OVER, .desc=NGLI_DOCSTRING("this node over destination")},
        {"dst_over", NGLI_BLENDING_DST_OVER, .desc=NGLI_DOCSTRING("destination over this node")},
        {"src_out",  NGLI_BLENDING_SRC_OUT,  .desc=NGLI_DOCSTRING("subtract destination from this node")},
        {"dst_out",  NGLI_BLENDING_DST_OUT,  .desc=NGLI_DOCSTRING("subtract this node from destination")},
        {"src_in",   NGLI_BLENDING_SRC_IN,   .desc=NGLI_DOCSTRING("keep only the part of this node overlapping with destination")},
        {"dst_in",   NGLI_BLENDING_DST_IN,   .desc=NGLI_DOCSTRING("keep only the part of destination overlapping with this node")},
        {"src_atop", NGLI_BLENDING_SRC_ATOP, .desc=NGLI_DOCSTRING("union of `src_in` and `dst_out`")},
        {"dst_atop", NGLI_BLENDING_DST_ATOP, .desc=NGLI_DOCSTRING("union of `src_out` and `dst_in`")},
        {"xor",      NGLI_BLENDING_XOR,      .desc=NGLI_DOCSTRING("exclusive or between this node and the destination")},
        {"add",      NGLI_BLENDING_ADD,      .desc=NGLI_DOCSTRING("add this node to destination")},
        {"multiply", NGLI_BLENDING_MULTIPLY, .desc=NGLI_DOCSTRING("multiply this node with destination")},
        {"screen",   NGLI_BLENDING_SCREEN,   .desc=NGLI_DOCSTRING("multiply the inverses of this node and destination")},
        {"darken",   NGLI_BLENDING_DARKEN,   .desc=NGLI_DOCSTRING("keep the darkest channels of this node and destination (the source coverage is ignored)")},
        {"lighten",  NGLI_BLENDING_LIGHTEN,  .desc=NGLI_DOCSTRING("keep the lightest channels of this node and destination (the source coverage is ignored)")},
        {"subtract", NGLI_BLENDING_SUBTRACT, .desc=NGLI_DOCSTRING("subtract this node from destination")},
        {NULL}
    }
};

int ngli_blending_apply_preset(struct ngpu_graphics_state *state, enum ngli_blending preset)
{
    if (preset == NGLI_BLENDING_DEFAULT)
        return 0;
    if (preset < 0 || preset >= NGLI_ARRAY_NB(blend_presets))
        return NGL_ERROR_INVALID_ARG;
    const enum ngpu_blend_factor srcf = blend_presets[preset].srcf;
    const enum ngpu_blend_factor dstf = blend_presets[preset].dstf;
    const enum ngpu_blend_op op = blend_presets[preset].op;
    state->blend = 1;
    state->blend_src_factor   = srcf;
    state->blend_dst_factor   = dstf;
    state->blend_op           = op;
    state->blend_src_factor_a = srcf;
    state->blend_dst_factor_a = dstf;
    state->blend_op_a         = op;

    /*
     * Taking the min/max of the premultiplied alphas would shrink or expand
     * the destination coverage instead of accumulating it, so the alpha
     * channel keeps compositing src_over. For fully opaque sources (the only
     * content these modes are exact for), this yields an alpha of 1, exactly
     * like the hand-built min/max recipe (blend_op=min/max with the default
     * one/zero blend factors) which replaces the destination alpha with the
     * source one.
     */
    if (op == NGPU_BLEND_OP_MIN || op == NGPU_BLEND_OP_MAX) {
        state->blend_src_factor_a = NGPU_BLEND_FACTOR_ONE;
        state->blend_dst_factor_a = NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state->blend_op_a         = NGPU_BLEND_OP_ADD;
    }

    return 0;
}
