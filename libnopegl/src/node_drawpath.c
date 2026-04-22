/*
 * Copyright 2023 Nope Forge
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
#include <math.h>

#include "blending.h"
#include "box.h"
#include "distmap.h"
#include "internal.h"
#include <ngpu/ngpu.h>
#include "node_uniform.h"
#include "nopegl/nopegl.h"
#include "path.h"
#include "pipeline_compat.h"
#include <ngpu/ngpu.h>
#include "utils/utils.h"

/* GLSL fragments as string */
#include "path_frag.h"
#include "path_vert.h"

struct pipeline_desc {
    struct pipeline_compat *pipeline_compat;
};

struct drawpath_opts {
    struct ngl_node *path_node;
    float box[4];
    float viewbox[4];
    int32_t pt_size;
    int32_t dpi;
    struct ngl_node *transform_chain;
    struct ngl_node *color_node;
    float color[3];
    struct ngl_node *opacity_node;
    float opacity;
    struct ngl_node *outline_node;
    float outline;
    struct ngl_node *outline_color_node;
    float outline_color[3];
    struct ngl_node *outline_pos_node;
    float outline_pos;
    struct ngl_node *glow_node;
    float glow;
    struct ngl_node *glow_color_node;
    float glow_color[3];
    struct ngl_node *blur_node;
    float blur;
};

struct drawpath_vert_block {
    struct ngli_mat4 modelview_matrix;
    struct ngli_mat4 projection_matrix;
    float vertices[4];
};

struct drawpath_frag_block {
    float coords[4];
    float color[3];
    float opacity;
    float outline_color[3];
    float outline;
    float glow_color[3];
    float glow;
    float blur;
    float outline_pos;
    int32_t debug;
    float _pad[1];
};

struct drawpath_priv {
    struct ngli_aabb atlas_coords;
    float vertices[4];
    struct distmap *distmap;
    struct path *path;
    struct ngpu_pgcraft *crafter;
    struct ngpu_block_desc vert_block_desc;
    struct ngpu_block_desc frag_block_desc;
    int32_t vert_block_index;
    int32_t frag_block_index;
    struct pipeline_desc pipeline_desc;
};

#define OFFSET(x) offsetof(struct drawpath_opts, x)
static const struct node_param drawpath_params[] = {
    {"path",         NGLI_PARAM_TYPE_NODE, OFFSET(path_node),
                     .node_types=(const uint32_t[]){NGL_NODE_PATH, NGL_NODE_SMOOTHPATH, NGLI_NODE_NONE},
                     .flags=NGLI_PARAM_FLAG_NON_NULL,
                     .desc=NGLI_DOCSTRING("path to draw")},
    {"box",          NGLI_PARAM_TYPE_VEC4, OFFSET(box), {.vec={-1.f, -1.f, 2.f, 2.f}},
                     .desc=NGLI_DOCSTRING("geometry box relative to screen (x, y, width, height)")},
    {"viewbox",      NGLI_PARAM_TYPE_VEC4, OFFSET(viewbox), {.vec={-1.f, -1.f, 2.f, 2.f}},
                     .desc=NGLI_DOCSTRING("vector space for interpreting the path (x, y, width, height)")},
    {"pt_size",      NGLI_PARAM_TYPE_I32, OFFSET(pt_size), {.i32=54},
                     .desc=NGLI_DOCSTRING("size in point (nominal size, 1pt = 1/72 inch)")},
    {"dpi",          NGLI_PARAM_TYPE_I32, OFFSET(dpi), {.i32=300},
                     .desc=NGLI_DOCSTRING("resolution (dot per inch)")},
    {"color",        NGLI_PARAM_TYPE_VEC3, OFFSET(color_node), {.vec={1.f, 1.f, 1.f}},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                     .desc=NGLI_DOCSTRING("path fill color")},
    {"opacity",      NGLI_PARAM_TYPE_F32, OFFSET(opacity_node), {.f32=1.f},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                     .desc=NGLI_DOCSTRING("path fill opacity")},
    {"outline",      NGLI_PARAM_TYPE_F32, OFFSET(outline_node), {.f32=.02f},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                     .desc=NGLI_DOCSTRING("path outline width")},
    {"outline_color", NGLI_PARAM_TYPE_VEC3, OFFSET(outline_color_node), {.vec={1.f, .7f, 0.f}},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                     .desc=NGLI_DOCSTRING("path outline color")},
    {"outline_pos",  NGLI_PARAM_TYPE_F32, OFFSET(outline_pos_node), {.f32=.5f},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                     .desc=NGLI_DOCSTRING("path outline position (0 for inside, 0.5 right at the edge, 1 for outside)")},
    {"glow",         NGLI_PARAM_TYPE_F32, OFFSET(glow_node),
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                     .desc=NGLI_DOCSTRING("path glow width")},
    {"glow_color",   NGLI_PARAM_TYPE_VEC3, OFFSET(glow_color_node), {.vec={1.f, 1.f, 1.f}},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                     .desc=NGLI_DOCSTRING("path glow color")},
    {"blur",         NGLI_PARAM_TYPE_F32, OFFSET(blur_node),
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                     .desc=NGLI_DOCSTRING("path blur")},
    {NULL}
};

static int drawpath_init(struct ngl_node *node)
{
    struct drawpath_priv *s = node->priv_data;
    const struct drawpath_opts *o = node->opts;

    s->distmap = ngli_distmap_create(node->ctx);
    if (!s->distmap)
        return NGL_ERROR_MEMORY;

    int ret = ngli_distmap_init(s->distmap, o->pt_size, o->dpi);
    if (ret < 0)
        return ret;

    const struct path *src_path = *(struct path **)o->path_node->priv_data;

    s->path = ngli_path_create();
    if (!s->path)
        return NGL_ERROR_MEMORY;

    ret = ngli_path_add_path(s->path, src_path);
    if (ret < 0)
        return ret;

    /*
     * Build a matrix to transform path into normalized coordinates, scaled up
     * to the desired resolution.
     */
    const float res = (float)o->pt_size * (float)o->dpi / 72.f;
    const struct ngli_box vb = {NGLI_ARG_VEC4(o->viewbox)};
    const struct ngli_mat4 path_transform = {.m = {
        res/vb.w, 0.f, 0.f, 0.f,
        0.f, res/vb.h, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        -vb.x/vb.w*res, -vb.y/vb.h*res, 0.f, 1.f,
    }};
    ngli_path_transform(s->path, path_transform.m);

    ret = ngli_path_finalize(s->path);
    if (ret < 0)
        return ret;

    const struct ngl_scene_params *params = &node->scene->params;
    const float ar = params->height ? (float)params->width / (float)params->height : 1.f;
    const int32_t shape_w = (int32_t)lrintf(ar > 1.f ? res * ar : res);
    const int32_t shape_h = (int32_t)lrintf(ar > 1.f ? res : res / ar);

    int32_t shape_id;
    ret = ngli_distmap_add_shape(s->distmap, shape_w, shape_h, s->path, 0, &shape_id);
    if (ret< 0)
        return ret;

    ret = ngli_distmap_finalize(s->distmap);
    if (ret < 0)
        return ret;

    s->atlas_coords = ngli_distmap_get_shape_coords(s->distmap, shape_id);

    float scale[2];
    ngli_distmap_get_shape_scale(s->distmap, shape_id, scale);

    /* Geometry scale up */
    const struct ngli_box box = {NGLI_ARG_VEC4(o->box)};
    const float nw = box.w * scale[0];
    const float nh = box.h * scale[1];
    const float bx = box.x + (box.w - nw) / 2.f;
    const float by = box.y + (box.h - nh) / 2.f;
    const float vertices[] = {bx, by, bx + nw, by + nh};
    memcpy(s->vertices, vertices, sizeof(s->vertices));

    struct ngpu_ctx *gpu_ctx = node->ctx->gpu_ctx;

    /* Initialize vertex block descriptor */
    ngpu_block_desc_init(gpu_ctx, &s->vert_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_add_field(&s->vert_block_desc, "modelview_matrix", NGPU_TYPE_MAT4, 0);
    ngpu_block_desc_add_field(&s->vert_block_desc, "projection_matrix", NGPU_TYPE_MAT4, 0);
    ngpu_block_desc_add_field(&s->vert_block_desc, "vertices", NGPU_TYPE_VEC4, 0);

    /* Initialize fragment block descriptor */
    ngpu_block_desc_init(gpu_ctx, &s->frag_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_add_field(&s->frag_block_desc, "coords", NGPU_TYPE_VEC4, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "color", NGPU_TYPE_VEC3, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "opacity", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "outline_color", NGPU_TYPE_VEC3, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "outline", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "glow_color", NGPU_TYPE_VEC3, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "glow", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "blur", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "outline_pos", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "debug", NGPU_TYPE_BOOL, 0);

    return 0;
}

static int drawpath_prepare(struct ngl_node *node,
                            const struct ngpu_graphics_state *graphics_state,
                            const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct ngpu_ctx *gpu_ctx = node->ctx->gpu_ctx;
    struct drawpath_priv *s = node->priv_data;

    struct ngpu_texture *texture = ngli_distmap_get_texture(s->distmap);
    const struct ngpu_pgcraft_texture textures[] = {
        {
            .name = "tex",
            .type = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .stage = NGPU_PROGRAM_STAGE_FRAG,
            .texture = texture,
            .no_metadata = true,
        },
    };

    const size_t vert_size = ngpu_block_desc_get_size(&s->vert_block_desc, 0);
    const size_t frag_size = ngpu_block_desc_get_size(&s->frag_block_desc, 0);
    ngli_assert(vert_size == sizeof(struct drawpath_vert_block));
    ngli_assert(frag_size == sizeof(struct drawpath_frag_block));

    const struct ngpu_pgcraft_block blocks[] = {
        {
            .name          = "vert_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_VERT,
            .block         = &s->vert_block_desc,
        },
        {
            .name          = "frag_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &s->frag_block_desc,
        },
    };

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "uv", .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/path",
        .vert_base        = path_vert,
        .frag_base        = path_frag,
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .blocks           = blocks,
        .nb_blocks        = NGLI_ARRAY_NB(blocks),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter)
        return NGL_ERROR_MEMORY;

    int ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    if (ret < 0)
        return ret;

    s->vert_block_index = ngpu_pgcraft_get_block_index(s->crafter, "vert_params", NGPU_PROGRAM_STAGE_VERT);
    s->frag_block_index = ngpu_pgcraft_get_block_index(s->crafter, "frag_params", NGPU_PROGRAM_STAGE_FRAG);

    struct pipeline_desc *desc = &s->pipeline_desc;

    struct ngpu_graphics_state state = *graphics_state;
    ret = ngli_blending_apply_preset(&state, NGLI_BLENDING_SRC_OVER);
    if (ret < 0)
        return ret;

    desc->pipeline_compat = ngli_pipeline_compat_create(gpu_ctx);
    if (!desc->pipeline_compat)
        return NGL_ERROR_MEMORY;

    const struct pipeline_compat_params params = {
        .type = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .state        = state,
            .rt_layout    = *rendertarget_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(s->crafter),
        },
        .program          = ngpu_pgcraft_get_program(s->crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(s->crafter),
        .resources        = ngpu_pgcraft_get_bindgroup_resources(s->crafter),
        .vertex_resources = ngpu_pgcraft_get_vertex_resources(s->crafter),
        .texture_infos    = ngpu_pgcraft_get_texture_infos(s->crafter),
    };

    ret = ngli_pipeline_compat_init(desc->pipeline_compat, &params);
    if (ret < 0)
        return ret;

    return 0;
}

static void drawpath_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct drawpath_priv *s = node->priv_data;
    const struct drawpath_opts *o = node->opts;
    struct pipeline_desc *desc = &s->pipeline_desc;

    const struct ngli_mat4 *modelview_matrix  = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const struct ngli_mat4 *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    /* Fill and push vertex block to staging buffer */
    struct drawpath_vert_block vert_data;
    vert_data.modelview_matrix = *modelview_matrix;
    vert_data.projection_matrix = *projection_matrix;
    memcpy(vert_data.vertices, s->vertices, sizeof(vert_data.vertices));

    if (s->vert_block_index >= 0) {
        const size_t vert_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &vert_data, sizeof(vert_data));
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(desc->pipeline_compat, s->vert_block_index,
                                           staging_buf, vert_offset, sizeof(vert_data));
    }

    /* Fill and push fragment block to staging buffer */
    const float *color_ptr         = ngli_node_get_data_ptr(o->color_node, o->color);
    const float *opacity_ptr       = ngli_node_get_data_ptr(o->opacity_node, &o->opacity);
    const float *outline_ptr       = ngli_node_get_data_ptr(o->outline_node, &o->outline);
    const float *outline_color_ptr = ngli_node_get_data_ptr(o->outline_color_node, o->outline_color);
    const float *outline_pos_ptr   = ngli_node_get_data_ptr(o->outline_pos_node, &o->outline_pos);
    const float *glow_ptr          = ngli_node_get_data_ptr(o->glow_node, &o->glow);
    const float *glow_color_ptr    = ngli_node_get_data_ptr(o->glow_color_node, o->glow_color);
    const float *blur_ptr          = ngli_node_get_data_ptr(o->blur_node, &o->blur);

    struct drawpath_frag_block frag_data = {0};
    memcpy(frag_data.coords, &s->atlas_coords, sizeof(frag_data.coords));
    memcpy(frag_data.color, color_ptr, sizeof(frag_data.color));
    frag_data.opacity = *opacity_ptr;
    memcpy(frag_data.outline_color, outline_color_ptr, sizeof(frag_data.outline_color));
    frag_data.outline = *outline_ptr;
    memcpy(frag_data.glow_color, glow_color_ptr, sizeof(frag_data.glow_color));
    frag_data.glow = *glow_ptr;
    frag_data.blur = *blur_ptr;
    frag_data.outline_pos = *outline_pos_ptr;
    frag_data.debug = 0;

    if (s->frag_block_index >= 0) {
        const size_t frag_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &frag_data, sizeof(frag_data));
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(desc->pipeline_compat, s->frag_block_index,
                                           staging_buf, frag_offset, sizeof(frag_data));
    }

    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    if (!ngpu_ctx_is_render_pass_active(gpu_ctx)) {
        ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);
    }

    ngpu_ctx_set_viewport(gpu_ctx, &ctx->viewport);
    ngpu_ctx_set_scissor(gpu_ctx, &ctx->scissor);

    ngli_pipeline_compat_draw(desc->pipeline_compat, 4, 1, 0);
}

static void drawpath_uninit(struct ngl_node *node)
{
    struct drawpath_priv *s = node->priv_data;
    ngli_pipeline_compat_freep(&s->pipeline_desc.pipeline_compat);
    ngpu_pgcraft_freep(&s->crafter);
    ngpu_block_desc_reset(&s->vert_block_desc);
    ngpu_block_desc_reset(&s->frag_block_desc);
    ngli_distmap_freep(&s->distmap);
    ngli_path_freep(&s->path);
}

const struct node_class ngli_drawpath_class = {
    .id        = NGL_NODE_DRAWPATH,
    .name      = "DrawPath",
    .init      = drawpath_init,
    .prepare   = drawpath_prepare,
    .update    = ngli_node_update_children,
    .draw      = drawpath_draw,
    .uninit    = drawpath_uninit,
    .opts_size = sizeof(struct drawpath_opts),
    .priv_size = sizeof(struct drawpath_priv),
    .params    = drawpath_params,
    .file      = __FILE__,
};
