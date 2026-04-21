/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023 Clément Bœsch <u pkh.me>
 * Copyright 2019-2022 GoPro Inc.
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

#include "box.h"
#include "internal.h"
#include <ngpu/ngpu.h>
#include <ngpu/ngpu.h>
#include "params.h"
#include "pipeline_compat.h"
#include "text.h"
#include "utils/darray.h"
#include "utils/utils.h"

/* GLSL fragments as string */
#include "text_bg_frag.h"
#include "text_bg_vert.h"
#include "text_chars_frag.h"
#include "text_chars_vert.h"
#include "text_slug_frag.h"
#include "text_slug_vert.h"

#define VERTEX_USAGE_FLAGS (NGPU_BUFFER_USAGE_TRANSFER_DST_BIT | \
                            NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT) \

#define INDEX_USAGE_FLAGS (NGLI_BUFFER_USAGE_TRANSFER_DST_BIT | \
                           NGLI_BUFFER_USAGE_INDEX_BUFFER_BIT)  \

#define DYNAMIC_VERTEX_USAGE_FLAGS (NGPU_BUFFER_USAGE_DYNAMIC_BIT | VERTEX_USAGE_FLAGS)
#define DYNAMIC_INDEX_USAGE_FLAGS  (NGLI_BUFFER_USAGE_DYNAMIC_BIT | INDEX_USAGE_FLAGS)

struct pipeline_desc_common {
    struct ngpu_pgcraft *crafter;
    struct pipeline_compat *pipeline_compat;
};

struct pipeline_desc_bg {
    struct pipeline_desc_common common;
    struct ngpu_block_desc vert_block_desc;
    struct ngpu_block_desc frag_block_desc;
    int32_t vert_block_index;
    int32_t frag_block_index;
};

struct pipeline_desc_fg {
    struct pipeline_desc_common common;
    struct ngpu_block_desc vert_block_desc;
    struct ngpu_block_desc frag_block_desc;
    int32_t vert_block_index;
    int32_t frag_block_index;
    int32_t vertices_index;
    int32_t atlas_coords_index;
    int32_t texcoord_bounds_index;
    int32_t banding_index;
    int32_t glyph_data_index;
    int32_t user_transform_index;
    int32_t color_index;
    int32_t outline_index;
    int32_t glow_index;
    int32_t blur_index;
    int32_t outline_pos_index;
};

struct pipeline_desc {
    struct pipeline_desc_bg bg; /* Background (bounding box) */
    struct pipeline_desc_fg fg; /* Foreground (characters) */
};

struct text_bg_vert_block {
    struct ngli_mat4 modelview_matrix;
    struct ngli_mat4 projection_matrix;
};

struct text_bg_frag_block {
    float color[3];
    float opacity;
};

struct text_fg_vert_block {
    struct ngli_mat4 modelview_matrix;
    struct ngli_mat4 projection_matrix;
};

struct text_fg_frag_block {
    float dist_scale;
    float _pad[3];
};

struct text_opts {
    struct livectl live;
    float fg_color[3];
    float fg_opacity;
    float bg_color[3];
    float bg_opacity;
    float box[4];
    struct ngl_node **font_faces;
    size_t nb_font_faces;
    int32_t padding;
    int32_t pt_size;
    int32_t dpi;
    float font_scale;
    enum text_scale_mode scale_mode;
    struct ngl_node **effect_nodes;
    size_t nb_effect_nodes;
    enum text_valign valign;
    enum text_halign halign;
    enum writing_mode writing_mode;
};

struct text_priv {
    /* characters */
    struct text *text_ctx;
    struct ngpu_buffer *vertices;
    struct ngpu_buffer *atlas_coords;
    struct ngpu_buffer *texcoord_bounds;
    struct ngpu_buffer *band_transforms;
    struct ngpu_buffer *glyph_data;
    struct ngpu_buffer *user_transforms;
    struct ngpu_buffer *colors;
    struct ngpu_buffer *outlines;
    struct ngpu_buffer *glows;
    struct ngpu_buffer *blurs;
    struct ngpu_buffer *outline_positions;
    size_t nb_chars;

    float dist_scale;

    /* background box */
    struct ngpu_buffer *bg_vertices;

    struct pipeline_desc pipeline_desc;
    int live_changed;
    struct ngpu_viewport viewport;
};

static const struct param_choices valign_choices = {
    .name = "valign",
    .consts = {
        {"center", NGLI_TEXT_VALIGN_CENTER, .desc=NGLI_DOCSTRING("vertically centered")},
        {"bottom", NGLI_TEXT_VALIGN_BOTTOM, .desc=NGLI_DOCSTRING("bottom positioned")},
        {"top",    NGLI_TEXT_VALIGN_TOP,    .desc=NGLI_DOCSTRING("top positioned")},
        {NULL}
    }
};

static const struct param_choices halign_choices = {
    .name = "halign",
    .consts = {
        {"center", NGLI_TEXT_HALIGN_CENTER, .desc=NGLI_DOCSTRING("horizontally centered")},
        {"right",  NGLI_TEXT_HALIGN_RIGHT,  .desc=NGLI_DOCSTRING("right positioned")},
        {"left",   NGLI_TEXT_HALIGN_LEFT,   .desc=NGLI_DOCSTRING("left positioned")},
        {NULL}
    }
};

static const struct param_choices writing_mode_choices = {
    .name = "writing_mode",
    .consts = {
        {"horizontal-tb", NGLI_TEXT_WRITING_MODE_HORIZONTAL_TB,
                          .desc=NGLI_DOCSTRING("left-to-right flow then top-to-bottom per line")},
        {"vertical-rl",   NGLI_TEXT_WRITING_MODE_VERTICAL_RL,
                          .desc=NGLI_DOCSTRING("top-to-bottom flow then right-to-left per line")},
        {"vertical-lr",   NGLI_TEXT_WRITING_MODE_VERTICAL_LR,
                          .desc=NGLI_DOCSTRING("top-to-bottom flow then left-to-right per line")},
        {NULL}
    }
};

static const struct param_choices scale_mode_choices = {
    .name = "scale_mode",
    .consts = {
        {"auto",  NGLI_TEXT_SCALE_MODE_AUTO,  .desc=NGLI_DOCSTRING("automatic size by fitting the specified bounding box")},
        {"fixed", NGLI_TEXT_SCALE_MODE_FIXED, .desc=NGLI_DOCSTRING("fixed character size (bounding box ignored for scaling)")},
        {NULL}
    }
};

static int set_live_changed(struct ngl_node *node)
{
    struct text_priv *s = node->priv_data;
    s->live_changed = 1;
    return 0;
}

#define OFFSET(x) offsetof(struct text_opts, x)
static const struct node_param text_params[] = {
    {"text",         NGLI_PARAM_TYPE_STR, OFFSET(live.val.s), {.str=""},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_NON_NULL,
                     .update_func=set_live_changed,
                     .desc=NGLI_DOCSTRING("text string to rasterize")},
    {"live_id",      NGLI_PARAM_TYPE_STR, OFFSET(live.id),
                     .desc=NGLI_DOCSTRING("live control identifier")},
    {"fg_color",     NGLI_PARAM_TYPE_VEC3, OFFSET(fg_color), {.vec={1.f, 1.f, 1.f}},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .update_func=set_live_changed,
                     .desc=NGLI_DOCSTRING("foreground text color")},
    {"fg_opacity",   NGLI_PARAM_TYPE_F32, OFFSET(fg_opacity), {.f32=1.f},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .update_func=set_live_changed,
                     .desc=NGLI_DOCSTRING("foreground text opacity")},
    {"bg_color",     NGLI_PARAM_TYPE_VEC3, OFFSET(bg_color), {.vec={0.f, 0.f, 0.f}},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .desc=NGLI_DOCSTRING("background text color")},
    {"bg_opacity",   NGLI_PARAM_TYPE_F32, OFFSET(bg_opacity), {.f32=.8f},
                     .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .desc=NGLI_DOCSTRING("background text opacity")},
    {"box",          NGLI_PARAM_TYPE_VEC4, OFFSET(box), {.vec={-1.f, -1.f, 2.f, 2.f}},
                     .desc=NGLI_DOCSTRING("geometry box relative to screen (x, y, width, height)")},
    {"font_faces",   NGLI_PARAM_TYPE_NODELIST, OFFSET(font_faces),
                     .node_types=(const uint32_t[]){NGL_NODE_FONTFACE, NGLI_NODE_NONE},
                     .desc=NGLI_DOCSTRING("font faces in order of preferences (require build with external text libraries)")},
    {"padding",      NGLI_PARAM_TYPE_I32, OFFSET(padding), {.i32=4},
                     .desc=NGLI_DOCSTRING("padding around the text, in point units")},
    {"pt_size",      NGLI_PARAM_TYPE_I32, OFFSET(pt_size), {.i32=54},
                     .desc=NGLI_DOCSTRING("characters size in point (nominal size, 1pt = 1/72 inch)")},
    {"dpi",          NGLI_PARAM_TYPE_I32, OFFSET(dpi), {.i32=96},
                     .desc=NGLI_DOCSTRING("resolution (dot per inch)")},
    {"font_scale",   NGLI_PARAM_TYPE_F32, OFFSET(font_scale), {.f32=1.f},
                     .desc=NGLI_DOCSTRING("scaling of the font")},
    {"scale_mode",   NGLI_PARAM_TYPE_SELECT, OFFSET(scale_mode), {.i32=NGLI_TEXT_SCALE_MODE_AUTO},
                     .choices=&scale_mode_choices,
                     .desc=NGLI_DOCSTRING("scaling behaviour for the characters")},
    {"effects",      NGLI_PARAM_TYPE_NODELIST, OFFSET(effect_nodes),
                     .node_types=(const uint32_t[]){NGL_NODE_TEXTEFFECT, NGLI_NODE_NONE},
                     .desc=NGLI_DOCSTRING("stack of effects")},
    {"valign",       NGLI_PARAM_TYPE_SELECT, OFFSET(valign), {.i32=NGLI_TEXT_VALIGN_CENTER},
                     .choices=&valign_choices,
                     .desc=NGLI_DOCSTRING("vertical alignment of the text in the box")},
    {"halign",       NGLI_PARAM_TYPE_SELECT, OFFSET(halign), {.i32=NGLI_TEXT_HALIGN_CENTER},
                     .choices=&halign_choices,
                     .desc=NGLI_DOCSTRING("horizontal alignment of the text in the box")},
    {"writing_mode", NGLI_PARAM_TYPE_SELECT, OFFSET(writing_mode), {.i32=NGLI_TEXT_WRITING_MODE_HORIZONTAL_TB},
                     .choices=&writing_mode_choices,
                     .desc=NGLI_DOCSTRING("direction flow per character and line")},
    {NULL}
};

static void destroy_characters_resources(struct text_priv *s)
{
    ngpu_buffer_freep(&s->vertices);
    ngpu_buffer_freep(&s->atlas_coords);
    ngpu_buffer_freep(&s->texcoord_bounds);
    ngpu_buffer_freep(&s->band_transforms);
    ngpu_buffer_freep(&s->glyph_data);
    ngpu_buffer_freep(&s->user_transforms);
    ngpu_buffer_freep(&s->colors);
    ngpu_buffer_freep(&s->outlines);
    ngpu_buffer_freep(&s->glows);
    ngpu_buffer_freep(&s->blurs);
    ngpu_buffer_freep(&s->outline_positions);
    s->nb_chars = 0;
}

static int refresh_pipeline_data(struct ngl_node *node)
{
    int ret = 0;
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct text_priv *s = node->priv_data;
    struct text *text = s->text_ctx;

    const size_t text_nbchr = text->chars.count;
    if (!text_nbchr) {
        destroy_characters_resources(s);
        return 0;
    }

    if (text_nbchr > s->nb_chars) { // need re-alloc
        destroy_characters_resources(s);

        /* The content of these buffers will remain constant until the next text content update */
        s->vertices        = ngpu_buffer_create(gpu_ctx);
        s->atlas_coords    = ngpu_buffer_create(gpu_ctx);
        s->texcoord_bounds = ngpu_buffer_create(gpu_ctx);
        s->band_transforms = ngpu_buffer_create(gpu_ctx);
        s->glyph_data      = ngpu_buffer_create(gpu_ctx);
        if (!s->vertices || !s->atlas_coords || !s->texcoord_bounds || !s->band_transforms || !s->glyph_data)
            return NGL_ERROR_MEMORY;

        /* The content of these buffers will be updated later using the effects data (see apply_effects()) */
        s->user_transforms = ngpu_buffer_create(gpu_ctx);
        s->colors          = ngpu_buffer_create(gpu_ctx);
        s->outlines        = ngpu_buffer_create(gpu_ctx);
        s->glows           = ngpu_buffer_create(gpu_ctx);
        s->blurs           = ngpu_buffer_create(gpu_ctx);
        s->outline_positions = ngpu_buffer_create(gpu_ctx);
        if (!s->user_transforms || !s->colors || !s->outlines || !s->glows  || !s->blurs || !s->outline_positions)
            return NGL_ERROR_MEMORY;

        if ((ret = ngpu_buffer_init(s->vertices, text_nbchr * 4 * sizeof(float), DYNAMIC_VERTEX_USAGE_FLAGS)) < 0 ||
            (ret = ngpu_buffer_init(s->atlas_coords, text_nbchr * 4 * sizeof(float), DYNAMIC_VERTEX_USAGE_FLAGS)) < 0 ||
            (ret = ngpu_buffer_init(s->texcoord_bounds, text_nbchr * 4 * sizeof(float), DYNAMIC_VERTEX_USAGE_FLAGS)) < 0 ||
            (ret = ngpu_buffer_init(s->band_transforms, text_nbchr * 4 * sizeof(float), DYNAMIC_VERTEX_USAGE_FLAGS)) < 0 ||
            (ret = ngpu_buffer_init(s->glyph_data, text_nbchr * 4 * sizeof(int32_t), DYNAMIC_VERTEX_USAGE_FLAGS)) < 0 ||
            (ret = ngpu_buffer_init(s->user_transforms, text_nbchr * 4 * 4 * sizeof(float), DYNAMIC_VERTEX_USAGE_FLAGS)) < 0 ||
            (ret = ngpu_buffer_init(s->colors, text_nbchr * 4 * sizeof(float), DYNAMIC_VERTEX_USAGE_FLAGS)) < 0 ||
            (ret = ngpu_buffer_init(s->outlines, text_nbchr * 4 * sizeof(float), DYNAMIC_VERTEX_USAGE_FLAGS)) < 0 ||
            (ret = ngpu_buffer_init(s->glows, text_nbchr * 4 * sizeof(float), DYNAMIC_VERTEX_USAGE_FLAGS)) < 0 ||
            (ret = ngpu_buffer_init(s->blurs, text_nbchr * sizeof(float), DYNAMIC_VERTEX_USAGE_FLAGS)) < 0 ||
            (ret = ngpu_buffer_init(s->outline_positions, text_nbchr * sizeof(float), DYNAMIC_VERTEX_USAGE_FLAGS)) < 0)
            return ret;

        struct pipeline_desc_fg *desc_fg = &s->pipeline_desc.fg;
        struct pipeline_desc_common *desc = &desc_fg->common;
        if (desc->pipeline_compat) {
            ngli_pipeline_compat_update_vertex_buffer(desc->pipeline_compat, desc_fg->vertices_index,       s->vertices);
            ngli_pipeline_compat_update_vertex_buffer(desc->pipeline_compat, desc_fg->atlas_coords_index,   s->atlas_coords);
            ngli_pipeline_compat_update_vertex_buffer(desc->pipeline_compat, desc_fg->texcoord_bounds_index, s->texcoord_bounds);
            ngli_pipeline_compat_update_vertex_buffer(desc->pipeline_compat, desc_fg->banding_index,        s->band_transforms);
            ngli_pipeline_compat_update_vertex_buffer(desc->pipeline_compat, desc_fg->glyph_data_index,     s->glyph_data);
            ngli_pipeline_compat_update_vertex_buffer(desc->pipeline_compat, desc_fg->user_transform_index, s->user_transforms);
            ngli_pipeline_compat_update_vertex_buffer(desc->pipeline_compat, desc_fg->color_index,          s->colors);
            ngli_pipeline_compat_update_vertex_buffer(desc->pipeline_compat, desc_fg->outline_index,        s->outlines);
            ngli_pipeline_compat_update_vertex_buffer(desc->pipeline_compat, desc_fg->glow_index,           s->glows);
            ngli_pipeline_compat_update_vertex_buffer(desc->pipeline_compat, desc_fg->blur_index,           s->blurs);
            ngli_pipeline_compat_update_vertex_buffer(desc->pipeline_compat, desc_fg->outline_pos_index,    s->outline_positions);
        }
    }

    if (text->cls->flags & NGLI_TEXT_FLAG_MUTABLE_ATLAS) {
        struct pipeline_desc_fg *desc_fg = &s->pipeline_desc.fg;
        struct pipeline_desc_common *desc = &desc_fg->common;
        if (desc->pipeline_compat &&
            ((ret = ngli_pipeline_compat_update_texture(desc->pipeline_compat, 0, text->curve_texture)) < 0 ||
             (ret = ngli_pipeline_compat_update_texture(desc->pipeline_compat, 1, text->band_texture)) < 0))
            return ret;
    }

    if ((ret = ngpu_buffer_upload(s->vertices,        text->data_ptrs.vertices,        0, text_nbchr * 4 * sizeof(float))) < 0 ||
        (ret = ngpu_buffer_upload(s->atlas_coords,    text->data_ptrs.atlas_coords,    0, text_nbchr * 4 * sizeof(float))) < 0 ||
        (ret = ngpu_buffer_upload(s->texcoord_bounds, text->data_ptrs.texcoord_bounds, 0, text_nbchr * 4 * sizeof(float))) < 0 ||
        (ret = ngpu_buffer_upload(s->band_transforms, text->data_ptrs.band_transforms, 0, text_nbchr * 4 * sizeof(float))) < 0 ||
        (ret = ngpu_buffer_upload(s->glyph_data,      text->data_ptrs.glyph_data,      0, text_nbchr * 4 * sizeof(int32_t))) < 0)
        return ret;

    s->nb_chars = text_nbchr;

    return 0;
}

static int update_text_content(struct ngl_node *node)
{
    struct text_priv *s = node->priv_data;
    const struct text_opts *o = node->opts;

    int ret = ngli_text_set_string(s->text_ctx, o->live.val.s);
    if (ret < 0)
        return ret;

    return refresh_pipeline_data(node);
}

/* Update the GPU buffers using the updated effects data */
static int apply_effects(struct text_priv *s)
{
    int ret;
    struct text *text = s->text_ctx;

    const size_t text_nbchr = text->chars.count;
    if (!text_nbchr)
        return 0;

    const struct text_data_pointers *ptrs = &text->data_ptrs;
    if ((ret = ngpu_buffer_upload(s->user_transforms, ptrs->transform, 0, text_nbchr * 4 * 4 * sizeof(*ptrs->transform))) < 0 ||
        (ret = ngpu_buffer_upload(s->colors, ptrs->color, 0, text_nbchr * 4 * sizeof(*ptrs->color))) < 0 ||
        (ret = ngpu_buffer_upload(s->outlines, ptrs->outline, 0, text_nbchr * 4 * sizeof(*ptrs->outline))) < 0 ||
        (ret = ngpu_buffer_upload(s->glows, ptrs->glow, 0, text_nbchr * 4 * sizeof(*ptrs->glow))) < 0 ||
        (ret = ngpu_buffer_upload(s->blurs, ptrs->blur, 0, text_nbchr * sizeof(*ptrs->blur))) < 0 ||
        (ret = ngpu_buffer_upload(s->outline_positions, ptrs->outline_pos, 0, text_nbchr * sizeof(*ptrs->outline_pos))) < 0)
        return ret;

    return 0;
}

static int init_bounding_box_geometry(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct text_priv *s = node->priv_data;
    const struct text_opts *o = node->opts;

    const struct ngli_box box = {NGLI_ARG_VEC4(o->box)};
    const float vertices[] = {
        box.x,         box.y,
        box.x + box.w, box.y,
        box.x,         box.y + box.h,
        box.x + box.w, box.y + box.h,
    };

    s->bg_vertices = ngpu_buffer_create(gpu_ctx);
    if (!s->bg_vertices)
        return NGL_ERROR_MEMORY;

    int ret;
    if ((ret = ngpu_buffer_init(s->bg_vertices, sizeof(vertices), VERTEX_USAGE_FLAGS)) < 0 ||
        (ret = ngpu_buffer_upload(s->bg_vertices, vertices, 0, sizeof(vertices))) < 0)
        return ret;

    return 0;
}

static int text_init(struct ngl_node *node)
{
    struct text_priv *s = node->priv_data;
    const struct text_opts *o = node->opts;

    s->viewport = node->ctx->viewport;

    s->text_ctx = ngli_text_create(node->ctx);
    if (!s->text_ctx)
        return NGL_ERROR_MEMORY;

    const struct text_config config = {
        .font_faces = o->font_faces,
        .nb_font_faces = o->nb_font_faces,
        .pt_size = o->pt_size,
        .dpi = o->dpi,
        .padding = o->padding,
        .scale_mode = o->scale_mode,
        .font_scale = o->font_scale,
        .valign = o->valign,
        .halign = o->halign,
        .writing_mode = o->writing_mode,
        .box = {NGLI_ARG_VEC4(o->box)},
        .effect_nodes = o->effect_nodes,
        .nb_effect_nodes = o->nb_effect_nodes,
        .defaults = {
            .color = {NGLI_ARG_VEC3(o->fg_color)},
            .opacity = o->fg_opacity,
        }
    };

    int ret = ngli_text_init(s->text_ctx, &config);
    if (ret < 0)
        return ret;

    ret = ngli_text_prefetch(s->text_ctx);
    if (ret < 0)
        return ret;

    s->dist_scale = 72.f / (float)(o->pt_size * o->dpi);


    ret = init_bounding_box_geometry(node);
    if (ret < 0)
        return ret;

    ret = update_text_content(node);
    if (ret < 0)
        return ret;

    return 0;
}

static int init_subdesc(struct ngl_node *node,
                        struct pipeline_desc_common *desc,
                        const struct ngpu_graphics_state *graphics_state,
                        const struct ngpu_rendertarget_layout *rendertarget_layout,
                        const struct ngpu_pgcraft_params *crafter_params)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    desc->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!desc->crafter)
        return NGL_ERROR_MEMORY;

    int ret = ngpu_pgcraft_craft(desc->crafter, crafter_params);
    if (ret < 0)
        return ret;

    desc->pipeline_compat = ngli_pipeline_compat_create(gpu_ctx);
    if (!desc->pipeline_compat)
        return NGL_ERROR_MEMORY;

    const struct pipeline_compat_params params = {
        .type          = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics      = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .state        = *graphics_state,
            .rt_layout    = *rendertarget_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(desc->crafter),
        },
        .program          = ngpu_pgcraft_get_program(desc->crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(desc->crafter),
        .resources        = ngpu_pgcraft_get_bindgroup_resources(desc->crafter),
        .vertex_resources = ngpu_pgcraft_get_vertex_resources(desc->crafter),
        .texture_infos    = ngpu_pgcraft_get_texture_infos(desc->crafter),
    };

    ret = ngli_pipeline_compat_init(desc->pipeline_compat, &params);
    if (ret < 0)
        return ret;

    return 0;
}

static int bg_prepare(struct ngl_node *node, struct pipeline_desc_bg *desc,
                      const struct ngpu_graphics_state *graphics_state,
                      const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct text_priv *s = node->priv_data;
    struct ngpu_ctx *gpu_ctx = node->ctx->gpu_ctx;

    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);

    /* Initialize vertex block descriptor */
    ngpu_block_desc_init(gpu_ctx, &desc->vert_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_add_field(&desc->vert_block_desc, "modelview_matrix", NGPU_TYPE_MAT4, 0);
    ngpu_block_desc_add_field(&desc->vert_block_desc, "projection_matrix", NGPU_TYPE_MAT4, 0);
    ngli_assert(ngpu_block_desc_get_size(&desc->vert_block_desc, 0) == sizeof(struct text_bg_vert_block));

    const size_t vert_size = ngpu_block_desc_get_size(&desc->vert_block_desc, 0);

    /* Initialize fragment block descriptor */
    ngpu_block_desc_init(gpu_ctx, &desc->frag_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_add_field(&desc->frag_block_desc, "color", NGPU_TYPE_VEC3, 0);
    ngpu_block_desc_add_field(&desc->frag_block_desc, "opacity", NGPU_TYPE_F32, 0);
    ngli_assert(ngpu_block_desc_get_size(&desc->frag_block_desc, 0) == sizeof(struct text_bg_frag_block));

    const size_t frag_size = ngpu_block_desc_get_size(&desc->frag_block_desc, 0);

    const struct ngpu_pgcraft_block blocks[] = {
        {
            .name          = "vert_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_VERT,
            .block         = &desc->vert_block_desc,
            .buffer        = {.buffer = staging_buf, .size = vert_size},
        },
        {
            .name          = "frag_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &desc->frag_block_desc,
            .buffer        = {.buffer = staging_buf, .size = frag_size},
        },
    };

    const struct ngpu_pgcraft_attribute attributes[] = {
        {
            .name     = "position",
            .type     = NGPU_TYPE_VEC2,
            .format   = NGPU_FORMAT_R32G32_SFLOAT,
            .stride   = 2 * sizeof(float),
            .buffer   = s->bg_vertices,
        },
    };

    /* This controls how the background blends onto the current framebuffer */
    struct ngpu_graphics_state state = *graphics_state;
    state.blend = 1;
    state.blend_src_factor   = NGPU_BLEND_FACTOR_ONE;
    state.blend_dst_factor   = NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    state.blend_src_factor_a = NGPU_BLEND_FACTOR_ONE;
    state.blend_dst_factor_a = NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/text-bg",
        .vert_base        = text_bg_vert,
        .frag_base        = text_bg_frag,
        .blocks           = blocks,
        .nb_blocks        = NGLI_ARRAY_NB(blocks),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
    };

    int ret = init_subdesc(node, &desc->common, &state, rendertarget_layout, &crafter_params);
    if (ret < 0)
        return ret;

    desc->vert_block_index = ngpu_pgcraft_get_block_index(desc->common.crafter, "vert_params", NGPU_PROGRAM_STAGE_VERT);
    desc->frag_block_index = ngpu_pgcraft_get_block_index(desc->common.crafter, "frag_params", NGPU_PROGRAM_STAGE_FRAG);

    return 0;
}

static int fg_prepare(struct ngl_node *node, struct pipeline_desc_fg *desc,
                      const struct ngpu_graphics_state *graphics_state,
                      const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct text_priv *s = node->priv_data;
    struct ngpu_ctx *gpu_ctx = node->ctx->gpu_ctx;
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);

    /* Initialize vertex block descriptor */
    ngpu_block_desc_init(gpu_ctx, &desc->vert_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_add_field(&desc->vert_block_desc, "modelview_matrix", NGPU_TYPE_MAT4, 0);
    ngpu_block_desc_add_field(&desc->vert_block_desc, "projection_matrix", NGPU_TYPE_MAT4, 0);
    ngli_assert(ngpu_block_desc_get_size(&desc->vert_block_desc, 0) == sizeof(struct text_fg_vert_block));

    const size_t vert_size = ngpu_block_desc_get_size(&desc->vert_block_desc, 0);

    /* Initialize fragment block descriptor */
    ngpu_block_desc_init(gpu_ctx, &desc->frag_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_add_field(&desc->frag_block_desc, "dist_scale", NGPU_TYPE_F32, 0);
    ngli_assert(ngpu_block_desc_get_size(&desc->frag_block_desc, 0) == sizeof(struct text_fg_frag_block));

    const size_t frag_size = ngpu_block_desc_get_size(&desc->frag_block_desc, 0);

    const struct ngpu_pgcraft_block blocks[] = {
        {
            .name          = "vert_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_VERT,
            .block         = &desc->vert_block_desc,
            .buffer        = {.buffer = staging_buf, .size = vert_size},
        },
        {
            .name          = "frag_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &desc->frag_block_desc,
            .buffer        = {.buffer = staging_buf, .size = frag_size},
        },
    };

    const struct ngpu_pgcraft_texture textures[] = {
        {
            .name        = "curve_tex",
            .type        = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .texture     = s->text_ctx->curve_texture,
            .no_metadata = true,
        },
        {
            .name        = "band_tex",
            .type        = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .texture     = s->text_ctx->band_texture,
            .no_metadata = true,
        },
    };

    const struct ngpu_pgcraft_attribute attributes[] = {
        {
            .name     = "vertices",
            .type     = NGPU_TYPE_VEC4,
            .format   = NGPU_FORMAT_R32G32B32A32_SFLOAT,
            .stride   = 4 * sizeof(float),
            .buffer   = s->vertices,
            .rate     = 1,
        }, {
            .name     = "atlas_coords",
            .type     = NGPU_TYPE_VEC4,
            .format   = NGPU_FORMAT_R32G32B32A32_SFLOAT,
            .stride   = 4 * sizeof(float),
            .buffer   = s->atlas_coords,
            .rate     = 1,
        }, {
            .name     = "texcoord_bounds",
            .type     = NGPU_TYPE_VEC4,
            .format   = NGPU_FORMAT_R32G32B32A32_SFLOAT,
            .stride   = 4 * sizeof(float),
            .buffer   = s->texcoord_bounds,
            .rate     = 1,
        }, {
            .name     = "frag_banding",
            .type     = NGPU_TYPE_VEC4,
            .format   = NGPU_FORMAT_R32G32B32A32_SFLOAT,
            .stride   = 4 * sizeof(float),
            .buffer   = s->band_transforms,
            .rate     = 1,
        }, {
            .name     = "frag_glyph_data",
            .type     = NGPU_TYPE_IVEC4,
            .format   = NGPU_FORMAT_R32G32B32A32_SINT,
            .stride   = 4 * sizeof(int32_t),
            .buffer   = s->glyph_data,
            .rate     = 1,
        }, {
            .name     = "user_transform",
            .type     = NGPU_TYPE_MAT4,
            .format   = NGPU_FORMAT_R32G32B32A32_SFLOAT,
            .stride   = 4 * 4 * sizeof(float),
            .buffer   = s->user_transforms,
            .rate     = 1,
        }, {
            .name     = "frag_color",
            .type     = NGPU_TYPE_VEC4,
            .format   = NGPU_FORMAT_R32G32B32A32_SFLOAT,
            .stride   = 4 * sizeof(float),
            .buffer   = s->colors,
            .rate     = 1,
        }, {
            .name     = "frag_outline",
            .type     = NGPU_TYPE_VEC4,
            .format   = NGPU_FORMAT_R32G32B32A32_SFLOAT,
            .stride   = 4 * sizeof(float),
            .buffer   = s->outlines,
            .rate     = 1,
        }, {
            .name     = "frag_glow",
            .type     = NGPU_TYPE_VEC4,
            .format   = NGPU_FORMAT_R32G32B32A32_SFLOAT,
            .stride   = 4 * sizeof(float),
            .buffer   = s->glows,
            .rate     = 1,
        }, {
            .name     = "frag_blur",
            .type     = NGPU_TYPE_F32,
            .format   = NGPU_FORMAT_R32_SFLOAT,
            .stride   = sizeof(float),
            .buffer   = s->blurs,
            .rate     = 1,
        }, {
            .name     = "frag_outline_pos",
            .type     = NGPU_TYPE_F32,
            .format   = NGPU_FORMAT_R32_SFLOAT,
            .stride   = sizeof(float),
            .buffer   = s->outline_positions,
            .rate     = 1,
        },
    };

    /* This controls how the characters blend onto the background */
    struct ngpu_graphics_state state = *graphics_state;
    state.blend = 1;
    state.blend_src_factor   = NGPU_BLEND_FACTOR_ONE;
    state.blend_dst_factor   = NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    state.blend_src_factor_a = NGPU_BLEND_FACTOR_ONE;
    state.blend_dst_factor_a = NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "texcoord",    .type = NGPU_TYPE_VEC2},
        {.name = "banding",     .type = NGPU_TYPE_VEC4},
        {.name = "glyph",      .type = NGPU_TYPE_IVEC4},
        {.name = "color",      .type = NGPU_TYPE_VEC4},
        {.name = "outline",    .type = NGPU_TYPE_VEC4},
        {.name = "glow",       .type = NGPU_TYPE_VEC4},
        {.name = "blur",       .type = NGPU_TYPE_F32},
        {.name = "outline_pos", .type = NGPU_TYPE_F32},
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/text-fg",
        .vert_base        = text_slug_vert,
        .frag_base        = text_slug_frag,
        .blocks           = blocks,
        .nb_blocks        = NGLI_ARRAY_NB(blocks),
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    int ret = init_subdesc(node, &desc->common, &state, rendertarget_layout, &crafter_params);
    if (ret < 0)
        return ret;

    desc->vert_block_index = ngpu_pgcraft_get_block_index(desc->common.crafter, "vert_params", NGPU_PROGRAM_STAGE_VERT);
    desc->frag_block_index = ngpu_pgcraft_get_block_index(desc->common.crafter, "frag_params", NGPU_PROGRAM_STAGE_FRAG);

    desc->vertices_index       = ngpu_pgcraft_get_vertex_buffer_index(desc->common.crafter, "vertices");
    desc->atlas_coords_index   = ngpu_pgcraft_get_vertex_buffer_index(desc->common.crafter, "atlas_coords");
    desc->texcoord_bounds_index = ngpu_pgcraft_get_vertex_buffer_index(desc->common.crafter, "texcoord_bounds");
    desc->banding_index        = ngpu_pgcraft_get_vertex_buffer_index(desc->common.crafter, "frag_banding");
    desc->glyph_data_index     = ngpu_pgcraft_get_vertex_buffer_index(desc->common.crafter, "frag_glyph_data");
    desc->user_transform_index = ngpu_pgcraft_get_vertex_buffer_index(desc->common.crafter, "user_transform");
    desc->color_index          = ngpu_pgcraft_get_vertex_buffer_index(desc->common.crafter, "frag_color");
    desc->outline_index        = ngpu_pgcraft_get_vertex_buffer_index(desc->common.crafter, "frag_outline");
    desc->glow_index           = ngpu_pgcraft_get_vertex_buffer_index(desc->common.crafter, "frag_glow");
    desc->blur_index           = ngpu_pgcraft_get_vertex_buffer_index(desc->common.crafter, "frag_blur");
    desc->outline_pos_index    = ngpu_pgcraft_get_vertex_buffer_index(desc->common.crafter, "frag_outline_pos");

    return 0;
}

static int text_prepare(struct ngl_node *node,
                        const struct ngpu_graphics_state *graphics_state,
                        const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct text_priv *s = node->priv_data;

    struct pipeline_desc *desc = &s->pipeline_desc;

    int ret = bg_prepare(node, &desc->bg, graphics_state, rendertarget_layout);
    if (ret < 0)
        return ret;

    ret = fg_prepare(node, &desc->fg, graphics_state, rendertarget_layout);
    if (ret < 0)
        return ret;

    return 0;
}

static int text_update(struct ngl_node *node, double t)
{
    struct ngl_ctx *ctx = node->ctx;
    struct text_priv *s = node->priv_data;
    const struct text_opts *o = node->opts;

    if (s->live_changed) {
        const struct text_effects_defaults defaults = {
            .color = {NGLI_ARG_VEC3(o->fg_color)},
            .opacity = o->fg_opacity,
        };
        ngli_text_update_effects_defaults(s->text_ctx, &defaults);

        int ret = update_text_content(node);
        if (ret < 0)
            return ret;
        s->live_changed = 0;
    }

    if (memcmp(&s->viewport, &ctx->viewport, sizeof(ctx->viewport))) {
        memcpy(&s->viewport, &ctx->viewport, sizeof(ctx->viewport));
        ngli_text_refresh_geometry_data(s->text_ctx);
        int ret = refresh_pipeline_data(node);
        if (ret < 0)
            return ret;
    }

    int ret = ngli_text_set_time(s->text_ctx, t);
    if (ret < 0)
        return ret;

    ret = apply_effects(s);
    if (ret < 0)
        return ret;

    return 0;
}

static void text_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct text_priv *s = node->priv_data;
    const struct text_opts *o = node->opts;

    const struct ngli_mat4 *modelview_matrix  = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const struct ngli_mat4 *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    struct pipeline_desc *desc = &s->pipeline_desc;

    if (!ngpu_ctx_is_render_pass_active(ctx->gpu_ctx)) {
        struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
        ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);
    }

    /* Fill and push background vertex block to staging buffer */
    struct pipeline_desc_bg *bg_desc = &desc->bg;
    struct text_bg_vert_block bg_vert_data;
    bg_vert_data.modelview_matrix = *modelview_matrix;
    bg_vert_data.projection_matrix = *projection_matrix;

    if (bg_desc->vert_block_index >= 0) {
        const size_t vert_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &bg_vert_data, sizeof(bg_vert_data));
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(bg_desc->common.pipeline_compat, bg_desc->vert_block_index,
                                           staging_buf, vert_offset, sizeof(bg_vert_data));
    }

    /* Fill and push background fragment block to staging buffer */
    struct text_bg_frag_block bg_frag_data = {0};
    memcpy(bg_frag_data.color, o->bg_color, sizeof(bg_frag_data.color));
    bg_frag_data.opacity = o->bg_opacity;

    if (bg_desc->frag_block_index >= 0) {
        const size_t frag_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &bg_frag_data, sizeof(bg_frag_data));
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(bg_desc->common.pipeline_compat, bg_desc->frag_block_index,
                                           staging_buf, frag_offset, sizeof(bg_frag_data));
    }

    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    ngpu_ctx_set_viewport(gpu_ctx, &ctx->viewport);
    ngpu_ctx_set_scissor(gpu_ctx, &ctx->scissor);

    ngli_pipeline_compat_draw(bg_desc->common.pipeline_compat, 4, 1, 0);

    if (s->nb_chars) {
        /* Fill and push foreground vertex block to staging buffer */
        struct pipeline_desc_fg *fg_desc = &desc->fg;
        struct text_fg_vert_block fg_vert_data;
        fg_vert_data.modelview_matrix = *modelview_matrix;
        fg_vert_data.projection_matrix = *projection_matrix;

        if (fg_desc->vert_block_index >= 0) {
            const size_t vert_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &fg_vert_data, sizeof(fg_vert_data));
            struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
            ngli_pipeline_compat_update_buffer(fg_desc->common.pipeline_compat, fg_desc->vert_block_index,
                                               staging_buf, vert_offset, sizeof(fg_vert_data));
        }

        /* Fill and push foreground fragment block to staging buffer */
        struct text_fg_frag_block fg_frag_data = {0};
        fg_frag_data.dist_scale = s->dist_scale;

        if (fg_desc->frag_block_index >= 0) {
            const size_t frag_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &fg_frag_data, sizeof(fg_frag_data));
            struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
            ngli_pipeline_compat_update_buffer(fg_desc->common.pipeline_compat, fg_desc->frag_block_index,
                                               staging_buf, frag_offset, sizeof(fg_frag_data));
        }

        ngli_pipeline_compat_draw(fg_desc->common.pipeline_compat, 4, (uint32_t)s->nb_chars, 0);
    }
}

static void text_release(struct ngl_node *node)
{
    struct text_priv *s = node->priv_data;
    if (s->text_ctx)
        ngli_text_release(s->text_ctx);
}

static void text_uninit(struct ngl_node *node)
{
    struct text_priv *s = node->priv_data;
    struct pipeline_desc *desc = &s->pipeline_desc;
    ngli_pipeline_compat_freep(&desc->bg.common.pipeline_compat);
    ngli_pipeline_compat_freep(&desc->fg.common.pipeline_compat);
    ngpu_pgcraft_freep(&desc->bg.common.crafter);
    ngpu_pgcraft_freep(&desc->fg.common.crafter);
    ngpu_block_desc_reset(&desc->bg.vert_block_desc);
    ngpu_block_desc_reset(&desc->bg.frag_block_desc);
    ngpu_block_desc_reset(&desc->fg.vert_block_desc);
    ngpu_block_desc_reset(&desc->fg.frag_block_desc);
    ngpu_buffer_freep(&s->bg_vertices);
    destroy_characters_resources(s);
    ngli_text_freep(&s->text_ctx);
}

const struct node_class ngli_text_class = {
    .id             = NGL_NODE_TEXT,
    .category       = NGLI_NODE_CATEGORY_DRAW,
    .name           = "Text",
    .init           = text_init,
    .prepare        = text_prepare,
    .update         = text_update,
    .draw           = text_draw,
    .release        = text_release,
    .uninit         = text_uninit,
    .opts_size      = sizeof(struct text_opts),
    .priv_size      = sizeof(struct text_priv),
    .params         = text_params,
    .flags          = NGLI_NODE_FLAG_LIVECTL,
    .livectl_offset = OFFSET(live),
    .file           = __FILE__,
};
