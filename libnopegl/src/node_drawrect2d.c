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
#include "blending.h"
#include "geometry.h"
#include "internal.h"
#include "node2d.h"
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
#include "staging_buffer.h"
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

/* Vertex shader for non-texture fills */
static const char drawrect_vert_notex[] =
    "void main()\n"
    "{\n"
    "    vec2 dir = sign(uvcoord - 0.5);\n"
    "    ngl_out_pos = projection_matrix * modelview_matrix"
    " * vec4(position.xy + dir * ngli_margin_px, 0.0, 1.0);\n"
    "    ngli_uv = uvcoord + dir * ngli_margin_uv;\n"
    "    vec2 adj_uvcoord = (uvcoord - 0.5) * ngli_uv_scale + 0.5;\n"
    "    ngli_tex_coord = adj_uvcoord;\n"
    "}\n";

/* Default stroke base when no stroke node is attached (width=0 → invisible) */
static const struct stroke_base_opts default_stroke_base = {
    .width       = 0.f,
    .mode        = STROKE_INSIDE,
    .dash_length = 0.f,
    .dash_ratio  = 0.5f,
    .dash_offset = 0.f,
    .dash_cap    = STROKE_DASH_CAP_BUTT,
    .opacity     = 1.f,
};

/* ngli_stroke_color() for the no-stroke case: transparent */
static const char no_stroke_glsl[] =
    "vec4 ngli_stroke_color(vec2 uv) { return vec4(0.0); }\n";

static const struct stroke_info default_stroke_info = {
    .glsl = no_stroke_glsl,
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
    NGLI_ALIGNED_MAT(projection_matrix);
    NGLI_ALIGNED_MAT(modelview_matrix);
    float uv_scale[2];
    float margin_px;
    float _pad0;
    float margin_uv[2];
};

struct drawrect2d_frag_block {
    float rect_size[2];
    float corner_radius;
    float outline_width;
    int32_t outline_mode;
    float dash_length;
    float dash_ratio;
    float dash_offset;
    int32_t dash_cap;
    float opacity;
    float fill_opacity;
    float stroke_opacity;
    float clip_min[2];
    float clip_max[2];
    int32_t content_wrap;
    float content_zoom;
    float content_translate[2];
    float content_orientation[2];
    float frag_uv_scale[2];
};

struct drawrect2d_opts {
    float rect[4];
    struct ngl_node *fill_node;
    struct ngl_node *stroke_node;
    float corner_radius;
    struct ngli_node2d_opts node2d;
    float clip_rect[4];
    struct ngl_node *content_zoom_node;
    float content_zoom;
    struct ngl_node *content_translate_node;
    float content_translate[2];
    float content_orientation;
};

/* Tracks a user-supplied uniform node (CustomFill) */
struct user_uniform {
    int32_t field_index;
    const struct ngl_node *node;
    enum ngpu_type type;
};

/* Tracks a prebuilt fill/stroke uniform: reads from opts at draw time */
struct prebuilt_uniform {
    int32_t field_index;
    enum ngpu_type type;
    const uint8_t *base;  /* pointer to fill/stroke node opts */
    size_t offset;        /* byte offset within base */
};

struct drawrect2d_priv {
    struct ngli_node2d_info node2d_info;
    struct ngpu_pgcraft_attribute position_attr;
    struct ngpu_pgcraft_attribute uvcoord_attr;
    uint32_t nb_vertices;
    struct geometry *geometry;
    bool update_geometry;
    struct pipeline_compat *pipeline_compat;
    struct darray textures_map;    // array of struct texture_map
    struct darray blocks_map;      // array of struct block_map
    struct ngpu_pgcraft *crafter;

    /* Uniform blocks */
    struct ngpu_block_desc vert_block_desc;
    size_t vert_block_size;
    int32_t vert_block_index;

    int32_t frag_block_index;

    struct ngpu_block_desc user_block_desc;
    size_t user_block_size;
    int32_t user_block_index;

    struct darray user_uniforms;            // array of struct user_uniform
    struct darray prebuilt_uniforms;        // array of struct prebuilt_uniform
    struct darray stroke_prebuilt_uniforms; // array of struct prebuilt_uniform
    const struct fill_info *fill_info;
    const struct stroke_info *stroke_info;
    char *frag_shader;
};


static int update_rect(struct ngl_node *node)
{
    struct drawrect2d_priv *s = node->priv_data;
    s->update_geometry = true;

    return 0;
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
        .offset      = OFFSET(rect),
        .flags       = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc        = NGLI_DOCSTRING("rect (x, y, width, height)"),
        .update_func = update_rect,
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
        .desc       = NGLI_DOCSTRING("optional outline stroke"),
    },
    {
        .key    = "corner_radius",
        .type   = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(corner_radius),
        .flags  = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc   = NGLI_DOCSTRING("corner radius in pixels"),
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
        .key       = "blending",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(node2d.blending),
        .def_value = {.i32 = NGLI_BLENDING_SRC_OVER},
        .choices   = &ngli_blending_choices,
        .desc      = NGLI_DOCSTRING("define how this node and the current frame buffer are blending together"),
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

static int build_texture_map(struct drawrect2d_priv *s)
{
    const struct ngpu_pgcraft_texture_infos texture_infos = ngpu_pgcraft_get_texture_infos(s->crafter);
    for (size_t i = 0; i < texture_infos.nb_infos; i++) {
        const struct texture_map map = {.image = texture_infos.infos[i].image, .image_rev = SIZE_MAX};
        if (!ngli_darray_push(&s->textures_map, &map))
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

    if (!is_valid_orientation(o->content_orientation)) {
        LOG(ERROR, "content_orientation must be 0, +/-90, +/-180 or +/-270, got %g", o->content_orientation);
        return NGL_ERROR_INVALID_ARG;
    }

    const float half_width = o->rect[2] / 2.0f;
    const float half_height = o->rect[3] / 2.0f;
    s->node2d_info.aabb = (struct aabb) {
        .center = {o->rect[0] + half_width, o->rect[1] + half_height, 0.0f, 1.0f},
        .extent = {half_width, half_height},
    };

    const struct fill_info *fi = (const struct fill_info *)o->fill_node->priv_data;
    s->fill_info = fi;

    const struct stroke_info *si = o->stroke_node
        ? (const struct stroke_info *)o->stroke_node->priv_data
        : &default_stroke_info;
    s->stroke_info = si;

    const struct ngl_node *texture = fi->texture;

    s->pipeline_compat = NULL;
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
        (ret = ngli_geometry_init(s->geometry, NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)) < 0)
        return ret;

    s->position_attr.stride = s->geometry->vertices_layout.stride;
    s->position_attr.offset = s->geometry->vertices_layout.offset;
    s->position_attr.buffer = s->geometry->vertices_buffer;

    s->uvcoord_attr.stride = s->geometry->uvcoords_layout.stride;
    s->uvcoord_attr.offset = s->geometry->uvcoords_layout.offset;
    s->uvcoord_attr.buffer = s->geometry->uvcoords_buffer;

    s->nb_vertices = (uint32_t)s->geometry->vertices_layout.count;

    /* Build fragment shader */
    struct bstr *bstr = ngli_bstr_create();
    if (!bstr)
        return NGL_ERROR_MEMORY;

    const uint32_t all_helper_flags = fi->helper_flags | si->helper_flags;
    if (all_helper_flags & FILL_HELPER_MISC_UTILS) ngli_bstr_print(bstr, helper_misc_utils_glsl);
    if (all_helper_flags & FILL_HELPER_NOISE)      ngli_bstr_print(bstr, helper_noise_glsl);
    if (all_helper_flags & FILL_HELPER_SRGB)       ngli_bstr_print(bstr, helper_srgb_glsl);
    ngli_bstr_print(bstr, fi->glsl);
    if (fi->color_output_count)
        ngli_bstr_print(bstr, "void main() { ngli_colors(ngli_uv, ngli_tex_coord); }\n");
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

    /* Build vertex uniform block */
    static const struct ngpu_block_field vert_fields[] = {
        {.name = "projection_matrix",  .type = NGPU_TYPE_MAT4},
        {.name = "modelview_matrix",   .type = NGPU_TYPE_MAT4},
        {.name = "ngli_uv_scale",      .type = NGPU_TYPE_VEC2},
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
        {.name = "ngli_corner_radius",        .type = NGPU_TYPE_F32},
        {.name = "ngli_outline_width",        .type = NGPU_TYPE_F32},
        {.name = "ngli_outline_mode",         .type = NGPU_TYPE_I32},
        {.name = "ngli_dash_length",          .type = NGPU_TYPE_F32},
        {.name = "ngli_dash_ratio",           .type = NGPU_TYPE_F32},
        {.name = "ngli_dash_offset",          .type = NGPU_TYPE_F32},
        {.name = "ngli_dash_cap",             .type = NGPU_TYPE_I32},
        {.name = "ngli_opacity",              .type = NGPU_TYPE_F32},
        {.name = "ngli_fill_opacity",         .type = NGPU_TYPE_F32},
        {.name = "ngli_stroke_opacity",       .type = NGPU_TYPE_F32},
        {.name = "ngli_clip_min",             .type = NGPU_TYPE_VEC2},
        {.name = "ngli_clip_max",             .type = NGPU_TYPE_VEC2},
        {.name = "ngli_content_wrap",         .type = NGPU_TYPE_I32},
        {.name = "ngli_content_zoom",         .type = NGPU_TYPE_F32},
        {.name = "ngli_content_translate",    .type = NGPU_TYPE_VEC2},
        {.name = "ngli_content_orientation",  .type = NGPU_TYPE_VEC2},
        {.name = "ngli_frag_uv_scale",       .type = NGPU_TYPE_VEC2},
    };

    struct ngpu_block_desc frag_block_desc;
    ngpu_block_desc_init(gpu_ctx, &frag_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ret = ngpu_block_desc_add_fields(&frag_block_desc, frag_static_fields, NGLI_ARRAY_NB(frag_static_fields));
    if (ret < 0) {
        ngpu_block_desc_reset(&frag_block_desc);
        return ret;
    }
    const size_t frag_block_size = ngpu_block_desc_get_size(&frag_block_desc, 0);
    ngli_assert(frag_block_size == sizeof(struct drawrect2d_frag_block));

    /* Build user uniform block (dynamic fill/stroke/custom uniforms) */
    const int has_user_uniforms = fi->nb_uniforms > 0
                               || fi->nb_custom_uniforms > 0
                               || si->nb_uniforms > 0;

    s->user_block_index = -1;
    if (has_user_uniforms) {
        ngpu_block_desc_init(gpu_ctx, &s->user_block_desc, NGPU_BLOCK_LAYOUT_STD140);

        /* Fill prebuilt uniforms: add to user block */
        for (size_t i = 0; i < fi->nb_uniforms; i++) {
            const struct fill_uniform_def *ud = &fi->uniforms[i];
            const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, ud->name, ud->type, 0);
            if (field_idx < 0)
                return field_idx;
            const struct prebuilt_uniform pu = {
                .field_index = field_idx,
                .type        = ud->type,
                .base        = (const uint8_t *)fi->opts,
                .offset      = ud->opts_offset,
            };
            if (!ngli_darray_push(&s->prebuilt_uniforms, &pu))
                return NGL_ERROR_MEMORY;
        }

        /* CustomFill user uniforms: add to user block */
        for (size_t i = 0; i < fi->nb_custom_uniforms; i++) {
            const struct fill_custom_uniform_def *cu = &fi->custom_uniforms[i];
            const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, cu->name, cu->type, 0);
            if (field_idx < 0)
                return field_idx;
            const struct user_uniform uu = {
                .field_index = field_idx,
                .node        = cu->node,
                .type        = cu->type,
            };
            if (!ngli_darray_push(&s->user_uniforms, &uu))
                return NGL_ERROR_MEMORY;
        }

        /* Stroke prebuilt uniforms: add to user block */
        for (size_t i = 0; i < si->nb_uniforms; i++) {
            const struct stroke_uniform_def *ud = &si->uniforms[i];
            const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, ud->name, ud->type, 0);
            if (field_idx < 0)
                return field_idx;
            const struct prebuilt_uniform pu = {
                .field_index = field_idx,
                .type        = ud->type,
                .base        = (const uint8_t *)si->opts,
                .offset      = ud->opts_offset,
            };
            if (!ngli_darray_push(&s->stroke_prebuilt_uniforms, &pu))
                return NGL_ERROR_MEMORY;
        }

        s->user_block_size = ngpu_block_desc_get_size(&s->user_block_desc, 0);
    }

    struct ngpu_buffer *staging_buf = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);

    struct darray textures;
    ngli_darray_init(&textures, sizeof(struct ngpu_pgcraft_texture), 0);

    if (texture) {
        struct texture_info *texture_info = texture->priv_data;
        const struct ngpu_pgcraft_texture tex = {
            .name        = "tex",
            .type        = ngli_node_texture_get_pgcraft_texture_type(texture),
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .image       = &texture_info->image,
            .format      = texture_info->params.format,
            .clamp_video = texture_info->clamp_video,
            .premult     = texture_info->premult,
        };
        if (!ngli_darray_push(&textures, &tex)) {
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    for (size_t i = 0; i < fi->nb_custom_textures; i++) {
        const struct fill_custom_texture_def *ct = &fi->custom_textures[i];
        struct texture_info *texture_info = ngli_node_texture_get_texture_info(ct->texture_node);
        struct ngpu_pgcraft_texture tex = {
            .type        = ngli_node_texture_get_pgcraft_texture_type(ct->texture_node),
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .image       = &texture_info->image,
            .format      = texture_info->params.format,
            .clamp_video = texture_info->clamp_video,
            .premult     = texture_info->premult,
        };
        snprintf(tex.name, sizeof(tex.name), "%s", ct->name);
        if (!ngli_darray_push(&textures, &tex)) {
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    struct darray blocks;
    ngli_darray_init(&blocks, sizeof(struct ngpu_pgcraft_block), 0);

    const struct ngpu_pgcraft_block vert_crafter_block = {
        .name          = "vert",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER,
        .stage         = NGPU_PROGRAM_STAGE_VERT,
        .block         = &s->vert_block_desc,
        .buffer        = {.buffer = staging_buf, .size = s->vert_block_size},
    };
    if (!ngli_darray_push(&blocks, &vert_crafter_block)) {
        ngli_darray_reset(&blocks);
        ngli_darray_reset(&textures);
        ngpu_block_desc_reset(&frag_block_desc);
        return NGL_ERROR_MEMORY;
    }

    const struct ngpu_pgcraft_block frag_crafter_block = {
        .name          = "frag",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER,
        .stage         = NGPU_PROGRAM_STAGE_FRAG,
        .block         = &frag_block_desc,
        .buffer        = {.buffer = staging_buf, .size = frag_block_size},
    };
    if (!ngli_darray_push(&blocks, &frag_crafter_block)) {
        ngli_darray_reset(&blocks);
        ngli_darray_reset(&textures);
        ngpu_block_desc_reset(&frag_block_desc);
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
        if (!ngli_darray_push(&blocks, &user_crafter_block)) {
            ngli_darray_reset(&blocks);
            ngli_darray_reset(&textures);
            ngpu_block_desc_reset(&frag_block_desc);
            return NGL_ERROR_MEMORY;
        }
    }

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

        if (!ngli_darray_push(&blocks, &crafter_block)) {
            ngli_darray_reset(&blocks);
            ngli_darray_reset(&textures);
            ngpu_block_desc_reset(&frag_block_desc);
            return NGL_ERROR_MEMORY;
        }
    }

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "ngli_uv",        .type = NGPU_TYPE_VEC2},
        {.name = "ngli_tex_coord", .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_attribute attributes[] = {
        s->position_attr,
        s->uvcoord_attr,
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/drawrect",
        .vert_base        = texture ? drawrect_vert : drawrect_vert_notex,
        .frag_base        = s->frag_shader,
        .textures         = ngli_darray_data(&textures),
        .nb_textures      = ngli_darray_count(&textures),
        .blocks           = ngli_darray_data(&blocks),
        .nb_blocks        = ngli_darray_count(&blocks),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
        .nb_frag_output   = fi->color_output_count,
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
    ngpu_block_desc_reset(&frag_block_desc);
    if (ret < 0)
        return ret;

    s->vert_block_index = ngpu_pgcraft_get_block_index(s->crafter, "vert", NGPU_PROGRAM_STAGE_VERT);
    s->frag_block_index = ngpu_pgcraft_get_block_index(s->crafter, "frag", NGPU_PROGRAM_STAGE_FRAG);
    if (has_user_uniforms)
        s->user_block_index = ngpu_pgcraft_get_block_index(s->crafter, "user", NGPU_PROGRAM_STAGE_FRAG);

    return 0;
}

static int drawrect2d_prepare(struct ngl_node *node,
                              const struct ngpu_graphics_state *graphics_state,
                              const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct drawrect2d_priv *s = node->priv_data;
    const struct drawrect2d_opts *o = node->opts;
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    ngli_darray_init(&s->textures_map, sizeof(struct texture_map), 0);
    ngli_darray_init(&s->blocks_map, sizeof(struct block_map), 0);

    struct ngpu_graphics_state state = *graphics_state;
    int ret = ngli_blending_apply_preset(&state, o->node2d.blending);
    if (ret < 0)
        return ret;

    s->pipeline_compat = ngli_pipeline_compat_create(gpu_ctx);
    if (!s->pipeline_compat)
        return NGL_ERROR_MEMORY;

    const struct pipeline_compat_params params = {
        .type = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology     = s->geometry->topology,
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

    const struct fill_info *fi = s->fill_info;
    for (size_t i = 0; i < fi->nb_custom_blocks; i++) {
        const struct fill_custom_block_def *cb = &fi->custom_blocks[i];
        const struct block_info *info = cb->node->priv_data;
        const struct block_map bm = {
            .index      = ngpu_pgcraft_get_block_index(s->crafter, cb->name, NGPU_PROGRAM_STAGE_FRAG),
            .info       = info,
            .buffer_rev = SIZE_MAX,
        };
        if (!ngli_darray_push(&s->blocks_map, &bm))
            return NGL_ERROR_MEMORY;
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

    if (!s->update_geometry)
        return 0;

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
    ret = ngpu_buffer_upload(s->geometry->vertices_buffer, vertices, 0, sizeof(vertices));
    if (ret < 0)
        return ret;

    const float half_w = w / 2.0f;
    const float half_h = h / 2.0f;
    s->node2d_info.aabb = (struct aabb) {
        .center = {x + half_w, y + half_h, 0.0f, 1.0f},
        .extent = {half_w, half_h},
    };

    s->update_geometry = false;

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

    NGLI_ALIGNED_MAT(trs_matrix);
    ngli_node2d_compute_trs(node, trs_matrix);

    NGLI_ALIGNED_MAT(modelview_matrix);
    const float *prev_matrix = ngli_darray_tail(&ctx->transform_2d_stack);
    ngli_mat4_mul(modelview_matrix, prev_matrix, trs_matrix);

    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    memcpy(node2d_info->transform_matrix, modelview_matrix, sizeof(node2d_info->transform_matrix));
    node2d_info->screen_aabb = ngli_aabb_apply_transform(&node2d_info->aabb, modelview_matrix);
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

    NGLI_ALIGNED_MAT(trs_matrix);
    ngli_node2d_compute_trs(node, trs_matrix);

    NGLI_ALIGNED_MAT(modelview_matrix);
    const float *prev_matrix = ngli_darray_tail(&ctx->transform_2d_stack);
    ngli_mat4_mul(modelview_matrix, prev_matrix, trs_matrix);

    struct pipeline_compat *pl_compat = s->pipeline_compat;

    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    memcpy(node2d_info->transform_matrix, modelview_matrix, sizeof(node2d_info->transform_matrix));
    node2d_info->screen_aabb = ngli_aabb_apply_transform(&node2d_info->aabb, modelview_matrix);

    const struct stroke_base_opts *so = o->stroke_node
        ? (const struct stroke_base_opts *)o->stroke_node->opts : &default_stroke_base;

    const struct fill_info *fi = s->fill_info;
    const struct fill_base_opts *fo = (const struct fill_base_opts *)fi->opts;

    /* Update textures */
    struct texture_map *texture_map = ngli_darray_data(&s->textures_map);
    for (size_t i = 0; i < ngli_darray_count(&s->textures_map); i++)
        ngli_pipeline_compat_update_image(pl_compat, (int32_t)i, texture_map[i].image, ctx->current_staging_buffer);

    /* Compute texture scaling */
    const int orientation_quarter = ((int)o->content_orientation / 90) & 3;
    const int orientation_is_transposed = orientation_quarter & 1;
    float uv_scale[] = {1.f, 1.f};
    if (fo->scaling != FILL_SCALING_NONE && ngli_darray_count(&s->textures_map) > 0) {
        const struct image *image = texture_map[0].image;
        const float tex_w = orientation_is_transposed ? (float)image->params.height : (float)image->params.width;
        const float tex_h = orientation_is_transposed ? (float)image->params.width  : (float)image->params.height;
        const float *scale_val = ngli_node_get_data_ptr(o->node2d.scale_node, o->node2d.scale);
        const float scaled_w = o->rect[2] * scale_val[0];
        const float scaled_h = o->rect[3] * scale_val[1];
        if (tex_w > 0.f && tex_h > 0.f && scaled_w > 0.f && scaled_h > 0.f) {
            const float ratio = (scaled_w / scaled_h) / (tex_w / tex_h);
            if (fo->scaling == FILL_SCALING_FIT) {
                uv_scale[0] = ratio > 1.f ? ratio : 1.f;
                uv_scale[1] = ratio < 1.f ? 1.f / ratio : 1.f;
            } else { /* FILL_SCALING_FILL */
                uv_scale[0] = ratio < 1.f ? ratio : 1.f;
                uv_scale[1] = ratio > 1.f ? 1.f / ratio : 1.f;
            }
        }
    }

    /* Compute content transform: zoom, translate */
    const float content_zoom_val = *(const float *)ngli_node_get_data_ptr(o->content_zoom_node, &o->content_zoom);
    const float *content_translate_val = ngli_node_get_data_ptr(o->content_translate_node, o->content_translate);
    float content_zoom = content_zoom_val > 0.f ? content_zoom_val : 1.f;
    float content_translate[2] = {content_translate_val[0], content_translate_val[1]};
    if (fo->scaling == FILL_SCALING_FIT) {
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
    const float outer_edge = (so->mode == STROKE_OUTSIDE) ? so->width :
                             (so->mode == STROKE_CENTER)  ? so->width * 0.5f : 0.f;
    const float margin_uv_px = outer_edge + 1.f;
    const float margin_px = margin_uv_px + 1.f;
    const float margin_uv[2] = {
        o->rect[2] > 0.f ? margin_uv_px / o->rect[2] : 0.f,
        o->rect[3] > 0.f ? margin_uv_px / o->rect[3] : 0.f,
    };

    /* Compute opacity: multiply local opacity by cascaded group opacity */
    const float *group_opacity = ngli_darray_tail(&ctx->opacity_2d_stack);
    const float local_opacity = *(const float *)ngli_node_get_data_ptr(o->node2d.opacity_node, &o->node2d.opacity);
    const float final_opacity = local_opacity * *group_opacity;

    /* Compute clip rect: convert pixel coordinates to UV space */
    float clip_min[2] = {-1e9f, -1e9f};
    float clip_max[2] = { 1e9f,  1e9f};
    if (o->clip_rect[2] > 0.f && o->clip_rect[3] > 0.f && o->rect[2] > 0.f && o->rect[3] > 0.f) {
        clip_min[0] = (o->clip_rect[0] - o->rect[0]) / o->rect[2];
        clip_min[1] = (o->clip_rect[1] - o->rect[1]) / o->rect[3];
        clip_max[0] = clip_min[0] + o->clip_rect[2] / o->rect[2];
        clip_max[1] = clip_min[1] + o->clip_rect[3] / o->rect[3];
    }

    /* Fill and push vertex block to staging buffer */
    {
        struct drawrect2d_vert_block vert_data = {0};
        memcpy(vert_data.projection_matrix, ctx->projection_2d_matrix, sizeof(vert_data.projection_matrix));
        memcpy(vert_data.modelview_matrix, modelview_matrix, sizeof(vert_data.modelview_matrix));
        memcpy(vert_data.uv_scale, uv_scale, sizeof(vert_data.uv_scale));
        vert_data.margin_px = margin_px;
        memcpy(vert_data.margin_uv, margin_uv, sizeof(vert_data.margin_uv));

        const size_t vert_offset = ngli_staging_buffer_push(ctx->current_staging_buffer, &vert_data, s->vert_block_size);
        if (vert_offset == SIZE_MAX)
            return;
        struct ngpu_buffer *staging_buf = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->vert_block_index,
                                           staging_buf, vert_offset, s->vert_block_size);
    }

    /* Fill and push static fragment block to staging buffer */
    {
        struct drawrect2d_frag_block frag_data = {0};
        frag_data.rect_size[0]  = o->rect[2];
        frag_data.rect_size[1]  = o->rect[3];
        frag_data.corner_radius = o->corner_radius;
        frag_data.outline_width = so->width;
        frag_data.outline_mode  = so->mode;
        frag_data.dash_length   = so->dash_length;
        frag_data.dash_ratio    = so->dash_ratio;
        frag_data.dash_offset   = so->dash_offset;
        frag_data.dash_cap      = so->dash_cap;
        frag_data.opacity       = final_opacity;
        frag_data.fill_opacity  = fo->opacity;
        frag_data.stroke_opacity = so->opacity;
        memcpy(frag_data.clip_min, clip_min, sizeof(frag_data.clip_min));
        memcpy(frag_data.clip_max, clip_max, sizeof(frag_data.clip_max));
        frag_data.content_wrap  = fo->wrap;
        frag_data.content_zoom  = content_zoom;
        memcpy(frag_data.content_translate, content_translate, sizeof(frag_data.content_translate));
        memcpy(frag_data.content_orientation, orientation_cos_sin[orientation_quarter], sizeof(frag_data.content_orientation));
        memcpy(frag_data.frag_uv_scale, uv_scale, sizeof(frag_data.frag_uv_scale));

        const size_t frag_offset = ngli_staging_buffer_push(ctx->current_staging_buffer,
                                                            &frag_data, sizeof(frag_data));
        if (frag_offset == SIZE_MAX)
            return;
        struct ngpu_buffer *staging_buf = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->frag_block_index,
                                           staging_buf, frag_offset, sizeof(frag_data));
    }

    /* Fill and push user block to staging buffer (if present) */
    if (s->user_block_index >= 0) {
        size_t offset = 0;
        uint8_t *data = ngli_staging_buffer_reserve(ctx->current_staging_buffer, s->user_block_size, &offset);


        /* Fill prebuilt uniforms */
        const struct ngpu_block_field *fields = s->user_block_desc.fields;
        const struct prebuilt_uniform *pbu = ngli_darray_data(&s->prebuilt_uniforms);
        for (size_t i = 0; i < ngli_darray_count(&s->prebuilt_uniforms); i++)
            ngpu_block_field_copy(&fields[pbu[i].field_index], data + fields[pbu[i].field_index].offset, pbu[i].base + pbu[i].offset);

        /* Stroke prebuilt uniforms */
        const struct prebuilt_uniform *stroke_pbu = ngli_darray_data(&s->stroke_prebuilt_uniforms);
        for (size_t i = 0; i < ngli_darray_count(&s->stroke_prebuilt_uniforms); i++)
            ngpu_block_field_copy(&fields[stroke_pbu[i].field_index], data + fields[stroke_pbu[i].field_index].offset, stroke_pbu[i].base + stroke_pbu[i].offset);

        /* CustomFill user uniforms */
        const struct user_uniform *user_uniforms = ngli_darray_data(&s->user_uniforms);
        for (size_t i = 0; i < ngli_darray_count(&s->user_uniforms); i++) {
            const struct user_uniform *uu = &user_uniforms[i];
            ngpu_block_field_copy(&fields[uu->field_index], data + fields[uu->field_index].offset, node_get_data_ptr(uu->node, uu->type));
        }

        struct ngpu_buffer *buffer = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->user_block_index,
                                           buffer, offset, s->user_block_size);
    }

    /* CustomFill block buffer updates */
    struct block_map *blocks = ngli_darray_data(&s->blocks_map);
    for (size_t i = 0; i < ngli_darray_count(&s->blocks_map); i++) {
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
}

static void drawrect2d_uninit(struct ngl_node *node)
{
    struct drawrect2d_priv *s = node->priv_data;
    ngli_pipeline_compat_freep(&s->pipeline_compat);
    ngli_darray_reset(&s->user_uniforms);
    ngli_darray_reset(&s->prebuilt_uniforms);
    ngli_darray_reset(&s->stroke_prebuilt_uniforms);
    ngli_darray_reset(&s->textures_map);
    ngli_darray_reset(&s->blocks_map);
    ngli_darray_reset(&s->user_uniforms);
    ngli_darray_reset(&s->prebuilt_uniforms);
    ngli_darray_reset(&s->stroke_prebuilt_uniforms);
    ngpu_block_desc_reset(&s->vert_block_desc);
    ngpu_block_desc_reset(&s->user_block_desc);
    ngpu_pgcraft_freep(&s->crafter);
    ngli_geometry_freep(&s->geometry);
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
