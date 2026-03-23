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
#include "internal.h"
#include <ngpu/ngpu.h>
#include "node_uniform.h"
#include "nopegl/nopegl.h"
#include "path.h"
#include "pipeline_compat.h"
#include <ngpu/ngpu.h>
#include "slug.h"
#include "utils/utils.h"

/* GLSL fragments as string */
#include "path_frag.h"
#include "path_vert.h"

struct uniform_map {
    int index;
    const void *data;
};

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

struct drawpath_priv {
    float vertices[4];
    float texcoord_bounds[4];
    float banding[4];
    int32_t glyph_data[4];
    float dist_scale;
    int32_t path_is_open;
    struct slug *slug;
    struct path *path;
    struct darray uniforms_map; // struct uniform_map
    struct darray uniforms; // struct ngpu_pgcraft_uniform
    struct ngpu_pgcraft *crafter;
    int modelview_matrix_index;
    int projection_matrix_index;
    int vertices_index;
    int texcoord_bounds_index;
    int banding_index;
    int glyph_index;
    int dist_scale_index;
    int path_is_open_index;
    struct darray pipeline_descs;
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

// TODO factor out with drawother and pass
static int build_uniforms_map(struct drawpath_priv *s)
{
    ngli_darray_init(&s->uniforms_map, sizeof(struct uniform_map), 0);

    const struct ngpu_pgcraft_uniform *uniforms = ngli_darray_data(&s->uniforms);
    for (size_t i = 0; i < ngli_darray_count(&s->uniforms); i++) {
        const struct ngpu_pgcraft_uniform *uniform = &uniforms[i];
        const int index = ngpu_pgcraft_get_uniform_index(s->crafter, uniform->name, uniform->stage);

        /* The following can happen if the driver makes optimisation (MESA is
         * typically able to optimize several passes of the same filter) */
        if (index < 0)
            continue;

        /* This skips unwanted uniforms such as modelview and projection which
         * are handled separately */
        if (!uniform->data)
            continue;

        const struct uniform_map map = {.index=index, .data=uniform->data};
        if (!ngli_darray_push(&s->uniforms_map, &map))
            return NGL_ERROR_MEMORY;
    }

    return 0;
}

static int drawpath_init(struct ngl_node *node)
{
    struct drawpath_priv *s = node->priv_data;
    const struct drawpath_opts *o = node->opts;

    ngli_darray_init(&s->pipeline_descs, sizeof(struct pipeline_desc), 0);

    s->slug = ngli_slug_create(node->ctx);
    if (!s->slug)
        return NGL_ERROR_MEMORY;

    int ret = ngli_slug_init(s->slug);
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
     * Build a matrix to transform the path from viewbox coordinates into
     * resolution-scaled coordinates. This preserves backward compatibility
     * with the effect parameter calibration (outline, glow, blur).
     */
    const float res = (float)o->pt_size * (float)o->dpi / 72.f;
    const struct ngli_box vb = {NGLI_ARG_VEC4(o->viewbox)};
    const NGLI_ALIGNED_MAT(path_transform) = {
        res/vb.w, 0.f, 0.f, 0.f,
        0.f, res/vb.h, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        -vb.x/vb.w*res, -vb.y/vb.h*res, 0.f, 1.f,
    };
    ngli_path_transform(s->path, path_transform);

    ret = ngli_path_finalize(s->path);
    if (ret < 0)
        return ret;

    /* Detect whether the path is open (no closing segments) */
    const struct darray *segments = ngli_path_get_segments(s->path);
    const struct path_segment *segs = ngli_darray_data(segments);
    s->path_is_open = 1;
    for (size_t i = 0; i < ngli_darray_count(segments); i++) {
        if (segs[i].flags & NGLI_PATH_SEGMENT_FLAG_CLOSING) {
            s->path_is_open = 0;
            break;
        }
    }

    int shape_id = ngli_slug_add_shape(s->slug, s->path, 0);
    if (shape_id < 0)
        return shape_id;

    ret = ngli_slug_finalize(s->slug);
    if (ret < 0)
        return ret;

    struct slug_glyph_data gd;
    ngli_slug_get_glyph_data(s->slug, shape_id, &gd);

    /*
     * Padding around the shape quad to allow effects (outline, glow, blur)
     * to render beyond the tight shape bounding box. Uses the same approach
     * as text rendering: 80% of the max em-space dimension.
     */
    #define EFFECT_PAD_RATIO 0.8f
    const float *eb = gd.em_bounds;
    const float em_w = eb[2] - eb[0];
    const float em_h = eb[3] - eb[1];
    const float effect_pad = EFFECT_PAD_RATIO * NGLI_MAX(em_w, em_h);

    /* Geometry: map padded em-bounds to display box */
    const struct ngli_box box = {NGLI_ARG_VEC4(o->box)};
    const float padded_em_w = em_w + 2.f * effect_pad;
    const float padded_em_h = em_h + 2.f * effect_pad;
    const float nw = padded_em_w > 0.f ? box.w * padded_em_w / res : box.w;
    const float nh = padded_em_h > 0.f ? box.h * padded_em_h / res : box.h;
    const float bx = box.x + (box.w - nw) / 2.f;
    const float by = box.y + (box.h - nh) / 2.f;

    s->vertices[0] = bx;
    s->vertices[1] = by;
    s->vertices[2] = bx + nw;
    s->vertices[3] = by + nh;

    /* Texcoord bounds: em-bounds dilated by effect padding (same as text) */
    s->texcoord_bounds[0] = eb[0] - effect_pad;
    s->texcoord_bounds[1] = eb[1] - effect_pad;
    s->texcoord_bounds[2] = eb[2] + effect_pad;
    s->texcoord_bounds[3] = eb[3] + effect_pad;

    memcpy(s->banding, gd.band_transform, sizeof(s->banding));

    s->glyph_data[0] = gd.glyph_loc[0];
    s->glyph_data[1] = gd.glyph_loc[1];
    s->glyph_data[2] = gd.band_max[0];
    s->glyph_data[3] = gd.band_max[1];

    s->dist_scale = 1.f / res;

    const struct ngpu_pgcraft_uniform uniforms[] = {
        {.name="modelview_matrix",  .type=NGPU_TYPE_MAT4,  .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="projection_matrix", .type=NGPU_TYPE_MAT4,  .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="vertices",          .type=NGPU_TYPE_VEC4,  .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="texcoord_bounds",   .type=NGPU_TYPE_VEC4,  .stage=NGPU_PROGRAM_STAGE_VERT},

        {.name="banding",           .type=NGPU_TYPE_VEC4,  .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="glyph",             .type=NGPU_TYPE_IVEC4, .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="dist_scale",        .type=NGPU_TYPE_F32,   .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="path_is_open",      .type=NGPU_TYPE_BOOL,  .stage=NGPU_PROGRAM_STAGE_FRAG},

        {.name="color",             .type=NGPU_TYPE_VEC3,  .stage=NGPU_PROGRAM_STAGE_FRAG, .data=ngli_node_get_data_ptr(o->color_node, o->color)},
        {.name="opacity",           .type=NGPU_TYPE_F32,   .stage=NGPU_PROGRAM_STAGE_FRAG, .data=ngli_node_get_data_ptr(o->opacity_node, &o->opacity)},
        {.name="outline",           .type=NGPU_TYPE_F32,   .stage=NGPU_PROGRAM_STAGE_FRAG, .data=ngli_node_get_data_ptr(o->outline_node, &o->outline)},
        {.name="outline_color",     .type=NGPU_TYPE_VEC3,  .stage=NGPU_PROGRAM_STAGE_FRAG, .data=ngli_node_get_data_ptr(o->outline_color_node, &o->outline_color)},
        {.name="glow",              .type=NGPU_TYPE_F32,   .stage=NGPU_PROGRAM_STAGE_FRAG, .data=ngli_node_get_data_ptr(o->glow_node, &o->glow)},
        {.name="glow_color",        .type=NGPU_TYPE_VEC3,  .stage=NGPU_PROGRAM_STAGE_FRAG, .data=ngli_node_get_data_ptr(o->glow_color_node, o->glow_color)},
        {.name="blur",              .type=NGPU_TYPE_F32,   .stage=NGPU_PROGRAM_STAGE_FRAG, .data=ngli_node_get_data_ptr(o->blur_node, &o->blur)},
        {.name="outline_pos",       .type=NGPU_TYPE_F32,   .stage=NGPU_PROGRAM_STAGE_FRAG, .data=ngli_node_get_data_ptr(o->outline_pos_node, &o->outline_pos)},
    };

    /* register source uniforms */
    ngli_darray_init(&s->uniforms, sizeof(struct ngpu_pgcraft_uniform), 0);
    for (size_t i = 0; i < NGLI_ARRAY_NB(uniforms); i++)
        if (!ngli_darray_push(&s->uniforms, &uniforms[i]))
            return NGL_ERROR_MEMORY;

    const struct ngpu_pgcraft_texture textures[] = {
        {
            .name = "curve_tex",
            .type = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .stage = NGPU_PROGRAM_STAGE_FRAG,
            .texture = ngli_slug_get_curve_texture(s->slug),
        },
        {
            .name = "band_tex",
            .type = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .stage = NGPU_PROGRAM_STAGE_FRAG,
            .texture = ngli_slug_get_band_texture(s->slug),
        },
    };

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "texcoord", .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/path",
        .vert_base        = path_vert,
        .frag_base        = path_frag,
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .uniforms         = ngli_darray_data(&s->uniforms),
        .nb_uniforms      = ngli_darray_count(&s->uniforms),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    struct ngl_ctx *ctx = node->ctx;
    s->crafter = ngpu_pgcraft_create(ctx->gpu_ctx);
    if (!s->crafter)
        return NGL_ERROR_MEMORY;

    ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    if (ret < 0)
        return ret;

    s->modelview_matrix_index  = ngpu_pgcraft_get_uniform_index(s->crafter, "modelview_matrix", NGPU_PROGRAM_STAGE_VERT);
    s->projection_matrix_index = ngpu_pgcraft_get_uniform_index(s->crafter, "projection_matrix", NGPU_PROGRAM_STAGE_VERT);
    s->vertices_index          = ngpu_pgcraft_get_uniform_index(s->crafter, "vertices", NGPU_PROGRAM_STAGE_VERT);
    s->texcoord_bounds_index   = ngpu_pgcraft_get_uniform_index(s->crafter, "texcoord_bounds", NGPU_PROGRAM_STAGE_VERT);
    s->banding_index           = ngpu_pgcraft_get_uniform_index(s->crafter, "banding", NGPU_PROGRAM_STAGE_FRAG);
    s->glyph_index             = ngpu_pgcraft_get_uniform_index(s->crafter, "glyph", NGPU_PROGRAM_STAGE_FRAG);
    s->dist_scale_index        = ngpu_pgcraft_get_uniform_index(s->crafter, "dist_scale", NGPU_PROGRAM_STAGE_FRAG);
    s->path_is_open_index      = ngpu_pgcraft_get_uniform_index(s->crafter, "path_is_open", NGPU_PROGRAM_STAGE_FRAG);

    ret = build_uniforms_map(s);
    if (ret < 0)
        return ret;

    return 0;
}

static int drawpath_prepare(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct drawpath_priv *s = node->priv_data;
    struct rnode *rnode = node->ctx->rnode_pos;

    struct pipeline_desc *desc = ngli_darray_push(&s->pipeline_descs, NULL);
    if (!desc)
        return NGL_ERROR_MEMORY;
    rnode->id = ngli_darray_count(&s->pipeline_descs) - 1;

    struct ngpu_graphics_state state = rnode->graphics_state;
    int ret = ngli_blending_apply_preset(&state, NGLI_BLENDING_SRC_OVER);
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
            .rt_layout    = rnode->rendertarget_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(s->crafter),
        },
        .program          = ngpu_pgcraft_get_program(s->crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(s->crafter),
        .resources        = ngpu_pgcraft_get_bindgroup_resources(s->crafter),
        .vertex_resources = ngpu_pgcraft_get_vertex_resources(s->crafter),
        .compat_info      = ngpu_pgcraft_get_compat_info(s->crafter),
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
    struct pipeline_desc *descs = ngli_darray_data(&s->pipeline_descs);
    struct pipeline_desc *desc = &descs[ctx->rnode_pos->id];
    struct pipeline_compat *pl_compat = desc->pipeline_compat;

    const float *modelview_matrix  = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const float *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    ngli_pipeline_compat_update_uniform(pl_compat, s->modelview_matrix_index, modelview_matrix);
    ngli_pipeline_compat_update_uniform(pl_compat, s->projection_matrix_index, projection_matrix);
    ngli_pipeline_compat_update_uniform(pl_compat, s->vertices_index, s->vertices);
    ngli_pipeline_compat_update_uniform(pl_compat, s->texcoord_bounds_index, s->texcoord_bounds);
    ngli_pipeline_compat_update_uniform(pl_compat, s->banding_index, s->banding);
    ngli_pipeline_compat_update_uniform(pl_compat, s->glyph_index, s->glyph_data);
    ngli_pipeline_compat_update_uniform(pl_compat, s->dist_scale_index, &s->dist_scale);
    ngli_pipeline_compat_update_uniform(pl_compat, s->path_is_open_index, &s->path_is_open);

    const struct uniform_map *map = ngli_darray_data(&s->uniforms_map);
    for (size_t i = 0; i < ngli_darray_count(&s->uniforms_map); i++)
        ngli_pipeline_compat_update_uniform(pl_compat, map[i].index, map[i].data);

    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    if (!ngpu_ctx_is_render_pass_active(gpu_ctx)) {
        ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);
    }

    ngpu_ctx_set_viewport(gpu_ctx, &ctx->viewport);
    ngpu_ctx_set_scissor(gpu_ctx, &ctx->scissor);

    ngli_pipeline_compat_draw(pl_compat, 4, 1, 0);
}

static void drawpath_uninit(struct ngl_node *node)
{
    struct drawpath_priv *s = node->priv_data;
    struct pipeline_desc *descs = ngli_darray_data(&s->pipeline_descs);
    for (size_t i = 0; i < ngli_darray_count(&s->pipeline_descs); i++) {
        struct pipeline_desc *desc = &descs[i];
        ngli_pipeline_compat_freep(&desc->pipeline_compat);
    }
    ngli_darray_reset(&s->uniforms);
    ngli_darray_reset(&s->uniforms_map);
    ngpu_pgcraft_freep(&s->crafter);
    ngli_slug_freep(&s->slug);
    ngli_path_freep(&s->path);
    ngli_darray_reset(&s->pipeline_descs);
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
