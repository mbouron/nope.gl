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

#include "blending.h"
#include "geometry.h"
#include "internal.h"
#include "math_utils.h"
#include "log.h"
#include <ngpu/ngpu.h>
#include "node_block.h"
#include "node_fill.h"
#include "node_uniform.h"
#include "node_stroke.h"
#include "node_texture.h"
#include "node_uniform.h"
#include "pipeline_compat.h"
#include "transforms.h"
#include "utils/bstr.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

/* GLSL fragments as string */
#include "drawrect_frag.h"
#include "drawrect_vert.h"
#include "helper_misc_utils_glsl.h"
#include "helper_noise_glsl.h"
#include "helper_srgb_glsl.h"

/*
 * Vertex shader for non-texture fills: identical to drawrect_vert but
 * computes tex_coord directly from adj_uvcoord without tex_coord_matrix,
 * which is only injected by pgcraft when a texture is registered.
 */
static const char drawrect_vert_notex[] =
    "void main()\n"
    "{\n"
    "    vec2 dir = sign(uvcoord - 0.5);\n"
    "    ngl_out_pos = projection_matrix * modelview_matrix"
    " * vec4(position.xy + dir * margin_px, 0.0, 1.0);\n"
    "    uv = uvcoord + dir * margin_uv;\n"
    "    vec2 adj_uvcoord = (uvcoord - 0.5) * uv_scale + 0.5;\n"
    "    tex_coord = adj_uvcoord;\n"
    "}\n";

/* Default stroke base when no stroke node is attached (width=0 → invisible) */
static const struct stroke_base_opts default_stroke_base = {
    .width       = 0.f,
    .mode        = STROKE_INSIDE,
    .dash_length = 0.f,
    .dash_ratio  = 0.5f,
    .dash_offset = 0.f,
    .dash_cap    = STROKE_DASH_CAP_BUTT,
};

/* ngl_stroke_color() for the no-stroke case: transparent */
static const char no_stroke_glsl[] =
    "vec4 ngl_stroke_color(vec2 uv) { return vec4(0.0); }\n";

static const struct stroke_info default_stroke_info = {
    .glsl = no_stroke_glsl,
};

/* Tracks a user-supplied uniform node (CustomFill) */
struct user_uniform {
    int32_t index;
    const struct ngl_node *node;
    enum ngpu_type type;
};

/* Tracks a prebuilt fill uniform: reads from fill_node opts at draw time */
struct prebuilt_uniform {
    int32_t index;
    enum ngpu_type type;
    const uint8_t *base;  /* pointer to fill node opts */
    size_t offset;        /* byte offset within base */
};

static const void *node_get_data_ptr(const struct ngl_node *node, enum ngpu_type type)
{
    if (node->cls->category == NGLI_NODE_CATEGORY_VARIABLE) {
        const struct variable_info *var = node->priv_data;
        return var->data;
    }
    ngli_assert(node->cls->flags & NGLI_NODE_FLAG_LIVECTL);
    const struct livectl *ctl = (const struct livectl *)((const uint8_t *)node->opts + node->cls->livectl_offset);
    switch (type) {
    case NGPU_TYPE_F32:
    case NGPU_TYPE_VEC2:
    case NGPU_TYPE_VEC3:
    case NGPU_TYPE_VEC4:   return ctl->val.f;
    case NGPU_TYPE_MAT4:   return ctl->val.m;
    case NGPU_TYPE_I32:
    case NGPU_TYPE_IVEC2:
    case NGPU_TYPE_IVEC3:
    case NGPU_TYPE_IVEC4:
    case NGPU_TYPE_BOOL:   return ctl->val.i;
    case NGPU_TYPE_U32:
    case NGPU_TYPE_UVEC2:
    case NGPU_TYPE_UVEC3:
    case NGPU_TYPE_UVEC4:  return ctl->val.u;
    default:
        ngli_assert(0);
        return NULL;
    }
}

struct texture_map {
    const struct image *image;
    size_t image_rev;
};

struct block_map {
    int32_t index;
    const struct block_info *info;
    size_t buffer_rev;
};

struct pipeline_desc {
    struct pipeline_compat *pipeline_compat;
    struct darray textures_map;    /* struct texture_map */
    struct darray reframing_nodes; /* struct ngl_node * */
    struct darray blocks_map;      /* struct block_map */
};

struct drawrect_opts {
    float rect[4];
    struct ngl_node *fill_node;
    struct ngl_node *stroke_node;
    enum ngli_blending blending;
    float translate[3];
    float scale[3];
    float rotate_angle;
    float anchor[3];
    float corner_radius;
    struct ngl_node *opacity_node;
    float opacity;
    float clip_rect[4];
    float content_zoom;
    float content_translate[2];
};

struct drawrect_priv {
    struct ngpu_pgcraft_attribute position_attr;
    struct ngpu_pgcraft_attribute uvcoord_attr;
    uint32_t nb_vertices;
    struct geometry *geometry;
    struct darray pipeline_descs;
    struct ngpu_pgcraft *crafter;
    int32_t modelview_matrix_index;
    int32_t projection_matrix_index;
    int32_t rect_size_index;
    int32_t corner_radius_index;
    int32_t outline_width_index;
    int32_t outline_mode_index;
    int32_t wrap_index;
    int32_t uv_scale_index;
    int32_t frag_uv_scale_index;
    int32_t margin_px_index;
    int32_t margin_uv_index;
    int32_t dash_length_index;
    int32_t dash_ratio_index;
    int32_t dash_offset_index;
    int32_t dash_cap_index;
    int32_t opacity_index;
    int32_t clip_min_index;
    int32_t clip_max_index;
    int32_t content_zoom_index;
    int32_t content_translate_index;
    struct darray user_uniforms;          /* struct user_uniform (CustomFill only) */
    struct darray prebuilt_uniforms;      /* struct prebuilt_uniform (fill) */
    struct darray stroke_prebuilt_uniforms; /* struct prebuilt_uniform (stroke) */
    const struct fill_info *fill_info;
    const struct stroke_info *stroke_info;
    char *frag_shader;                    /* dynamically built, owned */
};

#define OFFSET(x) offsetof(struct drawrect_opts, x)
static const struct node_param drawrect_params[] = {
    {
        .key  = "rect",
        .type = NGLI_PARAM_TYPE_VEC4,
        .offset = OFFSET(rect),
        .desc = NGLI_DOCSTRING("rect (x, y, width, height)"),
    },
    {
        .key        = "fill",
        .type       = NGLI_PARAM_TYPE_NODE,
        .offset     = OFFSET(fill_node),
        .node_types = (const uint32_t[]){
            NGL_NODE_COLORFILL,
            NGL_NODE_TEXTUREFILL,
            NGL_NODE_GRADIENTFILL,
            NGL_NODE_GRADIENT4FILL,
            NGL_NODE_NOISEFILL,
            NGL_NODE_CUSTOMFILL,
            NGLI_NODE_NONE,
        },
        .flags = NGLI_PARAM_FLAG_NON_NULL,
        .desc  = NGLI_DOCSTRING("fill paint applied inside the rect"),
    },
    {
        .key        = "stroke",
        .type       = NGLI_PARAM_TYPE_NODE,
        .offset     = OFFSET(stroke_node),
        .node_types = (const uint32_t[]){
            NGL_NODE_STROKE,
            NGL_NODE_STROKEGRADIENT,
            NGL_NODE_STROKEGRADIENT4,
            NGLI_NODE_NONE,
        },
        .desc = NGLI_DOCSTRING("optional outline stroke"),
    },
    {
        .key     = "blending",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(blending),
        .choices = &ngli_blending_choices,
        .desc    = NGLI_DOCSTRING(
            "define how this node and the current frame buffer are blending together"),
    },
    {
        .key    = "translate",
        .type   = NGLI_PARAM_TYPE_VEC3,
        .offset = OFFSET(translate),
        .flags  = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc   = NGLI_DOCSTRING("translation vector"),
    },
    {
        .key       = "scale",
        .type      = NGLI_PARAM_TYPE_VEC3,
        .offset    = OFFSET(scale),
        .def_value = {.vec={1.f, 1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("scale factors"),
    },
    {
        .key    = "rotate_angle",
        .type   = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(rotate_angle),
        .flags  = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc   = NGLI_DOCSTRING("rotation angle in degrees (around the Z axis)"),
    },
    {
        .key       = "anchor",
        .type      = NGLI_PARAM_TYPE_VEC3,
        .offset    = OFFSET(anchor),
        .def_value = {.vec={NAN, NAN, NAN}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("pivot point for rotation and scale; defaults to the center of the rect"),
    },
    {
        .key    = "corner_radius",
        .type   = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(corner_radius),
        .flags  = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc   = NGLI_DOCSTRING("corner radius in pixels"),
    },
    {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(opacity_node),
        .def_value = {.f32 = 1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("opacity of the rectangle (0 for fully transparent, 1 for fully opaque)"),
    },
    {
        .key    = "clip_rect",
        .type   = NGLI_PARAM_TYPE_VEC4,
        .offset = OFFSET(clip_rect),
        .flags  = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc   = NGLI_DOCSTRING("clipping rectangle (x, y, width, height) in pixel coordinates; "
                                  "when width or height is 0 clipping is disabled"),
    },
    {
        .key       = "content_zoom",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(content_zoom),
        .def_value = {.f32 = 1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("zoom factor applied to the fill content (>1 zooms in; "
                                    "for fit scaling mode zoom is ignored)"),
    },
    {
        .key   = "content_translate",
        .type  = NGLI_PARAM_TYPE_VEC2,
        .offset = OFFSET(content_translate),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("UV-space translation of the fill content; "
                                "for fit scaling mode the translation is clamped to keep "
                                "the content within the DrawRect bounds"),
    },
    {NULL},
};
#undef OFFSET

static void reset_pipeline_desc(void *user_arg, void *data)
{
    struct pipeline_desc *desc = data;
    ngli_pipeline_compat_freep(&desc->pipeline_compat);
    ngli_darray_reset(&desc->textures_map);
    ngli_darray_reset(&desc->reframing_nodes);
    ngli_darray_reset(&desc->blocks_map);
}

static int build_texture_map(struct drawrect_priv *s, struct pipeline_desc *desc)
{
    const struct ngpu_pgcraft_compat_info *info = ngpu_pgcraft_get_compat_info(s->crafter);
    for (size_t i = 0; i < info->nb_texture_infos; i++) {
        const struct texture_map map = {.image = info->images[i], .image_rev = SIZE_MAX};
        if (!ngli_darray_push(&desc->textures_map, &map))
            return NGL_ERROR_MEMORY;
    }
    return 0;
}

static int drawrect_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct drawrect_priv *s = node->priv_data;
    const struct drawrect_opts *o = node->opts;

    const struct fill_info *fi = (const struct fill_info *)o->fill_node->priv_data;
    s->fill_info = fi;

    const struct stroke_info *si = o->stroke_node
        ? (const struct stroke_info *)o->stroke_node->priv_data
        : &default_stroke_info;
    s->stroke_info = si;

    const struct ngl_node *texture_node = fi->texture_transform
        ? ngli_transform_get_leaf_node(fi->texture_transform) : NULL;

    ngli_darray_init(&s->pipeline_descs, sizeof(struct pipeline_desc), 0);
    ngli_darray_set_free_func(&s->pipeline_descs, reset_pipeline_desc, NULL);
    ngli_darray_init(&s->user_uniforms, sizeof(struct user_uniform), 0);
    ngli_darray_init(&s->prebuilt_uniforms, sizeof(struct prebuilt_uniform), 0);
    ngli_darray_init(&s->stroke_prebuilt_uniforms, sizeof(struct prebuilt_uniform), 0);

    snprintf(s->position_attr.name, sizeof(s->position_attr.name), "position");
    s->position_attr.type   = NGPU_TYPE_VEC3;
    s->position_attr.format = NGPU_FORMAT_R32G32B32_SFLOAT;

    snprintf(s->uvcoord_attr.name, sizeof(s->uvcoord_attr.name), "uvcoord");
    s->uvcoord_attr.type   = NGPU_TYPE_VEC2;
    s->uvcoord_attr.format = NGPU_FORMAT_R32G32_SFLOAT;

    const float x = o->rect[0];
    const float y = o->rect[1];
    const float w = o->rect[2];
    const float h = o->rect[3];
    const float vertices[] = {
        x,   y,   0.f,
        x+w, y,   0.f,
        x,   y+h, 0.f,
        x+w, y+h, 0.f,
    };

    const float uvcoords[] = {
        0.f, 0.f,
        1.f, 0.f,
        0.f, 1.f,
        1.f, 1.f,
    };

    s->geometry = ngli_geometry_create(gpu_ctx);
    if (!s->geometry)
        return NGL_ERROR_MEMORY;

    int ret;
    if ((ret = ngli_geometry_set_vertices(s->geometry, 4, vertices)) < 0 ||
        (ret = ngli_geometry_set_uvcoords(s->geometry, 4, uvcoords)) < 0 ||
        (ret = ngli_geometry_init(s->geometry, NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) < 0))
        return ret;

    s->position_attr.stride = s->geometry->vertices_layout.stride;
    s->position_attr.offset = s->geometry->vertices_layout.offset;
    s->position_attr.buffer = s->geometry->vertices_buffer;

    s->uvcoord_attr.stride = s->geometry->uvcoords_layout.stride;
    s->uvcoord_attr.offset = s->geometry->uvcoords_layout.offset;
    s->uvcoord_attr.buffer = s->geometry->uvcoords_buffer;

    s->nb_vertices = (uint32_t)s->geometry->vertices_layout.count;

    /* Build fragment shader: helpers + ngl_color()/ngl_colors() + stroke + base frag */
    struct bstr *bstr = ngli_bstr_create();
    if (!bstr)
        return NGL_ERROR_MEMORY;

    const uint32_t all_helper_flags = fi->helper_flags | si->helper_flags;
    if (all_helper_flags & FILL_HELPER_MISC_UTILS) ngli_bstr_print(bstr, helper_misc_utils_glsl);
    if (all_helper_flags & FILL_HELPER_NOISE)      ngli_bstr_print(bstr, helper_noise_glsl);
    if (all_helper_flags & FILL_HELPER_SRGB)       ngli_bstr_print(bstr, helper_srgb_glsl);
    ngli_bstr_print(bstr, fi->glsl);
    if (fi->nb_frag_output)
        ngli_bstr_print(bstr, "void main() { ngl_colors(uv, tex_coord); }\n");
    else {
        ngli_bstr_print(bstr, si->glsl);
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

    /* Build dynamic uniforms array: static base + fill source-specific */
    static const struct ngpu_pgcraft_uniform static_uniforms[] = {
        {.name="projection_matrix", .type=NGPU_TYPE_MAT4, .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="modelview_matrix",  .type=NGPU_TYPE_MAT4, .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="uv_scale",          .type=NGPU_TYPE_VEC2, .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="margin_px",         .type=NGPU_TYPE_F32,  .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="margin_uv",         .type=NGPU_TYPE_VEC2, .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="rect_size",         .type=NGPU_TYPE_VEC2, .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="corner_radius",     .type=NGPU_TYPE_F32,  .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="outline_width",     .type=NGPU_TYPE_F32,  .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="outline_mode",      .type=NGPU_TYPE_I32,  .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="wrap",              .type=NGPU_TYPE_I32,  .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="dash_length",       .type=NGPU_TYPE_F32,  .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="dash_ratio",        .type=NGPU_TYPE_F32,  .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="dash_offset",       .type=NGPU_TYPE_F32,  .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="dash_cap",          .type=NGPU_TYPE_I32,  .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="opacity",           .type=NGPU_TYPE_F32,  .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="clip_min",          .type=NGPU_TYPE_VEC2, .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="clip_max",          .type=NGPU_TYPE_VEC2, .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="content_zoom",      .type=NGPU_TYPE_F32,  .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="content_translate", .type=NGPU_TYPE_VEC2, .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="frag_uv_scale",    .type=NGPU_TYPE_VEC2, .stage=NGPU_PROGRAM_STAGE_FRAG},
    };

    struct darray uniforms_arr;
    ngli_darray_init(&uniforms_arr, sizeof(struct ngpu_pgcraft_uniform), 0);
    for (size_t i = 0; i < NGLI_ARRAY_NB(static_uniforms); i++) {
        if (!ngli_darray_push(&uniforms_arr, &static_uniforms[i])) {
            ngli_darray_reset(&uniforms_arr);
            return NGL_ERROR_MEMORY;
        }
    }

    /* Fill prebuilt uniforms */
    for (size_t i = 0; i < fi->nb_uniforms; i++) {
        const struct fill_uniform_def *ud = &fi->uniforms[i];
        struct ngpu_pgcraft_uniform u = {.type=ud->type, .stage=NGPU_PROGRAM_STAGE_FRAG};
        snprintf(u.name, sizeof(u.name), "%s", ud->name);
        if (!ngli_darray_push(&uniforms_arr, &u)) {
            ngli_darray_reset(&uniforms_arr);
            return NGL_ERROR_MEMORY;
        }
    }

    /* CustomFill user uniforms */
    for (size_t i = 0; i < fi->nb_custom_uniforms; i++) {
        const struct fill_custom_uniform_def *cu = &fi->custom_uniforms[i];
        struct ngpu_pgcraft_uniform u = {.type=cu->type, .stage=NGPU_PROGRAM_STAGE_FRAG};
        snprintf(u.name, sizeof(u.name), "%s", cu->name);
        if (!ngli_darray_push(&uniforms_arr, &u)) {
            ngli_darray_reset(&uniforms_arr);
            return NGL_ERROR_MEMORY;
        }
    }

    /* Stroke prebuilt uniforms */
    for (size_t i = 0; i < si->nb_uniforms; i++) {
        const struct stroke_uniform_def *ud = &si->uniforms[i];
        struct ngpu_pgcraft_uniform u = {.type=ud->type, .stage=NGPU_PROGRAM_STAGE_FRAG};
        snprintf(u.name, sizeof(u.name), "%s", ud->name);
        if (!ngli_darray_push(&uniforms_arr, &u)) {
            ngli_darray_reset(&uniforms_arr);
            return NGL_ERROR_MEMORY;
        }
    }

    /* Texture setup */
    struct darray textures_arr;
    ngli_darray_init(&textures_arr, sizeof(struct ngpu_pgcraft_texture), 0);

    if (texture_node) {
        struct texture_info *texture_info = texture_node->priv_data;
        const struct ngpu_pgcraft_texture tex = {
            .name        = "tex",
            .type        = ngli_node_texture_get_pgcraft_texture_type(texture_node),
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .image       = &texture_info->image,
            .format      = texture_info->params.format,
            .clamp_video = texture_info->clamp_video,
            .premult     = texture_info->premult,
        };
        if (!ngli_darray_push(&textures_arr, &tex)) {
            ngli_darray_reset(&textures_arr);
            ngli_darray_reset(&uniforms_arr);
            return NGL_ERROR_MEMORY;
        }
    }

    /* CustomFill user textures */
    for (size_t i = 0; i < fi->nb_custom_textures; i++) {
        const struct fill_custom_texture_def *ct = &fi->custom_textures[i];
        const struct ngl_node *leaf = ngli_transform_get_leaf_node(ct->texture_node);
        struct texture_info *texture_info = ngli_node_texture_get_texture_info(leaf);
        struct ngpu_pgcraft_texture tex = {
            .type        = ngli_node_texture_get_pgcraft_texture_type(leaf),
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .image       = &texture_info->image,
            .format      = texture_info->params.format,
            .clamp_video = texture_info->clamp_video,
            .premult     = texture_info->premult,
        };
        snprintf(tex.name, sizeof(tex.name), "%s", ct->name);
        if (!ngli_darray_push(&textures_arr, &tex)) {
            ngli_darray_reset(&textures_arr);
            ngli_darray_reset(&uniforms_arr);
            return NGL_ERROR_MEMORY;
        }
    }

    /* CustomFill user blocks */
    struct darray blocks_arr;
    ngli_darray_init(&blocks_arr, sizeof(struct ngpu_pgcraft_block), 0);

    for (size_t i = 0; i < fi->nb_custom_blocks; i++) {
        const struct fill_custom_block_def *cb = &fi->custom_blocks[i];
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
        snprintf(crafter_block.name, sizeof(crafter_block.name), "%s", cb->name);

        if (!ngli_darray_push(&blocks_arr, &crafter_block)) {
            ngli_darray_reset(&blocks_arr);
            ngli_darray_reset(&textures_arr);
            ngli_darray_reset(&uniforms_arr);
            return NGL_ERROR_MEMORY;
        }
    }

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "uv",        .type = NGPU_TYPE_VEC2},
        {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_attribute attributes[] = {
        s->position_attr,
        s->uvcoord_attr,
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/drawrect",
        .vert_base        = texture_node ? drawrect_vert : drawrect_vert_notex,
        .frag_base        = s->frag_shader,
        .uniforms         = ngli_darray_data(&uniforms_arr),
        .nb_uniforms      = ngli_darray_count(&uniforms_arr),
        .textures         = ngli_darray_data(&textures_arr),
        .nb_textures      = ngli_darray_count(&textures_arr),
        .blocks           = ngli_darray_data(&blocks_arr),
        .nb_blocks        = ngli_darray_count(&blocks_arr),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
        .nb_frag_output   = fi->nb_frag_output,
    };

    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter) {
        ngli_darray_reset(&uniforms_arr);
        ngli_darray_reset(&textures_arr);
        ngli_darray_reset(&blocks_arr);
        return NGL_ERROR_MEMORY;
    }

    ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    ngli_darray_reset(&uniforms_arr);
    ngli_darray_reset(&textures_arr);
    ngli_darray_reset(&blocks_arr);
    if (ret < 0)
        return ret;

    s->modelview_matrix_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "modelview_matrix", NGPU_PROGRAM_STAGE_VERT);
    s->projection_matrix_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "projection_matrix", NGPU_PROGRAM_STAGE_VERT);
    s->rect_size_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "rect_size", NGPU_PROGRAM_STAGE_FRAG);
    s->corner_radius_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "corner_radius", NGPU_PROGRAM_STAGE_FRAG);
    s->outline_width_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "outline_width", NGPU_PROGRAM_STAGE_FRAG);
    s->outline_mode_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "outline_mode", NGPU_PROGRAM_STAGE_FRAG);
    s->wrap_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "wrap", NGPU_PROGRAM_STAGE_FRAG);
    s->uv_scale_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "uv_scale", NGPU_PROGRAM_STAGE_VERT);
    s->frag_uv_scale_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "frag_uv_scale", NGPU_PROGRAM_STAGE_FRAG);
    s->margin_px_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "margin_px", NGPU_PROGRAM_STAGE_VERT);
    s->margin_uv_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "margin_uv", NGPU_PROGRAM_STAGE_VERT);
    s->dash_length_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "dash_length", NGPU_PROGRAM_STAGE_FRAG);
    s->dash_ratio_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "dash_ratio", NGPU_PROGRAM_STAGE_FRAG);
    s->dash_offset_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "dash_offset", NGPU_PROGRAM_STAGE_FRAG);
    s->dash_cap_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "dash_cap", NGPU_PROGRAM_STAGE_FRAG);
    s->opacity_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "opacity", NGPU_PROGRAM_STAGE_FRAG);
    s->clip_min_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "clip_min", NGPU_PROGRAM_STAGE_FRAG);
    s->clip_max_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "clip_max", NGPU_PROGRAM_STAGE_FRAG);
    s->content_zoom_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "content_zoom", NGPU_PROGRAM_STAGE_FRAG);
    s->content_translate_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "content_translate", NGPU_PROGRAM_STAGE_FRAG);

    /* Register fill prebuilt uniform indices */
    for (size_t i = 0; i < fi->nb_uniforms; i++) {
        const struct fill_uniform_def *ud = &fi->uniforms[i];
        const struct prebuilt_uniform pu = {
            .index  = ngpu_pgcraft_get_uniform_index(s->crafter, ud->name, NGPU_PROGRAM_STAGE_FRAG),
            .type   = ud->type,
            .base   = (const uint8_t *)fi->opts,
            .offset = ud->opts_offset,
        };
        if (!ngli_darray_push(&s->prebuilt_uniforms, &pu))
            return NGL_ERROR_MEMORY;
    }

    /* Register custom user uniform indices (CustomFill) */
    for (size_t i = 0; i < fi->nb_custom_uniforms; i++) {
        const struct fill_custom_uniform_def *cu = &fi->custom_uniforms[i];
        const struct user_uniform uu = {
            .index = ngpu_pgcraft_get_uniform_index(s->crafter, cu->name, NGPU_PROGRAM_STAGE_FRAG),
            .node  = cu->node,
            .type  = cu->type,
        };
        if (!ngli_darray_push(&s->user_uniforms, &uu))
            return NGL_ERROR_MEMORY;
    }

    /* Register stroke prebuilt uniform indices */
    for (size_t i = 0; i < si->nb_uniforms; i++) {
        const struct stroke_uniform_def *ud = &si->uniforms[i];
        const struct prebuilt_uniform pu = {
            .index  = ngpu_pgcraft_get_uniform_index(s->crafter, ud->name, NGPU_PROGRAM_STAGE_FRAG),
            .type   = ud->type,
            .base   = (const uint8_t *)si->opts,
            .offset = ud->opts_offset,
        };
        if (!ngli_darray_push(&s->stroke_prebuilt_uniforms, &pu))
            return NGL_ERROR_MEMORY;
    }

    return 0;
}

static struct pipeline_desc *create_pipeline_desc(struct ngl_node *node)
{
    struct drawrect_priv *s = node->priv_data;
    struct rnode *rnode = node->ctx->rnode_pos;

    struct pipeline_desc *desc = ngli_darray_push(&s->pipeline_descs, NULL);
    if (!desc)
        return NULL;

    rnode->id = ngli_darray_count(&s->pipeline_descs) - 1;

    ngli_darray_init(&desc->textures_map, sizeof(struct texture_map), 0);
    ngli_darray_init(&desc->reframing_nodes, sizeof(struct ngl_node *), 0);
    ngli_darray_init(&desc->blocks_map, sizeof(struct block_map), 0);

    return desc;
}

static int drawrect_prepare(struct ngl_node *node)
{
    struct drawrect_priv *s = node->priv_data;
    const struct drawrect_opts *o = node->opts;
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct rnode *rnode = ctx->rnode_pos;

    struct pipeline_desc *desc = create_pipeline_desc(node);
    if (!desc)
        return NGL_ERROR_MEMORY;

    struct ngpu_graphics_state state = rnode->graphics_state;
    int ret = ngli_blending_apply_preset(&state, o->blending);
    if (ret < 0)
        return ret;

    desc->pipeline_compat = ngli_pipeline_compat_create(gpu_ctx);
    if (!desc->pipeline_compat)
        return NGL_ERROR_MEMORY;

    const struct pipeline_compat_params params = {
        .type = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology     = s->geometry->topology,
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

    ret = ngli_node_prepare_children(node);
    if (ret < 0)
        return ret;

    ret = ngli_pipeline_compat_init(desc->pipeline_compat, &params);
    if (ret < 0)
        return ret;

    ret = build_texture_map(s, desc);
    if (ret < 0)
        return ret;

    const struct fill_info *fi = s->fill_info;
    if (fi->texture_transform && !ngli_darray_push(&desc->reframing_nodes, &fi->texture_transform))
        return NGL_ERROR_MEMORY;

    /* CustomFill texture reframing */
    for (size_t i = 0; i < fi->nb_custom_textures; i++) {
        if (!ngli_darray_push(&desc->reframing_nodes, &fi->custom_textures[i].texture_node))
            return NGL_ERROR_MEMORY;
    }

    /* CustomFill block map */
    for (size_t i = 0; i < fi->nb_custom_blocks; i++) {
        const struct fill_custom_block_def *cb = &fi->custom_blocks[i];
        const struct block_info *info = cb->node->priv_data;
        const struct block_map bm = {
            .index      = ngpu_pgcraft_get_block_index(s->crafter, cb->name, NGPU_PROGRAM_STAGE_FRAG),
            .info       = info,
            .buffer_rev = SIZE_MAX,
        };
        if (!ngli_darray_push(&desc->blocks_map, &bm))
            return NGL_ERROR_MEMORY;
    }

    return 0;
}

static void drawrect_draw(struct ngl_node *node)
{
    struct drawrect_priv *s = node->priv_data;
    const struct drawrect_opts *o = node->opts;

    ngli_node_draw_children(node);

    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    const float anchor[3] = {
        isnan(o->anchor[0]) ? o->rect[0] + o->rect[2] * 0.5f : o->anchor[0],
        isnan(o->anchor[1]) ? o->rect[1] + o->rect[3] * 0.5f : o->anchor[1],
        isnan(o->anchor[2]) ? 0.f                            : o->anchor[2],
    };

    NGLI_ALIGNED_MAT(SM);
    NGLI_ALIGNED_MAT(RM);
    NGLI_ALIGNED_MAT(TM);
    ngli_mat4_scale(SM, o->scale[0], o->scale[1], o->scale[2], anchor);
    float z_axis[3] = {0.f, 0.f, 1.f};
    ngli_mat4_rotate(RM, NGLI_DEG2RAD(o->rotate_angle), z_axis, anchor);
    ngli_mat4_translate(TM, o->translate[0], o->translate[1], o->translate[2]);
    NGLI_ALIGNED_MAT(trs_matrix);
    ngli_mat4_mul(trs_matrix, RM, SM);
    ngli_mat4_mul(trs_matrix, TM, trs_matrix);

    float *next_matrix = ngli_darray_push(&ctx->modelview_matrix_stack, NULL);
    if (!next_matrix)
        return;
    const float *prev_matrix = next_matrix - 4 * 4;
    ngli_mat4_mul(next_matrix, prev_matrix, trs_matrix);

    struct pipeline_desc *descs = ngli_darray_data(&s->pipeline_descs);
    struct pipeline_desc *desc = &descs[ctx->rnode_pos->id];
    struct pipeline_compat *pl_compat = desc->pipeline_compat;

    const float *modelview_matrix = ngli_darray_tail(&ctx->modelview_matrix_stack);

    NGLI_ALIGNED_MAT(base_projection_matrix);
    NGLI_ALIGNED_MAT(projection_matrix);
    const struct ngl_scene_params *scene_params = ngl_scene_get_params(node->scene);
    const float w = scene_params->width  > 0 ? (float)scene_params->width  : (float)ctx->viewport.width;
    const float h = scene_params->height > 0 ? (float)scene_params->height : (float)ctx->viewport.height;
    ngpu_ctx_get_projection_matrix(gpu_ctx, base_projection_matrix);
    ngli_mat4_orthographic(projection_matrix, 0.f, w, h, 0.f, -1.f, 1.f);
    ngli_mat4_mul(projection_matrix, base_projection_matrix, projection_matrix);

    ngli_pipeline_compat_update_uniform(pl_compat, s->modelview_matrix_index, modelview_matrix);
    ngli_pipeline_compat_update_uniform(pl_compat, s->projection_matrix_index, projection_matrix);

    const float rect_size[] = {o->rect[2], o->rect[3]};
    ngli_pipeline_compat_update_uniform(pl_compat, s->rect_size_index, rect_size);
    ngli_pipeline_compat_update_uniform(pl_compat, s->corner_radius_index, &o->corner_radius);

    /* Stroke structural uniforms (width/mode/dash — common to all stroke types) */
    const struct stroke_base_opts *so = o->stroke_node
        ? (const struct stroke_base_opts *)o->stroke_node->opts : &default_stroke_base;
    ngli_pipeline_compat_update_uniform(pl_compat, s->outline_width_index, &so->width);
    ngli_pipeline_compat_update_uniform(pl_compat, s->outline_mode_index,  &so->mode);
    ngli_pipeline_compat_update_uniform(pl_compat, s->dash_length_index,   &so->dash_length);
    ngli_pipeline_compat_update_uniform(pl_compat, s->dash_ratio_index,    &so->dash_ratio);
    ngli_pipeline_compat_update_uniform(pl_compat, s->dash_offset_index,   &so->dash_offset);
    ngli_pipeline_compat_update_uniform(pl_compat, s->dash_cap_index,      &so->dash_cap);

    /* Texture wrap */
    const struct fill_info *fi = s->fill_info;
    ngli_pipeline_compat_update_uniform(pl_compat, s->wrap_index, &fi->wrap);

    /* Fill prebuilt uniforms */
    const struct prebuilt_uniform *pbu = ngli_darray_data(&s->prebuilt_uniforms);
    for (size_t i = 0; i < ngli_darray_count(&s->prebuilt_uniforms); i++)
        ngli_pipeline_compat_update_uniform(pl_compat, pbu[i].index, pbu[i].base + pbu[i].offset);

    /* Stroke prebuilt uniforms (color) */
    const struct prebuilt_uniform *stroke_pbu = ngli_darray_data(&s->stroke_prebuilt_uniforms);
    for (size_t i = 0; i < ngli_darray_count(&s->stroke_prebuilt_uniforms); i++)
        ngli_pipeline_compat_update_uniform(pl_compat, stroke_pbu[i].index, stroke_pbu[i].base + stroke_pbu[i].offset);

    /* CustomFill user uniforms */
    const struct user_uniform *user_uniforms = ngli_darray_data(&s->user_uniforms);
    for (size_t i = 0; i < ngli_darray_count(&s->user_uniforms); i++) {
        const struct user_uniform *uu = &user_uniforms[i];
        ngli_pipeline_compat_update_uniform(pl_compat, uu->index, node_get_data_ptr(uu->node, uu->type));
    }

    struct texture_map *texture_map = ngli_darray_data(&desc->textures_map);

    /* Texture scaling (uv_scale) */
    float uv_scale[] = {1.f, 1.f};
    if (fi->scaling != FILL_SCALING_NONE && ngli_darray_count(&desc->textures_map) > 0) {
        const struct image *image = texture_map[0].image;
        const float tex_w = (float)image->params.width;
        const float tex_h = (float)image->params.height;
        if (tex_w > 0.f && tex_h > 0.f && o->rect[2] > 0.f && o->rect[3] > 0.f) {
            const float ratio = (o->rect[2] / o->rect[3]) / (tex_w / tex_h);
            if (fi->scaling == FILL_SCALING_FIT) {
                uv_scale[0] = ratio > 1.f ? ratio : 1.f;
                uv_scale[1] = ratio < 1.f ? 1.f / ratio : 1.f;
            } else { /* FILL_SCALING_FILL */
                uv_scale[0] = ratio < 1.f ? ratio : 1.f;
                uv_scale[1] = ratio > 1.f ? 1.f / ratio : 1.f;
            }
        }
    }
    ngli_pipeline_compat_update_uniform(pl_compat, s->uv_scale_index, uv_scale);
    ngli_pipeline_compat_update_uniform(pl_compat, s->frag_uv_scale_index, uv_scale);

    /* Content transform: zoom+translate on the fill content.
     * For fit scaling: zoom is ignored (forced to 1.0) and translate is clamped
     * to (uv_scale - 1) / 2 per axis so the texture stays within the DrawRect. */
    float content_zoom = o->content_zoom > 0.f ? o->content_zoom : 1.f;
    float content_translate[2] = {o->content_translate[0], o->content_translate[1]};
    if (fi->scaling == FILL_SCALING_FIT) {
        content_zoom = 1.f;
        const float max_tx = (uv_scale[0] - 1.f) * 0.5f;
        const float max_ty = (uv_scale[1] - 1.f) * 0.5f;
        content_translate[0] = NGLI_MIN(NGLI_MAX(content_translate[0], -max_tx), max_tx);
        content_translate[1] = NGLI_MIN(NGLI_MAX(content_translate[1], -max_ty), max_ty);
    }
    ngli_pipeline_compat_update_uniform(pl_compat, s->content_zoom_index,      &content_zoom);
    ngli_pipeline_compat_update_uniform(pl_compat, s->content_translate_index,  content_translate);

    /* Geometry dilation: expand quad to cover outside stroke + fwidth() border */
    const float outer_edge = (so->mode == STROKE_OUTSIDE) ? so->width :
                             (so->mode == STROKE_CENTER)  ? so->width * 0.5f : 0.f;
    const float margin_px = outer_edge + 1.f;
    const float margin_uv[2] = {
        o->rect[2] > 0.f ? margin_px / o->rect[2] : 0.f,
        o->rect[3] > 0.f ? margin_px / o->rect[3] : 0.f,
    };
    ngli_pipeline_compat_update_uniform(pl_compat, s->margin_px_index, &margin_px);
    ngli_pipeline_compat_update_uniform(pl_compat, s->margin_uv_index, margin_uv);

    /* Opacity */
    const void *opacity_data = ngli_node_get_data_ptr(o->opacity_node, &o->opacity);
    ngli_pipeline_compat_update_uniform(pl_compat, s->opacity_index, opacity_data);

    /* Clip rect: convert pixel coordinates to UV space */
    float clip_min[2] = {-1e9f, -1e9f};
    float clip_max[2] = { 1e9f,  1e9f};
    if (o->clip_rect[2] > 0.f && o->clip_rect[3] > 0.f && o->rect[2] > 0.f && o->rect[3] > 0.f) {
        clip_min[0] = (o->clip_rect[0] - o->rect[0]) / o->rect[2];
        clip_min[1] = (o->clip_rect[1] - o->rect[1]) / o->rect[3];
        clip_max[0] = clip_min[0] + o->clip_rect[2] / o->rect[2];
        clip_max[1] = clip_min[1] + o->clip_rect[3] / o->rect[3];
    }
    ngli_pipeline_compat_update_uniform(pl_compat, s->clip_min_index, clip_min);
    ngli_pipeline_compat_update_uniform(pl_compat, s->clip_max_index, clip_max);

    const struct ngl_node **reframing_nodes = ngli_darray_data(&desc->reframing_nodes);
    for (size_t i = 0; i < ngli_darray_count(&desc->textures_map); i++) {
        if (texture_map[i].image_rev != texture_map[i].image->rev) {
            ngli_pipeline_compat_update_image(pl_compat, (int32_t)i, texture_map[i].image);
            texture_map[i].image_rev = texture_map[i].image->rev;
        }

        NGLI_ALIGNED_MAT(reframing_matrix);
        ngli_transform_chain_compute(reframing_nodes[i], reframing_matrix);
        ngli_pipeline_compat_apply_reframing_matrix(pl_compat, (int32_t)i, texture_map[i].image, reframing_matrix);
    }

    /* CustomFill block buffer updates */
    struct block_map *blocks = ngli_darray_data(&desc->blocks_map);
    for (size_t i = 0; i < ngli_darray_count(&desc->blocks_map); i++) {
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

    ngli_pipeline_compat_draw(pl_compat, s->nb_vertices, 1, 0);

    ngli_darray_pop(&ctx->modelview_matrix_stack);
}

static void drawrect_uninit(struct ngl_node *node)
{
    struct drawrect_priv *s = node->priv_data;
    ngli_darray_reset(&s->pipeline_descs);
    ngli_darray_reset(&s->user_uniforms);
    ngli_darray_reset(&s->prebuilt_uniforms);
    ngli_darray_reset(&s->stroke_prebuilt_uniforms);
    ngpu_pgcraft_freep(&s->crafter);
    ngli_geometry_freep(&s->geometry);
    ngli_freep(&s->frag_shader);
}

const struct node_class ngli_drawrect_class = {
    .id        = NGL_NODE_DRAWRECT,
    .category  = NGLI_NODE_CATEGORY_DRAW,
    .name      = "DrawRect",
    .init      = drawrect_init,
    .prepare   = drawrect_prepare,
    .update    = ngli_node_update_children,
    .draw      = drawrect_draw,
    .uninit    = drawrect_uninit,
    .opts_size = sizeof(struct drawrect_opts),
    .priv_size = sizeof(struct drawrect_priv),
    .params    = drawrect_params,
    .file      = __FILE__,
};
