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
#include <string.h>

#include "aabb.h"
#include "blend_mode.h"
#include "internal.h"
#include "node2d.h"
#include "math_utils.h"
#include "log.h"
#include <ngpu/ngpu.h>
#include "node_block.h"
#include "node_paint.h"
#include "node_stroke2d.h"
#include "node_uniform.h"
#include "node_texture.h"
#include "pipeline_compat.h"
#include "utils/bstr.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

/* GLSL shaders as strings */
#include "drawrect_vert.h"
#include "drawrect_frag.h"
#include "helper_misc_utils_glsl.h"
#include "helper_noise_glsl.h"
#include "helper_srgb_glsl.h"

/* ngli_stroke() for the no-stroke case: transparent */
static const char no_stroke_glsl[] =
    "vec4 ngli_stroke(vec2 uv, vec2 tex_coord) { return vec4(0.0); }\n";

static const char *const paint_texture_names[PAINT_SHADER_ROLE_NB] = {
    [PAINT_SHADER_ROLE_FILL]   = "ngli_fill_tex",
    [PAINT_SHADER_ROLE_STROKE] = "ngli_stroke_tex",
};

struct resource_map {
    int32_t index;
    const struct block_info *info;
    size_t buffer_rev;
};

struct texture_map {
    const struct image *image;
    size_t image_rev;
};

struct block_map {
    int32_t index;
    const struct block_info *info;
    size_t buffer_rev;
};

struct drawrect2d_vert_block {
    struct ngli_mat4 projection_matrix;
    struct ngli_mat4 modelview_matrix;
    float rect[4];
    float uv_scale[2];
    float stroke_uv_scale[2];
    float margin_px;
    float _pad0;
    float margin_uv[2];
};

struct drawrect2d_frag_block {
    float rect_size[2];
    float corner_radius[2];
    float outline_width;
    int32_t outline_mode;
    float opacity;
    float fill_opacity;
    float stroke_opacity;
    int32_t fill_content_wrap;
    int32_t stroke_content_wrap;
    float content_zoom;
    float content_translate[2];
    float content_orientation[2];
    float frag_uv_scale[2];
    int32_t fill_premult;
    int32_t stroke_premult;
    struct ngli_vec4 clip_inv[NGLI_MAX_CLIPS_2D];
    struct ngli_vec4 clip_rect[NGLI_MAX_CLIPS_2D];
    struct ngli_vec4 clip_radius[NGLI_MAX_CLIPS_2D];
    int32_t nb_clips;
    float _pad1[3];
};

struct drawrect2d_opts {
    struct ngl_node *rect_node;
    float rect[4];
    struct ngl_node *fill_node;
    struct ngl_node *stroke_node;
    struct ngl_node *corner_radius_node;
    float corner_radius[2];
    struct ngli_node2d_opts node2d;
    struct ngl_node *clip_rect_node;
    float clip_rect[4];
    struct ngl_node *clip_corner_radius_node;
    float clip_corner_radius[2];
    struct ngl_node *content_zoom_node;
    float content_zoom;
    struct ngl_node *content_translate_node;
    float content_translate[2];
    float content_orientation;
};

/* Tracks a user-supplied uniform node (CustomPaint) */
struct user_uniform {
    int32_t field_index;
    const struct ngl_node *node;
};

/* Tracks a prebuilt fill/stroke uniform: reads from stable paint storage at draw time */
struct prebuilt_uniform {
    int32_t field_index;
    const uint8_t *data;
};

struct drawrect2d_priv {
    struct ngli_node2d_info node2d_info;
    float rect[4];
    float corner_radius[2];
    struct pipeline_compat *pipeline_compat;
    NGLI_DARRAY(struct texture_map) textures_map;
    NGLI_DARRAY(struct block_map) blocks_map;
    struct ngpu_pgcraft *crafter;

    /* Uniform blocks */
    struct ngpu_block_desc vert_block_desc;
    size_t vert_block_size;
    int32_t vert_block_index;

    struct ngpu_block_desc frag_block_desc;
    int32_t frag_block_index;

    struct ngpu_block_desc user_block_desc;
    size_t user_block_size;
    int32_t user_block_index;

    NGLI_DARRAY(struct user_uniform) user_uniforms;
    NGLI_DARRAY(struct prebuilt_uniform) prebuilt_uniforms;
    NGLI_DARRAY(struct user_uniform) stroke_user_uniforms;
    NGLI_DARRAY(struct prebuilt_uniform) stroke_prebuilt_uniforms;
    const struct paint_info *fill_paint;
    const struct paint_info *stroke_paint;
    const struct stroke2d_info *stroke;
    char *frag_shader;
    char *vert_shader;
};


static void compute_geometry(struct drawrect2d_priv *s, const float *rect, const float *corner_radius)
{
    s->rect[0] = rect[0];
    s->rect[1] = rect[1];
    s->rect[2] = NGLI_MAX(rect[2], 0.f);
    s->rect[3] = NGLI_MAX(rect[3], 0.f);

    const float half_w = s->rect[2] / 2.0f;
    const float half_h = s->rect[3] / 2.0f;
    s->node2d_info.aabb = (struct aabb) {
        .center = {s->rect[0] + half_w, s->rect[1] + half_h, 0.0f, 1.0f},
        .extent = {half_w, half_h},
    };

    s->corner_radius[0] = NGLI_MIN(NGLI_MAX(corner_radius[0], 0.f), half_w);
    s->corner_radius[1] = NGLI_MIN(NGLI_MAX(corner_radius[1], 0.f), half_h);
}

static const struct ngl_node *get_scaling_texture(const struct paint_info *paint)
{
    if (paint->texture)
        return paint->texture;
    if (paint->custom_textures.count)
        return paint->custom_textures.data[0].texture_node;
    return NULL;
}

static void compute_texture_uv_scale(const struct paint_info *paint,
                                     const float *rect,
                                     const float *node_scale,
                                     bool orientation_is_transposed,
                                     float *uv_scale)
{
    uv_scale[0] = 1.f;
    uv_scale[1] = 1.f;

    if (!paint)
        return;

    const struct paint_base_opts *opts = (const struct paint_base_opts *)paint->opts;
    const struct ngl_node *texture = get_scaling_texture(paint);
    if (opts->scaling == PAINT_SCALING_NONE || !texture)
        return;

    const struct texture_info *texture_info = ngli_node_texture_get_texture_info(texture);
    const struct image *image = &texture_info->image;
    const float tex_w = orientation_is_transposed ? (float)image->params.height : (float)image->params.width;
    const float tex_h = orientation_is_transposed ? (float)image->params.width  : (float)image->params.height;
    const float scaled_w = rect[2] * node_scale[0];
    const float scaled_h = rect[3] * node_scale[1];
    if (tex_w <= 0.f || tex_h <= 0.f || scaled_w <= 0.f || scaled_h <= 0.f)
        return;

    const float ratio = (scaled_w / scaled_h) / (tex_w / tex_h);
    if (opts->scaling == PAINT_SCALING_FIT) {
        uv_scale[0] = ratio > 1.f ? ratio : 1.f;
        uv_scale[1] = ratio < 1.f ? 1.f / ratio : 1.f;
    } else {
        uv_scale[0] = ratio < 1.f ? ratio : 1.f;
        uv_scale[1] = ratio > 1.f ? 1.f / ratio : 1.f;
    }
}

static int is_valid_orientation(float angle)
{
    return angle == 0.f ||
           angle ==  90.f || angle ==  180.f || angle ==  270.f ||
           angle == -90.f || angle == -180.f || angle == -270.f;
}

static int update_content_orientation(struct ngl_node *node)
{
    const struct drawrect2d_opts *o = node->opts;
    if (!is_valid_orientation(o->content_orientation)) {
        LOG(ERROR, "content_orientation must be 0, +/-90, +/-180 or +/-270, got %g", o->content_orientation);
        return NGL_ERROR_INVALID_ARG;
    }
    return 0;
}

#define OFFSET(x) offsetof(struct drawrect2d_opts, x)
static const struct node_param drawrect2d_params[] = {
    {
        .key         = "rect",
        .type        = NGLI_PARAM_TYPE_VEC4,
        .offset      = OFFSET(rect_node),
        .flags       = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc        = NGLI_DOCSTRING("rect (x, y, width, height)"),
    },
    {
        .key        = "fill",
        .type       = NGLI_PARAM_TYPE_NODE,
        .offset     = OFFSET(fill_node),
        .node_types = ngli_paint_node_types,
        .flags      = NGLI_PARAM_FLAG_NON_NULL,
        .desc       = NGLI_DOCSTRING("fill paint applied inside the rect"),
    },
    {
        .key        = "stroke",
        .type       = NGLI_PARAM_TYPE_NODE,
        .offset     = OFFSET(stroke_node),
        .node_types = (const uint32_t[]){
            NGL_NODE_STROKE2D,
            NGLI_NODE_NONE,
        },
        .desc       = NGLI_DOCSTRING("optional outline stroke"),
    },
    {
        .key    = "corner_radius",
        .type   = NGLI_PARAM_TYPE_VEC2,
        .offset = OFFSET(corner_radius_node),
        .flags  = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc   = NGLI_DOCSTRING("corner radii in pixels (x, y); set x != y for elliptical corners; "
                                  "set to (width/2, height/2) for a full ellipse/oval"),
    },
    {
        .key    = "translate",
        .type   = NGLI_PARAM_TYPE_VEC2,
        .offset = OFFSET(node2d.translate_node),
        .flags  = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc   = NGLI_DOCSTRING("translation in pixels"),
    },
    {
        .key    = "rotation",
        .type   = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(node2d.rotation_node),
        .flags  = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc   = NGLI_DOCSTRING("rotation angle in degrees"),
    },
    {
        .key       = "scale",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.scale_node),
        .def_value = {.vec={1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("scale factors"),
    },
    {
        .key       = "anchor",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.anchor_node),
        .def_value = {.vec={NAN, NAN}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("anchor/pivot point in pixels (default: center of rect)"),
    },
    {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(node2d.opacity_node),
        .def_value = {.f32 = 1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("opacity of the rectangle (0 for fully transparent, 1 for fully opaque)"),
    },
    {
        .key       = "visible",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(node2d.visible),
        .def_value = {.i32=1},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("whether the rectangle is visible"),
    },
    {
        .key       = "blend_mode",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(node2d.blend_mode),
        .def_value = {.i32 = NGLI_BLEND_MODE_SRC_OVER},
        .choices   = &ngli_blend_mode_choices,
        .desc      = NGLI_DOCSTRING("define how this node is composited with the current framebuffer"),
    },
    {
        .key    = "clip_rect",
        .type   = NGLI_PARAM_TYPE_VEC4,
        .offset = OFFSET(clip_rect_node),
        .flags  = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc   = NGLI_DOCSTRING("clipping rectangle (x, y, width, height) in pixel coordinates "
                                  "(follows the node's transform); when width or height is 0 "
                                  "clipping is disabled. The clip edge is anti-aliased"),
    },
    {
        .key    = "clip_corner_radius",
        .type   = NGLI_PARAM_TYPE_VEC2,
        .offset = OFFSET(clip_corner_radius_node),
        .flags  = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc   = NGLI_DOCSTRING("corner radii (x, y) of clip_rect in pixels, for rounded clipping; "
                                 "(0, 0) gives sharp corners. Each radius is clamped to half the "
                                 "corresponding clip_rect side"),
    },
    {
        .key       = "content_zoom",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(content_zoom_node),
        .def_value = {.f32 = 1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("zoom factor applied to the fill content (>1 zooms in; "
                                    "for fit scaling mode zoom is ignored)"),
    },
    {
        .key   = "content_translate",
        .type  = NGLI_PARAM_TYPE_VEC2,
        .offset = OFFSET(content_translate_node),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc  = NGLI_DOCSTRING("UV-space translation of the fill content; "
                                "for fit scaling mode the translation is clamped to keep "
                                "the content within the DrawRect2D bounds"),
    },
    {
        .key         = "content_orientation",
        .type        = NGLI_PARAM_TYPE_F32,
        .offset      = OFFSET(content_orientation),
        .flags       = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc        = NGLI_DOCSTRING("rotation angle in degrees applied to the fill content "
                                      "(must be 0, +/-90, +/-180 or +/-270)"),
        .update_func = update_content_orientation,
    },
    {NULL},
};
#undef OFFSET

static char *build_vertex_shader(bool has_fill_texture, bool has_stroke_texture)
{
    struct bstr *bstr = ngli_bstr_create();
    if (!bstr)
        return NULL;

    if (has_fill_texture)
        ngli_bstr_print(bstr, "#define NGLI_DRAWRECT_FILL_TEXTURE\n");
    if (has_stroke_texture)
        ngli_bstr_print(bstr, "#define NGLI_DRAWRECT_STROKE_TEXTURE\n");

    ngli_bstr_print(bstr, drawrect_vert);

    if (ngli_bstr_check(bstr) < 0) {
        ngli_bstr_freep(&bstr);
        return NULL;
    }

    char *shader = ngli_bstr_strdup(bstr);
    ngli_bstr_freep(&bstr);

    return shader;
}

static int build_texture_map(struct drawrect2d_priv *s)
{
    const struct ngpu_pgcraft_texture_infos texture_infos = ngpu_pgcraft_get_texture_infos(s->crafter);
    for (size_t i = 0; i < texture_infos.nb_infos; i++) {
        const struct texture_map map = {.image = texture_infos.infos[i].image, .image_rev = SIZE_MAX};
        if (ngli_darray_push(&s->textures_map, map) < 0)
            return NGL_ERROR_MEMORY;
    }
    return 0;
}

static int drawrect2d_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct drawrect2d_priv *s = node->priv_data;
    const struct drawrect2d_opts *o = node->opts;
    int ret;

    if (!is_valid_orientation(o->content_orientation)) {
        LOG(ERROR, "content_orientation must be 0, +/-90, +/-180 or +/-270, got %g", o->content_orientation);
        return NGL_ERROR_INVALID_ARG;
    }

    const float *rect = ngli_node_get_data_ptr(o->rect_node, o->rect);
    const float *corner_radius = ngli_node_get_data_ptr(o->corner_radius_node, o->corner_radius);
    compute_geometry(s, rect, corner_radius);

    const struct paint_info *fill_paint = (const struct paint_info *)o->fill_node->priv_data;
    s->fill_paint = fill_paint;

    const struct stroke2d_info *stroke = o->stroke_node ? ngli_stroke2d_get_info(o->stroke_node) : NULL;
    const struct paint_info *stroke_paint = stroke ? (const struct paint_info *)stroke->paint->priv_data : NULL;
    if (fill_paint->color_output_count && stroke_paint) {
        LOG(ERROR, "DrawRect2D.stroke is not supported with a multi-render-target CustomPaint");
        return NGL_ERROR_INVALID_USAGE;
    }
    if (stroke_paint && stroke_paint->color_output_count) {
        LOG(ERROR, "a Stroke2D paint cannot be a multi-render-target CustomPaint");
        return NGL_ERROR_INVALID_USAGE;
    }
    s->stroke = stroke;
    s->stroke_paint = stroke_paint;

    const struct ngl_node *texture = fill_paint->texture;

    s->pipeline_compat = NULL;

    s->vert_shader = build_vertex_shader(fill_paint->texture != NULL,
                                         stroke_paint && stroke_paint->texture);
    if (!s->vert_shader)
        return NGL_ERROR_MEMORY;

    /* Build fragment shader */
    struct bstr *bstr = ngli_bstr_create();
    if (!bstr)
        return NGL_ERROR_MEMORY;

    const uint32_t all_helper_flags = fill_paint->helper_flags | (stroke_paint ? stroke_paint->helper_flags : 0);
    if (all_helper_flags & PAINT_HELPER_MISC_UTILS) ngli_bstr_print(bstr, helper_misc_utils_glsl);
    if (all_helper_flags & PAINT_HELPER_NOISE)      ngli_bstr_print(bstr, helper_noise_glsl);
    if (all_helper_flags & PAINT_HELPER_SRGB)       ngli_bstr_print(bstr, helper_srgb_glsl);
    const char *fill_header = fill_paint->glsl_header;
    const char *stroke_header = stroke_paint ? stroke_paint->glsl_header : NULL;
    /* A header holding no placeholder expands identically for both roles */
    if (fill_header && stroke_header && !strcmp(fill_header, stroke_header) && !strchr(fill_header, '$'))
        stroke_header = NULL;
    if (fill_header) {
        ngli_paint_glsl_write(bstr, fill_header, PAINT_SHADER_ROLE_FILL,
                              fill_paint->color_output_count ? "ngli_colors" : "ngli_color");
        ngli_bstr_print(bstr, "\n");
    }
    if (stroke_header) {
        ngli_paint_glsl_write(bstr, stroke_header, PAINT_SHADER_ROLE_STROKE, "ngli_stroke");
        ngli_bstr_print(bstr, "\n");
    }
    ngli_paint_glsl_write(bstr, fill_paint->glsl, PAINT_SHADER_ROLE_FILL,
                          fill_paint->color_output_count ? "ngli_colors" : "ngli_color");
    if (fill_paint->color_output_count) {
        ngli_bstr_print(bstr, "void main() { ngli_colors(ngli_uv, ngli_tex_coord); }\n");
    } else {
        if (stroke_paint)
            ngli_paint_glsl_write(bstr, stroke_paint->glsl, PAINT_SHADER_ROLE_STROKE, "ngli_stroke");
        else
            ngli_bstr_print(bstr, no_stroke_glsl);
        ngli_bstr_print(bstr, drawrect_frag);
    }

    if (ngli_bstr_check(bstr) < 0) {
        ngli_bstr_freep(&bstr);
        return NGL_ERROR_MEMORY;
    }
    s->frag_shader = ngli_bstr_strdup(bstr);
    ngli_bstr_freep(&bstr);
    if (!s->frag_shader)
        return NGL_ERROR_MEMORY;

    /* Build vertex uniform block */
    static const struct ngpu_block_field vert_fields[] = {
        {.name = "projection_matrix",  .type = NGPU_TYPE_MAT4},
        {.name = "modelview_matrix",   .type = NGPU_TYPE_MAT4},
        {.name = "ngli_rect",          .type = NGPU_TYPE_VEC4},
        {.name = "ngli_uv_scale",      .type = NGPU_TYPE_VEC2},
        {.name = "ngli_stroke_uv_scale", .type = NGPU_TYPE_VEC2},
        {.name = "ngli_margin_px",     .type = NGPU_TYPE_F32},
        {.name = "ngli_margin_uv",     .type = NGPU_TYPE_VEC2},
    };
    ngpu_block_desc_init(gpu_ctx, &s->vert_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ret = ngpu_block_desc_add_fields(&s->vert_block_desc, vert_fields, NGLI_ARRAY_NB(vert_fields));
    if (ret < 0)
        return ret;
    s->vert_block_size = ngpu_block_desc_get_size(&s->vert_block_desc, 0);
    ngli_assert(s->vert_block_size == sizeof(struct drawrect2d_vert_block));

    /* Build static fragment uniform block */
    static const struct ngpu_block_field frag_static_fields[] = {
        {.name = "ngli_rect_size",            .type = NGPU_TYPE_VEC2},
        {.name = "ngli_corner_radius",        .type = NGPU_TYPE_VEC2},
        {.name = "ngli_outline_width",        .type = NGPU_TYPE_F32},
        {.name = "ngli_outline_mode",         .type = NGPU_TYPE_I32},
        {.name = "ngli_opacity",              .type = NGPU_TYPE_F32},
        {.name = "ngli_fill_opacity",         .type = NGPU_TYPE_F32},
        {.name = "ngli_stroke_opacity",       .type = NGPU_TYPE_F32},
        {.name = "ngli_fill_content_wrap",    .type = NGPU_TYPE_I32},
        {.name = "ngli_stroke_content_wrap",  .type = NGPU_TYPE_I32},
        {.name = "ngli_content_zoom",         .type = NGPU_TYPE_F32},
        {.name = "ngli_content_translate",    .type = NGPU_TYPE_VEC2},
        {.name = "ngli_content_orientation",  .type = NGPU_TYPE_VEC2},
        {.name = "ngli_frag_uv_scale",        .type = NGPU_TYPE_VEC2},
        {.name = "ngli_fill_premult",         .type = NGPU_TYPE_I32},
        {.name = "ngli_stroke_premult",       .type = NGPU_TYPE_I32},
        {.name = "ngli_clip_inv",             .type = NGPU_TYPE_VEC4, .count = NGLI_MAX_CLIPS_2D},
        {.name = "ngli_clip_rect",            .type = NGPU_TYPE_VEC4, .count = NGLI_MAX_CLIPS_2D},
        {.name = "ngli_clip_radius",          .type = NGPU_TYPE_VEC4, .count = NGLI_MAX_CLIPS_2D},
        {.name = "ngli_nb_clips",             .type = NGPU_TYPE_I32},
    };

    ngpu_block_desc_init(gpu_ctx, &s->frag_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ret = ngpu_block_desc_add_fields(&s->frag_block_desc, frag_static_fields, NGLI_ARRAY_NB(frag_static_fields));
    if (ret < 0)
        return ret;

    const size_t frag_block_size = ngpu_block_desc_get_size(&s->frag_block_desc, 0);
    ngli_assert(frag_block_size == sizeof(struct drawrect2d_frag_block));

    /* Build user uniform block (dynamic fill/stroke/custom uniforms) */
    const size_t nb_fill_uniforms = fill_paint->uniforms.count;
    const size_t nb_custom_uniforms = fill_paint->custom_uniforms.count;
    const size_t nb_stroke_uniforms = stroke_paint ? stroke_paint->uniforms.count : 0;
    const size_t nb_stroke_custom_uniforms = stroke_paint ? stroke_paint->custom_uniforms.count : 0;

    const int has_user_uniforms = nb_fill_uniforms > 0
                               || nb_custom_uniforms > 0
                               || nb_stroke_uniforms > 0
                               || nb_stroke_custom_uniforms > 0;

    s->user_block_index = -1;
    if (has_user_uniforms) {
        ngpu_block_desc_init(gpu_ctx, &s->user_block_desc, NGPU_BLOCK_LAYOUT_STD140);

        /* Fill prebuilt uniforms: add to user block */
        for (size_t i = 0; i < nb_fill_uniforms; i++) {
            const struct paint_uniform_def *ud = &fill_paint->uniforms.data[i];
            char name[NGPU_ID_LEN];
            ngli_paint_get_resource_name(name, sizeof(name), PAINT_SHADER_ROLE_FILL, ud->name);
            const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, name, ud->type, 0);
            if (field_idx < 0)
                return field_idx;
            const struct prebuilt_uniform pu = {
                .field_index = field_idx,
                .data        = ud->data,
            };
            if (ngli_darray_push(&s->prebuilt_uniforms, pu) < 0)
                return NGL_ERROR_MEMORY;
        }

        /* CustomPaint user uniforms: add to user block */
        for (size_t i = 0; i < nb_custom_uniforms; i++) {
            const struct paint_custom_uniform_def *cu = &fill_paint->custom_uniforms.data[i];
            char name[NGPU_ID_LEN];
            ngli_paint_get_resource_name(name, sizeof(name), PAINT_SHADER_ROLE_FILL, cu->name);
            const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, name, cu->type, 0);
            if (field_idx < 0)
                return field_idx;
            const struct user_uniform uu = {
                .field_index = field_idx,
                .node        = cu->node,
            };
            if (ngli_darray_push(&s->user_uniforms, uu) < 0)
                return NGL_ERROR_MEMORY;
        }

        for (size_t i = 0; i < nb_stroke_uniforms; i++) {
            const struct paint_uniform_def *ud = &stroke_paint->uniforms.data[i];
            char name[NGPU_ID_LEN];
            ngli_paint_get_resource_name(name, sizeof(name), PAINT_SHADER_ROLE_STROKE, ud->name);
            const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, name, ud->type, 0);
            if (field_idx < 0)
                return field_idx;
            const struct prebuilt_uniform pu = {
                .field_index = field_idx,
                .data        = ud->data,
            };
            if (ngli_darray_push(&s->stroke_prebuilt_uniforms, pu) < 0)
                return NGL_ERROR_MEMORY;
        }

        for (size_t i = 0; i < nb_stroke_custom_uniforms; i++) {
            const struct paint_custom_uniform_def *cu = &stroke_paint->custom_uniforms.data[i];
            char name[NGPU_ID_LEN];
            ngli_paint_get_resource_name(name, sizeof(name), PAINT_SHADER_ROLE_STROKE, cu->name);
            const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, name, cu->type, 0);
            if (field_idx < 0)
                return field_idx;
            const struct user_uniform uu = {
                .field_index = field_idx,
                .node        = cu->node,
            };
            if (ngli_darray_push(&s->stroke_user_uniforms, uu) < 0)
                return NGL_ERROR_MEMORY;
        }

        s->user_block_size = ngpu_block_desc_get_size(&s->user_block_desc, 0);
    }

    struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);

    NGLI_DARRAY(struct ngpu_pgcraft_texture) textures = {0};

    if (texture) {
        struct texture_info *texture_info = ngli_node_texture_get_texture_info(texture);
        struct ngpu_pgcraft_texture tex = {
            .type        = ngli_node_texture_get_pgcraft_texture_type(texture),
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .image       = &texture_info->image,
            .format      = texture_info->params.format,
            .clamp_video = texture_info->clamp_video,
            .premult     = texture_info->premult,
        };
        snprintf(tex.name, sizeof(tex.name), "%s", paint_texture_names[PAINT_SHADER_ROLE_FILL]);
        if (ngli_darray_push(&textures, tex) < 0) {
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    const size_t nb_custom_textures = fill_paint->custom_textures.count;
    for (size_t i = 0; i < nb_custom_textures; i++) {
        const struct paint_custom_texture_def *ct = &fill_paint->custom_textures.data[i];
        struct texture_info *texture_info = ngli_node_texture_get_texture_info(ct->texture_node);
        struct ngpu_pgcraft_texture tex = {
            .type        = ngli_node_texture_get_pgcraft_texture_type(ct->texture_node),
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .image       = &texture_info->image,
            .format      = texture_info->params.format,
            .clamp_video = texture_info->clamp_video,
            .premult     = texture_info->premult,
        };
        ngli_paint_get_resource_name(tex.name, sizeof(tex.name), PAINT_SHADER_ROLE_FILL, ct->name);
        if (ngli_darray_push(&textures, tex) < 0) {
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    if (stroke_paint && stroke_paint->texture) {
        struct texture_info *texture_info = ngli_node_texture_get_texture_info(stroke_paint->texture);
        struct ngpu_pgcraft_texture tex = {
            .type        = ngli_node_texture_get_pgcraft_texture_type(stroke_paint->texture),
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .image       = &texture_info->image,
            .format      = texture_info->params.format,
            .clamp_video = texture_info->clamp_video,
            .premult     = texture_info->premult,
        };
        snprintf(tex.name, sizeof(tex.name), "%s", paint_texture_names[PAINT_SHADER_ROLE_STROKE]);
        if (ngli_darray_push(&textures, tex) < 0) {
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    if (stroke_paint) {
        for (size_t i = 0; i < stroke_paint->custom_textures.count; i++) {
            const struct paint_custom_texture_def *ct = &stroke_paint->custom_textures.data[i];
            struct texture_info *texture_info = ngli_node_texture_get_texture_info(ct->texture_node);
            struct ngpu_pgcraft_texture tex = {
                .type        = ngli_node_texture_get_pgcraft_texture_type(ct->texture_node),
                .stage       = NGPU_PROGRAM_STAGE_FRAG,
                .image       = &texture_info->image,
                .format      = texture_info->params.format,
                .clamp_video = texture_info->clamp_video,
                .premult     = texture_info->premult,
            };
            ngli_paint_get_resource_name(tex.name, sizeof(tex.name), PAINT_SHADER_ROLE_STROKE, ct->name);
            if (ngli_darray_push(&textures, tex) < 0) {
                ngli_darray_reset(&textures);
                return NGL_ERROR_MEMORY;
            }
        }
    }

    NGLI_DARRAY(struct ngpu_pgcraft_block) blocks = {0};

    const struct ngpu_pgcraft_block vert_crafter_block = {
        .name          = "vert",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER,
        .stage         = NGPU_PROGRAM_STAGE_VERT,
        .block         = &s->vert_block_desc,
        .buffer        = {.buffer = staging_buf, .size = s->vert_block_size},
    };
    if (ngli_darray_push(&blocks, vert_crafter_block) < 0) {
        ngli_darray_reset(&blocks);
        ngli_darray_reset(&textures);
        return NGL_ERROR_MEMORY;
    }

    const struct ngpu_pgcraft_block frag_crafter_block = {
        .name          = "frag",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER,
        .stage         = NGPU_PROGRAM_STAGE_FRAG,
        .block         = &s->frag_block_desc,
        .buffer        = {.buffer = staging_buf, .size = frag_block_size},
    };
    if (ngli_darray_push(&blocks, frag_crafter_block) < 0) {
        ngli_darray_reset(&blocks);
        ngli_darray_reset(&textures);
        return NGL_ERROR_MEMORY;
    }

    if (has_user_uniforms) {
        const struct ngpu_pgcraft_block user_crafter_block = {
            .name          = "user",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &s->user_block_desc,
            .buffer        = {.buffer = staging_buf, .size = s->user_block_size},
        };
        if (ngli_darray_push(&blocks, user_crafter_block) < 0) {
            ngli_darray_reset(&blocks);
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    const size_t nb_custom_blocks = fill_paint->custom_blocks.count;
    for (size_t i = 0; i < nb_custom_blocks; i++) {
        const struct paint_custom_block_def *cb = &fill_paint->custom_blocks.data[i];
        struct block_info *block_info = cb->node->priv_data;
        struct ngpu_block_desc *block = &block_info->block;
        const size_t block_size = ngpu_block_desc_get_size(block, 0);

        enum ngpu_type type = NGPU_TYPE_UNIFORM_BUFFER;
        if (block->layout == NGPU_BLOCK_LAYOUT_STD430) {
            type = NGPU_TYPE_STORAGE_BUFFER;
        } else {
            const struct ngpu_limits *limits = ngpu_ctx_get_limits(gpu_ctx);
            if (block_size > limits->max_uniform_block_size)
                type = NGPU_TYPE_STORAGE_BUFFER;
        }

        if (type == NGPU_TYPE_UNIFORM_BUFFER)
            ngli_node_block_extend_usage(cb->node, NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        else
            ngli_node_block_extend_usage(cb->node, NGPU_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        const struct ngpu_buffer *buffer = block_info->buffer;
        const size_t buffer_size = buffer ? ngpu_buffer_get_size(buffer) : 0;
        struct ngpu_pgcraft_block crafter_block = {
            .type   = type,
            .stage  = NGPU_PROGRAM_STAGE_FRAG,
            .block  = block,
            .buffer = {.buffer = buffer, .size = buffer_size},
        };
        ngli_paint_get_resource_name(crafter_block.name, sizeof(crafter_block.name),
                                     PAINT_SHADER_ROLE_FILL, cb->name);

        if (ngli_darray_push(&blocks, crafter_block) < 0) {
            ngli_darray_reset(&blocks);
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    if (stroke_paint) {
        for (size_t i = 0; i < stroke_paint->custom_blocks.count; i++) {
            const struct paint_custom_block_def *cb = &stroke_paint->custom_blocks.data[i];
            struct block_info *block_info = cb->node->priv_data;
            struct ngpu_block_desc *block = &block_info->block;
            const size_t block_size = ngpu_block_desc_get_size(block, 0);

            enum ngpu_type type = NGPU_TYPE_UNIFORM_BUFFER;
            if (block->layout == NGPU_BLOCK_LAYOUT_STD430) {
                type = NGPU_TYPE_STORAGE_BUFFER;
            } else {
                const struct ngpu_limits *limits = ngpu_ctx_get_limits(gpu_ctx);
                if (block_size > limits->max_uniform_block_size)
                    type = NGPU_TYPE_STORAGE_BUFFER;
            }

            if (type == NGPU_TYPE_UNIFORM_BUFFER)
                ngli_node_block_extend_usage(cb->node, NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            else
                ngli_node_block_extend_usage(cb->node, NGPU_BUFFER_USAGE_STORAGE_BUFFER_BIT);

            const struct ngpu_buffer *buffer = block_info->buffer;
            const size_t buffer_size = buffer ? ngpu_buffer_get_size(buffer) : 0;
            struct ngpu_pgcraft_block crafter_block = {
                .type   = type,
                .stage  = NGPU_PROGRAM_STAGE_FRAG,
                .block  = block,
                .buffer = {.buffer = buffer, .size = buffer_size},
            };
            ngli_paint_get_resource_name(crafter_block.name, sizeof(crafter_block.name),
                                         PAINT_SHADER_ROLE_STROKE, cb->name);

            if (ngli_darray_push(&blocks, crafter_block) < 0) {
                ngli_darray_reset(&blocks);
                ngli_darray_reset(&textures);
                return NGL_ERROR_MEMORY;
            }
        }
    }

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "ngli_uv",        .type = NGPU_TYPE_VEC2},
        {.name = "ngli_tex_coord", .type = NGPU_TYPE_VEC2},
        {.name = "ngli_stroke_tex_coord", .type = NGPU_TYPE_VEC2},
        {.name = "ngli_clip_pos",  .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/drawrect",
        .vert_base        = s->vert_shader,
        .frag_base        = s->frag_shader,
        .textures         = textures.data,
        .nb_textures      = textures.count,
        .blocks           = blocks.data,
        .nb_blocks        = blocks.count,
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
        .nb_frag_output   = fill_paint->color_output_count,
    };

    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter) {
        ngli_darray_reset(&textures);
        ngli_darray_reset(&blocks);
        return NGL_ERROR_MEMORY;
    }

    ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    ngli_darray_reset(&textures);
    ngli_darray_reset(&blocks);
    if (ret < 0)
        return ret;

    s->vert_block_index = ngpu_pgcraft_get_block_index(s->crafter, "vert", NGPU_PROGRAM_STAGE_VERT);
    s->frag_block_index = ngpu_pgcraft_get_block_index(s->crafter, "frag", NGPU_PROGRAM_STAGE_FRAG);
    if (has_user_uniforms)
        s->user_block_index = ngpu_pgcraft_get_block_index(s->crafter, "user", NGPU_PROGRAM_STAGE_FRAG);

    return 0;
}

static int drawrect2d_prepare(struct ngl_node *node,
                              const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct drawrect2d_priv *s = node->priv_data;
    const struct drawrect2d_opts *o = node->opts;
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    struct ngpu_graphics_state state = NGPU_GRAPHICS_STATE_DEFAULTS;
    int ret = ngli_blend_mode_apply(&state, o->node2d.blend_mode);
    if (ret < 0)
        return ret;

    s->pipeline_compat = ngli_pipeline_compat_create(gpu_ctx);
    if (!s->pipeline_compat)
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

    ret = ngli_pipeline_compat_init(s->pipeline_compat, &params);
    if (ret < 0)
        return ret;

    ret = build_texture_map(s);
    if (ret < 0)
        return ret;

    const struct paint_info *fill_paint = s->fill_paint;
    const size_t nb_cblocks = fill_paint->custom_blocks.count;
    for (size_t i = 0; i < nb_cblocks; i++) {
        const struct paint_custom_block_def *cb = &fill_paint->custom_blocks.data[i];
        const struct block_info *info = cb->node->priv_data;
        char name[NGPU_ID_LEN];
        ngli_paint_get_resource_name(name, sizeof(name), PAINT_SHADER_ROLE_FILL, cb->name);
        const struct block_map bm = {
            .index      = ngpu_pgcraft_get_block_index(s->crafter, name, NGPU_PROGRAM_STAGE_FRAG),
            .info       = info,
            .buffer_rev = SIZE_MAX,
        };
        if (ngli_darray_push(&s->blocks_map, bm) < 0)
            return NGL_ERROR_MEMORY;
    }

    const struct paint_info *stroke_paint = s->stroke_paint;
    if (stroke_paint) {
        for (size_t i = 0; i < stroke_paint->custom_blocks.count; i++) {
            const struct paint_custom_block_def *cb = &stroke_paint->custom_blocks.data[i];
            const struct block_info *info = cb->node->priv_data;
            char name[NGPU_ID_LEN];
            ngli_paint_get_resource_name(name, sizeof(name), PAINT_SHADER_ROLE_STROKE, cb->name);
            const struct block_map bm = {
                .index      = ngpu_pgcraft_get_block_index(s->crafter, name, NGPU_PROGRAM_STAGE_FRAG),
                .info       = info,
                .buffer_rev = SIZE_MAX,
            };
            if (ngli_darray_push(&s->blocks_map, bm) < 0)
                return NGL_ERROR_MEMORY;
        }
    }

    return 0;
}

static int drawrect2d_update(struct ngl_node *node, double t)
{
    struct drawrect2d_priv *s = node->priv_data;
    const struct drawrect2d_opts *o = node->opts;

    int ret = ngli_node_update_children(node, t);
    if (ret < 0)
        return ret;

    const float *rect = ngli_node_get_data_ptr(o->rect_node, o->rect);
    const float *corner_radius = ngli_node_get_data_ptr(o->corner_radius_node, o->corner_radius);
    compute_geometry(s, rect, corner_radius);

    return 0;
}

static void drawrect2d_pre_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct drawrect2d_priv *s = node->priv_data;
    const struct drawrect2d_opts *o = node->opts;

    if (!o->node2d.visible) {
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    ngli_node_pre_draw_children(node);

    struct ngli_mat4 trs_matrix;
    ngli_node2d_compute_trs(node, trs_matrix.m);

    struct ngli_mat4 modelview_matrix;
    const struct ngli_mat4 *prev_matrix = ngli_darray_tail(&ctx->transform_2d_stack);
    ngli_mat4_mul(modelview_matrix.m, prev_matrix->m, trs_matrix.m);

    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    node2d_info->transform_matrix = modelview_matrix;

    /* Expand the AABB by the same margin the vertex shader adds so that
     * screen_aabb covers the actual rendered geometry (stroke + AA fringe).
     * Must mirror drawrect2d_draw()'s margin_px computation. */
    const struct stroke2d_info *stroke = s->stroke;
    const float outer_edge = ngli_stroke2d_get_outer_edge(stroke);
    const float margin_px = outer_edge + 2.f;
    struct aabb expanded_aabb = node2d_info->aabb;
    expanded_aabb.extent[0] += margin_px;
    expanded_aabb.extent[1] += margin_px;
    node2d_info->screen_aabb = ngli_aabb_apply_transform(&expanded_aabb, modelview_matrix.m);
}

static void drawrect2d_draw(struct ngl_node *node)
{
    struct drawrect2d_priv *s = node->priv_data;
    const struct drawrect2d_opts *o = node->opts;

    if (!o->node2d.visible) {
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    ngli_node_draw_children(node);

    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    struct ngli_mat4 trs_matrix;
    ngli_node2d_compute_trs(node, trs_matrix.m);

    struct ngli_mat4 modelview_matrix;
    const struct ngli_mat4 *prev_matrix = ngli_darray_tail(&ctx->transform_2d_stack);
    ngli_mat4_mul(modelview_matrix.m, prev_matrix->m, trs_matrix.m);

    struct pipeline_compat *pl_compat = s->pipeline_compat;

    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    node2d_info->transform_matrix = modelview_matrix;
    node2d_info->screen_aabb = ngli_aabb_apply_transform(&node2d_info->aabb, modelview_matrix.m);

    const struct stroke2d_info *stroke = s->stroke;
    const float stroke_width = ngli_stroke2d_get_width(stroke);

    const struct paint_info *fill_paint = s->fill_paint;
    const struct paint_base_opts *fill_opts = (const struct paint_base_opts *)fill_paint->opts;
    const struct paint_info *stroke_paint = s->stroke_paint;
    const struct paint_base_opts *stroke_opts = stroke_paint ? (const struct paint_base_opts *)stroke_paint->opts : NULL;

    /* Update textures */
    for (size_t i = 0; i < s->textures_map.count; i++)
        ngli_pipeline_compat_update_image(pl_compat, (int32_t)i, s->textures_map.data[i].image, ctx->current_staging_buffer);

    /* Compute texture scaling */
    const int orientation_quarter = ((int)o->content_orientation / 90) & 3;
    const bool orientation_is_transposed = orientation_quarter & 1;
    const float *scale_val = ngli_node_get_data_ptr(o->node2d.scale_node, o->node2d.scale);
    float uv_scale[2];
    float stroke_uv_scale[2];
    compute_texture_uv_scale(fill_paint, s->rect, scale_val, orientation_is_transposed, uv_scale);
    compute_texture_uv_scale(stroke_paint, s->rect, scale_val, false, stroke_uv_scale);

    /* Compute content transform: zoom, translate */
    const float content_zoom_val = *(const float *)ngli_node_get_data_ptr(o->content_zoom_node, &o->content_zoom);
    const float *content_translate_val = ngli_node_get_data_ptr(o->content_translate_node, o->content_translate);
    float content_zoom = content_zoom_val > 0.f ? content_zoom_val : 1.f;
    float content_translate[2] = {content_translate_val[0], content_translate_val[1]};
    if (fill_opts->scaling == PAINT_SCALING_FIT) {
        content_zoom = 1.f;
        const float max_tx = (uv_scale[0] - 1.f) * 0.5f;
        const float max_ty = (uv_scale[1] - 1.f) * 0.5f;
        content_translate[0] = NGLI_MIN(NGLI_MAX(content_translate[0], -max_tx), max_tx);
        content_translate[1] = NGLI_MIN(NGLI_MAX(content_translate[1], -max_ty), max_ty);
    }

    static const float orientation_cos_sin[][2] = {
        [0] = { 1.f,  0.f}, /* 0°   */
        [1] = { 0.f,  1.f}, /* 90°  */
        [2] = {-1.f,  0.f}, /* 180° */
        [3] = { 0.f, -1.f}, /* 270° */
    };

    /* Compute Geometry dilation: expand quad to cover outside stroke + AA border */
    const float outer_edge = ngli_stroke2d_get_outer_edge(stroke);
    const float margin_uv_px = outer_edge + 1.f;
    const float margin_px = margin_uv_px + 1.f;
    const float margin_uv[2] = {
        s->rect[2] > 0.f ? margin_uv_px / s->rect[2] : 0.f,
        s->rect[3] > 0.f ? margin_uv_px / s->rect[3] : 0.f,
    };

    /* Compute opacity: multiply local opacity by cascaded group opacity */
    const float *group_opacity = ngli_darray_tail(&ctx->opacity_2d_stack);
    const float local_opacity = *(const float *)ngli_node_get_data_ptr(o->node2d.opacity_node, &o->node2d.opacity);
    const float final_opacity = local_opacity * *group_opacity;

    /* Fill and push vertex block to staging buffer */
    {
        struct drawrect2d_vert_block vert_data = {0};
        vert_data.projection_matrix = ctx->projection_2d_matrix;
        vert_data.modelview_matrix = modelview_matrix;
        memcpy(vert_data.rect, s->rect, sizeof(vert_data.rect));
        memcpy(vert_data.uv_scale, uv_scale, sizeof(vert_data.uv_scale));
        memcpy(vert_data.stroke_uv_scale, stroke_uv_scale, sizeof(vert_data.stroke_uv_scale));
        vert_data.margin_px = margin_px;
        memcpy(vert_data.margin_uv, margin_uv, sizeof(vert_data.margin_uv));

        const size_t vert_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &vert_data, s->vert_block_size);
        if (vert_offset == SIZE_MAX)
            return;
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->vert_block_index,
                                           staging_buf, vert_offset, s->vert_block_size);
    }

    /* Fill and push static fragment block to staging buffer */
    {
        struct drawrect2d_frag_block frag_data = {0};
        frag_data.rect_size[0]  = s->rect[2];
        frag_data.rect_size[1]  = s->rect[3];
        memcpy(frag_data.corner_radius, s->corner_radius, sizeof(frag_data.corner_radius));
        frag_data.outline_width = stroke_width;
        frag_data.outline_mode  = stroke ? stroke->alignment : NGLI_STROKE2D_ALIGNMENT_CENTER;
        frag_data.opacity       = final_opacity;
        frag_data.fill_opacity  = fill_opts->opacity;
        frag_data.stroke_opacity = stroke_opts ? stroke_opts->opacity : 1.f;
        frag_data.fill_content_wrap = fill_opts->wrap;
        frag_data.stroke_content_wrap = stroke_opts ? stroke_opts->wrap : PAINT_WRAP_DEFAULT;
        frag_data.content_zoom  = content_zoom;
        memcpy(frag_data.content_translate, content_translate, sizeof(frag_data.content_translate));
        memcpy(frag_data.content_orientation, orientation_cos_sin[orientation_quarter], sizeof(frag_data.content_orientation));
        memcpy(frag_data.frag_uv_scale, uv_scale, sizeof(frag_data.frag_uv_scale));
        frag_data.fill_premult = fill_opts->premult;
        frag_data.stroke_premult = stroke_opts ? stroke_opts->premult : 1;
        size_t nb_clips = ctx->nb_clips_2d;
        for (size_t i = 0; i < nb_clips; i++) {
            frag_data.clip_inv[i]    = ctx->clips_2d[i].inv;
            frag_data.clip_rect[i]   = ctx->clips_2d[i].rect;
            frag_data.clip_radius[i] = ctx->clips_2d[i].radius;
        }
        const float *clip_rect = ngli_node_get_data_ptr(o->clip_rect_node, o->clip_rect);
        const float *clip_corner_radius = ngli_node_get_data_ptr(o->clip_corner_radius_node, o->clip_corner_radius);
        struct ngli_clip2d clip;
        if (nb_clips < NGLI_MAX_CLIPS_2D &&
            ngli_node2d_compute_clip(&modelview_matrix, clip_rect, clip_corner_radius, &clip)) {
            frag_data.clip_inv[nb_clips]    = clip.inv;
            frag_data.clip_rect[nb_clips]   = clip.rect;
            frag_data.clip_radius[nb_clips] = clip.radius;
            nb_clips++;
        }
        frag_data.nb_clips = (int32_t)nb_clips;

        const size_t frag_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer,
                                                            &frag_data, sizeof(frag_data));
        if (frag_offset == SIZE_MAX)
            return;
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->frag_block_index,
                                           staging_buf, frag_offset, sizeof(frag_data));
    }

    /* Fill and push user block to staging buffer (if present) */
    if (s->user_block_index >= 0) {
        size_t offset = 0;
        uint8_t *data = ngpu_staging_buffer_reserve(ctx->current_staging_buffer, s->user_block_size, &offset);


        /* Fill prebuilt uniforms */
        const struct ngpu_block_field *fields = s->user_block_desc.fields;
        const struct prebuilt_uniform *pbu = s->prebuilt_uniforms.data;
        for (size_t i = 0; i < s->prebuilt_uniforms.count; i++)
            ngpu_block_field_copy(&fields[pbu[i].field_index], data + fields[pbu[i].field_index].offset, pbu[i].data);

        /* Stroke prebuilt uniforms */
        const struct prebuilt_uniform *stroke_pbu = s->stroke_prebuilt_uniforms.data;
        for (size_t i = 0; i < s->stroke_prebuilt_uniforms.count; i++)
            ngpu_block_field_copy(&fields[stroke_pbu[i].field_index], data + fields[stroke_pbu[i].field_index].offset, stroke_pbu[i].data);

        /* CustomPaint user uniforms */
        for (size_t i = 0; i < s->user_uniforms.count; i++) {
            const struct user_uniform *uu = &s->user_uniforms.data[i];
            ngpu_block_field_copy(&fields[uu->field_index], data + fields[uu->field_index].offset, ngli_node_get_data_ptr(uu->node, NULL));
        }

        for (size_t i = 0; i < s->stroke_user_uniforms.count; i++) {
            const struct user_uniform *uu = &s->stroke_user_uniforms.data[i];
            ngpu_block_field_copy(&fields[uu->field_index], data + fields[uu->field_index].offset, ngli_node_get_data_ptr(uu->node, NULL));
        }

        struct ngpu_buffer *buffer = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->user_block_index,
                                           buffer, offset, s->user_block_size);
    }

    /* CustomPaint block buffer updates */
    struct block_map *blocks = s->blocks_map.data;
    for (size_t i = 0; i < s->blocks_map.count; i++) {
        const struct block_info *info = blocks[i].info;
        if (blocks[i].buffer_rev != info->buffer_rev) {
            ngli_pipeline_compat_update_buffer(pl_compat, blocks[i].index, info->buffer, 0, 0);
            blocks[i].buffer_rev = info->buffer_rev;
        }
    }

    if (!ngpu_ctx_is_render_pass_active(gpu_ctx))
        ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);

    ngpu_ctx_set_viewport(gpu_ctx, &ctx->viewport);
    ngpu_ctx_set_scissor(gpu_ctx, &ctx->scissor);

    ngli_pipeline_compat_draw(pl_compat, 4, 1, 0);
}

static void drawrect2d_uninit(struct ngl_node *node)
{
    struct drawrect2d_priv *s = node->priv_data;
    ngli_pipeline_compat_freep(&s->pipeline_compat);
    ngli_darray_reset(&s->user_uniforms);
    ngli_darray_reset(&s->prebuilt_uniforms);
    ngli_darray_reset(&s->stroke_user_uniforms);
    ngli_darray_reset(&s->stroke_prebuilt_uniforms);
    ngli_darray_reset(&s->textures_map);
    ngli_darray_reset(&s->blocks_map);
    ngpu_block_desc_reset(&s->vert_block_desc);
    ngpu_block_desc_reset(&s->frag_block_desc);
    ngpu_block_desc_reset(&s->user_block_desc);
    ngpu_pgcraft_freep(&s->crafter);
    ngli_freep(&s->vert_shader);
    ngli_freep(&s->frag_shader);
}

const struct node_class ngli_drawrect2d_class = {
    .id        = NGL_NODE_DRAWRECT2D,
    .category  = NGLI_NODE_CATEGORY_DRAW,
    .name      = "DrawRect2D",
    .init      = drawrect2d_init,
    .prepare   = drawrect2d_prepare,
    .update    = drawrect2d_update,
    .pre_draw  = drawrect2d_pre_draw,
    .draw      = drawrect2d_draw,
    .uninit    = drawrect2d_uninit,
    .opts_size = sizeof(struct drawrect2d_opts),
    .priv_size = sizeof(struct drawrect2d_priv),
    .params    = drawrect2d_params,
    .node2d_offset = offsetof(struct drawrect2d_opts, node2d),
    .flags     = NGLI_NODE_FLAG_2D,
    .file      = __FILE__,
};
