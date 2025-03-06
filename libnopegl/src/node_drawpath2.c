/*
 * Copyright 2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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
#include <string.h>

#include "blending.h"
#include "internal.h"
#include "log.h"
#include "math_utils.h"
#include "nanovg/nanovg.h"
#include "nanovg/nanovg_ngl.h"
#include "ngpu/format.h"
#include "node_draw.h"
#include "params.h"

struct pipeline_desc {
    uint64_t hash;
    NVGcontext *nvg_ctx;
};

struct drawpath2_opts {
    struct ngl_path_instruction *instructions;
    size_t instructions_size;
    int style;
    float color[3];
    float opacity;
    float stroke_width;
    int blending;
    int compute_bounds;
};

struct drawpath2_priv {
    struct draw_info draw_info;
    struct aabb aabb;
    struct darray pipeline_descs;
    size_t instruction_count;
};

NGLI_STATIC_ASSERT(nvpath_drawinfo, offsetof(struct drawpath2_priv, draw_info) == 0);

enum {
    STYLE_FILL,
    STYLE_STROKE,
};

static const struct param_choices style_choices = {
    .name = "style",
    .consts = {
        {"fill", STYLE_FILL, .desc = NGLI_DOCSTRING("fill path")},
        {"stroke", STYLE_STROKE, .desc = NGLI_DOCSTRING("stroke path")},
        {NULL},
    },
};

static int update_instructions(struct ngl_node *node);

#define OFFSET(x) offsetof(struct drawpath2_opts, x)
static const struct node_param drawpath2_params[] = {
    {
        .key = "instructions",
        .type = NGLI_PARAM_TYPE_DATA,
        .offset = OFFSET(instructions),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("array of `count` instructions"),
        .update_func = update_instructions,
     },
    {
        .key = "style",
        .type = NGLI_PARAM_TYPE_SELECT,
        .offset = OFFSET(style),
        .def_value = {.i32 = STYLE_FILL},
        .choices = &style_choices,
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("path paint style")},
    {
        .key = "color",
        .type = NGLI_PARAM_TYPE_VEC3,
        .offset = OFFSET(color),
        .def_value = {.vec = {1.0f, 1.0f, 1.0f}},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("path color"),
     },
    {
        .key = "opacity",
        .type = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(opacity),
        .def_value = {.f32 = 1.0f},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("path color opacity"),
     },
    {
        .key = "stroke_width",
        .type = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(stroke_width),
        .def_value = {.f32 = 1.0f},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("stroke width"),
     },
    {
         .key = "blending",
        .type = NGLI_PARAM_TYPE_SELECT,
        .offset = OFFSET(blending),
        .choices = &ngli_blending_choices,
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("define how this node and the current frame buffer are blending together"),
     },
    {
        .key = "compute_bounds",
        .type = NGLI_PARAM_TYPE_BOOL,
        .offset = OFFSET(compute_bounds),
        .desc = NGLI_DOCSTRING("enable bounding box computation"),
     },
    {NULL}
};

static int update_instructions(struct ngl_node *node)
{
    const struct drawpath2_opts *o = node->opts;
    struct drawpath2_priv *s = node->priv_data;

    s->instruction_count = o->instructions_size / sizeof(struct ngl_path_instruction);
    for (size_t i = 0; i < s->instruction_count; i++) {
        const struct ngl_path_instruction *inst = &o->instructions[i];
        if (inst->verb < NGL_PATH_INSTRUCTION_VERB_MOVE
            || inst->verb > NGL_PATH_INSTRUCTION_VERB_CLOSE)
            return NGL_ERROR_INVALID_DATA;
    }

    if (o->compute_bounds) {
        struct darray vertices;
        ngli_darray_init(&vertices, 3 * sizeof(float), 0);

        const struct ngpu_viewport *viewport = &node->ctx->viewport;
        for (size_t i = 0; i < s->instruction_count; i++) {
            const struct ngl_path_instruction *inst = &o->instructions[i];
            if (inst->verb < NGL_PATH_INSTRUCTION_VERB_CLOSE) {
                const NGLI_ALIGNED_VEC(vertex) = {
                    inst->params[0],
                    inst->params[1],
                    0.f,
                    1.f
                };
                // rescale from viewport coordinates to normalized coordinates
                /* Scale from [0,viewport] to [-1,1], swapping y-axis */
                const float r2w = 2.0f / (float)viewport->width;
                const float r2h = 2.0f / (float)viewport->height;
                const NGLI_ALIGNED_MAT(remap_uv_to_centered_matrix) = {
                    r2w, 0.f, 0.f, 0.f,
                    0.f,-r2h, 0.f, 0.f,
                    0.f, 0.f, 1.f, 0.f,
                   -1.f, 1.f, 0.f, 1.f,
                };
                NGLI_ALIGNED_VEC(normalized_vertex);
                ngli_mat4_mul_vec4(normalized_vertex, remap_uv_to_centered_matrix, vertex);
                if (!ngli_darray_push(&vertices, normalized_vertex)) {
                    ngli_darray_reset(&vertices);
                    return NGL_ERROR_MEMORY;
                }
            }
        }

        s->aabb = ngli_aabb_from_vertices(ngli_darray_data(&vertices),ngli_darray_count(&vertices));
        ngli_darray_reset(&vertices);
    }

    return 0;
}

#include "utils/xxhash.h"

static uint64_t hash_pipeline_state(const struct ngpu_rendertarget_layout *rt_layout,
                                    const struct ngpu_graphics_state *state)
{
    uint64_t hash = 0;
    hash = XXH64(rt_layout, sizeof(*rt_layout), hash);
    hash = XXH64(state, sizeof(*state), hash);
    return hash;
}

static struct pipeline_desc *get_pipeline(struct ngl_node *node,
                                          const struct ngpu_rendertarget_layout *rt_layout,
                                          const struct ngpu_graphics_state *state)
{
    struct drawpath2_priv *s = node->priv_data;

    const uint64_t hash = hash_pipeline_state(rt_layout, state);
    for (size_t i = 0; i < ngli_darray_count(&s->pipeline_descs); i++) {
        struct pipeline_desc *desc = ngli_darray_get(&s->pipeline_descs, i);
        if (desc->hash == hash)
            return desc;
    }
    ngli_assert(0);
}


static int drawpath2_init(struct ngl_node *node)
{
    struct drawpath2_priv *s = node->priv_data;
    const struct drawpath2_opts *o = node->opts;
    struct draw_info *draw_info = &s->draw_info;

    ngli_darray_init(&s->pipeline_descs, sizeof(struct pipeline_desc), 0);

    draw_info->compute_bounds = o->compute_bounds;

    int ret = update_instructions(node);
    if (ret < 0)
        return ret;

    return 0;
}

static int drawpath2_prepare(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct drawpath2_priv *s = node->priv_data;

    const enum ngpu_format format = ctx->rendertarget_layout.depth_stencil.format;
    if (!ngpu_format_has_depth(format)) {
        LOG(ERROR, "depth testing is not supported on rendertargets with no depth attachment");
        return NGL_ERROR_INVALID_USAGE;
    }

    if (!ngpu_format_has_stencil(format)) {
        LOG(ERROR,
            "stencil operations are not supported on rendertargets with no stencil attachment");
        return NGL_ERROR_INVALID_USAGE;
    }

    struct pipeline_desc *desc = ngli_darray_push(&s->pipeline_descs, NULL);
    if (!desc)
        return NGL_ERROR_MEMORY;

    struct ngpu_graphics_state state = NGPU_GRAPHICS_STATE_DEFAULTS;
    desc->hash = hash_pipeline_state(&ctx->rendertarget_layout, &state);

    const NGPU_NVGParams params = {
        .ngl_ctx = node->ctx,
        .rendertarget_layout = ctx->rendertarget_layout,
    };
    desc->nvg_ctx = ngpu_nvgCreate(&params);
    if (!desc->nvg_ctx)
        return NGL_ERROR_MEMORY;

    return 0;
}

#define NVG_UNDEFINED (-1)

static const enum NVGcompositeOperation nvg_blending_map[] = {
    [NGLI_BLENDING_DEFAULT] = NVG_UNDEFINED,
    [NGLI_BLENDING_SRC_OVER] = NVG_SOURCE_OVER,
    [NGLI_BLENDING_DST_OVER] = NVG_DESTINATION_OVER,
    [NGLI_BLENDING_SRC_OUT] = NVG_SOURCE_OUT,
    [NGLI_BLENDING_DST_OUT] = NVG_DESTINATION_OUT,
    [NGLI_BLENDING_SRC_IN] = NVG_SOURCE_IN,
    [NGLI_BLENDING_DST_IN] = NVG_DESTINATION_IN,
    [NGLI_BLENDING_SRC_ATOP] = NVG_ATOP,
    [NGLI_BLENDING_DST_ATOP] = NVG_DESTINATION_ATOP,
    [NGLI_BLENDING_XOR] = NVG_XOR,
};

static int get_nvg_blending(int blending)
{
    ngli_assert(blending >= 0 && blending < NGLI_ARRAY_NB(nvg_blending_map));
    return nvg_blending_map[blending];
}

static void handle_instruction(struct ngl_node *node,
                               struct pipeline_desc *desc,
                               const struct ngl_path_instruction *instruction)
{
    struct drawpath2_priv *s = node->priv_data;
    struct ngl_ctx *ctx = node->ctx;

    // TODO: add moveTo instruction if first path instruction is not a moveTo
    switch (instruction->verb) {
    case NGL_PATH_INSTRUCTION_VERB_MOVE:
        nvgMoveTo(desc->nvg_ctx, instruction->params[0], instruction->params[1]);
        break;
    case NGL_PATH_INSTRUCTION_VERB_LINE:
        nvgLineTo(desc->nvg_ctx, instruction->params[2], instruction->params[3]);
        break;
    case NGL_PATH_INSTRUCTION_VERB_QUAD:
        nvgQuadTo(desc->nvg_ctx, instruction->params[2], instruction->params[3],
                  instruction->params[4], instruction->params[5]);
        break;
    case NGL_PATH_INSTRUCTION_VERB_CONIC:
        nvgConicTo(desc->nvg_ctx, instruction->params[2], instruction->params[3],
                   instruction->params[4], instruction->params[5], instruction->weight);
        break;
    case NGL_PATH_INSTRUCTION_VERB_CUBIC:
        nvgBezierTo(desc->nvg_ctx, instruction->params[2], instruction->params[3],
                    instruction->params[4], instruction->params[5], instruction->params[6],
                    instruction->params[7]);
        break;
    case NGL_PATH_INSTRUCTION_VERB_CLOSE:
        nvgClosePath(desc->nvg_ctx);
        break;
    case NGL_PATH_INSTRUCTION_VERB_DONE:
        break;
    default:
        ngli_assert(0);
    };
}

static void drawpath2_draw(struct ngl_node *node)
{
    const struct drawpath2_opts *o = node->opts;
    struct drawpath2_priv *s = node->priv_data;
    struct ngl_ctx *ctx = node->ctx;
    struct pipeline_desc *desc = get_pipeline(node, &ctx->rendertarget_layout, &NGPU_GRAPHICS_STATE_DEFAULTS);

    if (!ctx->render_pass_started) {
        ngpu_ctx_begin_render_pass(ctx->gpu_ctx, ctx->current_rendertarget);
        ctx->render_pass_started = 1;
    }

    ngpu_ctx_set_viewport(ctx->gpu_ctx, &ctx->viewport);
    ngpu_ctx_set_scissor(ctx->gpu_ctx, &ctx->scissor);

    const float width = (float)ctx->viewport.width;
    const float height = (float)ctx->viewport.height;
    nvgBeginFrame(desc->nvg_ctx, width, height, 1);

    /* Scale from [0,viewport] to [-1,1], swapping y-axis */
    const float r2w = 2.0f / width;
    const float r2h = 2.0f / height;
    const NGLI_ALIGNED_MAT(remap_uv_to_centered_matrix) = {
        r2w, 0.f, 0.f, 0.f,
        0.f,-r2h, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
       -1.f, 1.f, 0.f, 1.f,
    };

    /* Scale from [-1,1] to [0,viewport], swapping y-axis */
    const float w_2 = width * 0.5f;
    const float h_2 = height * 0.5f;
    const NGLI_ALIGNED_MAT(remap_centered_to_uv_matrix) = {
        w_2, 0.f, 0.f, 0.f,
        0.f,-h_2, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        w_2, h_2, 0.f, 1.f,
    };

    const float *modelview_matrix = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const float *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    struct draw_info *draw_info = &s->draw_info;
    if (draw_info->compute_bounds) {
        draw_info->aabb = s->aabb;
        draw_info->viewport = ctx->viewport;

        NGLI_ALIGNED_MAT(transform_matrix) = NGLI_MAT4_IDENTITY;
        ngpu_ctx_transform_projection_matrix_inv(ctx->gpu_ctx, transform_matrix);
        ngli_mat4_mul(transform_matrix, transform_matrix, projection_matrix);
        ngli_mat4_mul(transform_matrix, transform_matrix, modelview_matrix);
        memcpy(draw_info->transform_matrix, transform_matrix, sizeof(transform_matrix));

        draw_info->screen_aabb = ngli_aabb_apply_projection(&draw_info->aabb, transform_matrix);
        draw_info->screen_obb_computed = 0;
    }
    NGLI_ALIGNED_MAT(matrix) = NGLI_MAT4_IDENTITY;
    ngli_mat4_mul(matrix, matrix, projection_matrix);
    ngli_mat4_mul(matrix, modelview_matrix, remap_uv_to_centered_matrix);
    ngli_mat4_mul(matrix, remap_centered_to_uv_matrix, matrix);

    nvgResetTransform(desc->nvg_ctx);
    nvgTransform(desc->nvg_ctx, matrix[0], matrix[1], matrix[4], matrix[5], matrix[12], matrix[13]);

    const enum NVGcompositeOperation op = get_nvg_blending(o->blending);
    if (op >= 0) {
        nvgGlobalCompositeOperation(desc->nvg_ctx, op);
    }

    nvgBeginPath(desc->nvg_ctx);

    for (size_t i = 0; i < s->instruction_count; i++) {
        const struct ngl_path_instruction *instruction = &o->instructions[i];
        handle_instruction(node, desc, instruction);
    }

    if (o->style == STYLE_FILL) {
        nvgFillColor(desc->nvg_ctx, nvgRGBAf(o->color[0], o->color[1], o->color[2], o->opacity));
        nvgFill(desc->nvg_ctx);
    } else if (o->style == STYLE_STROKE) {
        nvgStrokeColor(desc->nvg_ctx, nvgRGBAf(o->color[0], o->color[1], o->color[2], o->opacity));
        nvgStrokeWidth(desc->nvg_ctx, o->stroke_width);
        nvgStroke(desc->nvg_ctx);
    } else {
        ngli_assert(0);
    }

    nvgEndFrame(desc->nvg_ctx);
}

static void drawpath2_uninit(struct ngl_node *node)
{
    struct drawpath2_priv *s = node->priv_data;

    struct pipeline_desc *descs = ngli_darray_data(&s->pipeline_descs);
    for (size_t i = 0; i < ngli_darray_count(&s->pipeline_descs); i++) {
        struct pipeline_desc *desc = &descs[i];
        ngpu_nvgDelete(desc->nvg_ctx);
    }
    ngli_darray_reset(&s->pipeline_descs);
}

const struct node_class ngli_drawpath2_class = {
    .id = NGL_NODE_DRAWPATH2,
    .category = NGLI_NODE_CATEGORY_DRAW,
    .name = "DrawPath2",
    .init = drawpath2_init,
    .prepare = drawpath2_prepare,
    .draw = drawpath2_draw,
    .uninit = drawpath2_uninit,
    .priv_size = sizeof(struct drawpath2_priv),
    .opts_size = sizeof(struct drawpath2_opts),
    .params = drawpath2_params,
    .file = __FILE__,
};
