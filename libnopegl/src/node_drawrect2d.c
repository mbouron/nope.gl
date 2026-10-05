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
#include "pipeline.h"
#include "shader2d.h"
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
    "vec4 ngli_stroke(const ngl_FragmentInput frag) { return vec4(0.0); }\n";

static const char *const paint_texture_names[PAINT_SHADER_ROLE_NB] = {
    [PAINT_SHADER_ROLE_FILL]   = "ngli_fill_tex",
    [PAINT_SHADER_ROLE_STROKE] = "ngli_stroke_tex",
};

struct uv_transform {
    float lin[4];
    float off[4];
};

#define UV_TRANSFORM_FIELDS(prefix) \
    {.name = prefix "_lin", .type = NGPU_TYPE_VEC4}, \
    {.name = prefix "_off", .type = NGPU_TYPE_VEC4}

struct drawrect2d_vert_block {
    struct ngli_mat4 projection_matrix;
    struct ngli_mat4 modelview_matrix;
    float rect[4];
    float margin_uv[2];
    float margin_px;
    float _pad;
    struct uv_transform content_uv;
    struct uv_transform fill_uv;
    struct uv_transform stroke_uv;
    struct uv_transform fill_tex_uv;
    struct uv_transform stroke_tex_uv;
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
    int32_t fill_premult;
    int32_t stroke_premult;
    float _pad0[3];
    struct uv_transform content_uv;
    struct uv_transform fill_uv;
    struct uv_transform stroke_uv;
    struct ngli_vec4 clip_inv[NGLI_MAX_CLIPS_2D];
    struct ngli_vec4 clip_rect[NGLI_MAX_CLIPS_2D];
    struct ngli_vec4 clip_radius[NGLI_MAX_CLIPS_2D];
    int32_t nb_clips;
    float _pad2[3];
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
    struct ngli_pipeline *pipeline;
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
    struct shader2d shader;
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

static bool is_2d_texture(const struct ngl_node *node)
{
    const enum ngpu_pgcraft_texture_type type = ngli_node_texture_get_pgcraft_texture_type(node);
    return type == NGPU_PGCRAFT_TEXTURE_TYPE_2D || type == NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO;
}

/* The texture a paint shows: TexturePaint.texture or CustomPaint.texture */
static const struct ngl_node *get_scaling_texture(const struct paint_info *paint)
{
    return paint->texture;
}

static void get_texture_uv_transform(const struct paint_info *paint, float *dst)
{
    const struct ngl_node *texture = paint ? get_scaling_texture(paint) : NULL;
    const struct ngli_image *image = NULL;
    if (texture) {
        const struct texture_info *info = ngli_node_texture_get_texture_info(texture);
        image = ngli_image_resource_get(info->resource);
    }
    ngli_image_get_coordinates_scale_offset(image, dst, dst + 2);
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
    const struct ngli_image_params *image_params = ngli_image_get_params(texture_info->image);
    const float tex_w = orientation_is_transposed ? (float)image_params->height : (float)image_params->width;
    const float tex_h = orientation_is_transposed ? (float)image_params->width  : (float)image_params->height;
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

static struct uv_transform compute_uv_transform(const float *cos_sin, const float *scale,
                                                float zoom, const float *translate)
{
    const float co = cos_sin[0], so = cos_sin[1];
    const float dx = scale[0] / zoom, dy = scale[1] / zoom;
    const float ox = translate[0] - 0.5f * dx;
    const float oy = translate[1] - 0.5f * dy;
    return (struct uv_transform) {
        .lin = {co * dx, so * dx, -so * dy, co * dy},
        .off = {co * ox - so * oy + 0.5f, so * ox + co * oy + 0.5f},
    };
}

static struct uv_transform apply_texture_uv_transform(struct uv_transform map, const float *uv_transform)
{
    for (size_t i = 0; i < 2; i++) {
        map.lin[i] *= uv_transform[i];
        map.lin[2 + i] *= uv_transform[i];
        map.off[i] = map.off[i] * uv_transform[i] + uv_transform[2 + i];
    }
    return map;
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

static int register_image_sources(struct drawrect2d_priv *s)
{
    int32_t image_index = 0;
    const struct paint_info *paints[PAINT_SHADER_ROLE_NB] = {
        [PAINT_SHADER_ROLE_FILL]   = s->fill_paint,
        [PAINT_SHADER_ROLE_STROKE] = s->stroke_paint,
    };
    for (size_t i = 0; i < NGLI_ARRAY_NB(paints); i++) {
        const struct paint_info *paint = paints[i];
        if (!paint)
            continue;
        if (paint->texture) {
            const struct texture_info *info = ngli_node_texture_get_texture_info(paint->texture);
            const struct image_resource *source = info->resource;
            int ret = ngli_pipeline_set_image_source(s->pipeline, image_index++, source);
            if (ret < 0 && ret != NGL_ERROR_NOT_FOUND)
                return ret;
        }
        for (size_t j = 0; j < paint->custom_textures.count; j++) {
            const struct paint_custom_texture_def *ct = &paint->custom_textures.data[j];
            if (i == PAINT_SHADER_ROLE_STROKE &&
                ngli_paint_get_custom_resource(s->fill_paint, ct->name))
                continue;
            const struct texture_info *info = ngli_node_texture_get_texture_info(ct->texture_node);
            const struct image_resource *source = info->resource;
            int ret = ngli_pipeline_set_image_source(s->pipeline, image_index++, source);
            if (ret < 0 && ret != NGL_ERROR_NOT_FOUND)
                return ret;
        }
    }
    return 0;
}

static const struct shader2d_role shader_roles[] = {
    [PAINT_SHADER_ROLE_FILL]   = {.name = "fill",   .uv = "ngli_v_rect_uv"},
    [PAINT_SHADER_ROLE_STROKE] = {.name = "stroke", .uv = "ngli_v_rect_uv"},
};

static void write_fragment_input(const struct drawrect2d_priv *s, struct bstr *b,
                                   const struct shader2d_role *shader_role, bool custom)
{
    const char *role = shader_role->name;
    const bool fill = !strcmp(role, "fill");
    const char *content_uv = fill ? "ngli_v_content_uv" : "ngli_v_rect_uv";
    const char *content_lin = fill ? "ngli_content_uv_lin" : "vec4(1.0, 0.0, 0.0, 1.0)";
    const char *content_off = fill ? "ngli_content_uv_off.xy" : "vec2(0.0)";
    ngli_bstr_printf(b, "ngl_FragmentInput ngli_%s_fragment_input() {\n", role);
    ngli_bstr_print(b,  "    ngl_FragmentInput frag;\n"
                        "    frag.rect_uv = ngli_v_rect_uv;\n"
                        "    frag.rect_size = ngli_rect_size;\n"
                        "    frag.canvas_px = ngli_v_clip_pos;\n");
    ngli_bstr_printf(b, "    frag.content_uv = %s;\n", content_uv);
    ngli_bstr_printf(b, "    frag._uv = ngli_v_%s_uv;\n"
                        "    frag._coord = ngli_v_%s_tex_uv;\n"
                        "    frag._lin = ngli_%s_uv_lin;\n"
                        "    frag._off = ngli_%s_uv_off.xy;\n", role, role, role, role);
    ngli_bstr_printf(b, "    frag._content_lin = %s;\n"
                        "    frag._content_off = %s;\n", content_lin, content_off);
    if (custom)
        ngli_shader2d_write_fragment_input_textures(&s->shader, b, shader_role);
    ngli_bstr_print(b, "    return frag;\n}\n");
}

static int drawrect2d_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct drawrect2d_priv *s = node->priv_data;
    const struct drawrect2d_opts *o = node->opts;
    int ret;

    ngli_shader2d_init(&s->shader);

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
    ret = ngli_paint_check_compatible(fill_paint, stroke_paint);
    if (ret < 0)
        return ret;
    s->stroke = stroke;
    s->stroke_paint = stroke_paint;

    const struct ngl_node *texture = fill_paint->texture;

    s->pipeline = NULL;

    const struct paint_info *paints[] = {fill_paint, stroke_paint};
    for (size_t i = 0; i < NGLI_ARRAY_NB(paints); i++) {
        if (!paints[i])
            continue;
        for (size_t j = 0; j < paints[i]->custom_textures.count; j++) {
            const struct paint_custom_texture_def *t = &paints[i]->custom_textures.data[j];
            if (!is_2d_texture(t->texture_node))
                continue;
            ret = ngli_shader2d_add_texture(&s->shader, t->name);
            if (ret < 0)
                return ret;
        }
    }

    struct bstr *bstr = ngli_bstr_create();
    if (!bstr)
        return NGL_ERROR_MEMORY;
    ngli_shader2d_write_header(&s->shader, bstr);
    write_fragment_input(s, bstr, &shader_roles[PAINT_SHADER_ROLE_FILL], fill_paint->custom);
    write_fragment_input(s, bstr, &shader_roles[PAINT_SHADER_ROLE_STROKE], stroke_paint && stroke_paint->custom);
    if (fill_paint->texture)
        ngli_shader2d_write_texture_helpers(bstr, paint_texture_names[PAINT_SHADER_ROLE_FILL]);
    if (stroke_paint && stroke_paint->texture)
        ngli_shader2d_write_texture_helpers(bstr, paint_texture_names[PAINT_SHADER_ROLE_STROKE]);
    const uint32_t all_helper_flags = fill_paint->helper_flags | (stroke_paint ? stroke_paint->helper_flags : 0);
    if (all_helper_flags & PAINT_HELPER_MISC_UTILS) ngli_bstr_print(bstr, helper_misc_utils_glsl);
    if (all_helper_flags & PAINT_HELPER_NOISE)      ngli_bstr_print(bstr, helper_noise_glsl);
    if (all_helper_flags & PAINT_HELPER_SRGB)       ngli_bstr_print(bstr, helper_srgb_glsl);
    ret = ngli_paint_glsl_write(bstr, fill_paint, PAINT_SHADER_ROLE_FILL,
                                fill_paint->color_output_count ? "ngli_colors" : "ngli_color");
    if (ret >= 0 && fill_paint->color_output_count) {
        ngli_bstr_print(bstr, "void main() { ngli_colors(ngli_fill_fragment_input()); }\n");
    } else if (ret >= 0) {
        if (stroke_paint && fill_paint->custom && stroke_paint->custom &&
            fill_paint->texture == stroke_paint->texture &&
            !strcmp(fill_paint->glsl, stroke_paint->glsl)) {
            ngli_bstr_print(bstr, "vec4 ngli_stroke(const ngl_FragmentInput frag) { return ngli_color(frag); }\n");
        } else if (stroke_paint) {
            ret = ngli_paint_glsl_write(bstr, stroke_paint, PAINT_SHADER_ROLE_STROKE, "ngli_stroke");
        } else {
            ngli_bstr_print(bstr, no_stroke_glsl);
        }
        ngli_bstr_print(bstr, drawrect_frag);
    }
    if (ret >= 0)
        ret = ngli_bstr_check(bstr);
    if (ret >= 0) {
        s->frag_shader = ngli_bstr_strdup(bstr);
        if (!s->frag_shader)
            ret = NGL_ERROR_MEMORY;
    }
    ngli_bstr_freep(&bstr);
    if (ret < 0)
        return ret;

    /* Build vertex uniform block */
    static const struct ngpu_block_field vert_fields[] = {
        {.name = "ngli_projection_matrix", .type = NGPU_TYPE_MAT4},
        {.name = "ngli_modelview_matrix", .type = NGPU_TYPE_MAT4},
        {.name = "ngli_rect",          .type = NGPU_TYPE_VEC4},
        {.name = "ngli_margin_uv",     .type = NGPU_TYPE_VEC2},
        {.name = "ngli_margin_px",     .type = NGPU_TYPE_F32},
        /* Unnamed block members need distinct names across stages on GLES. */
        UV_TRANSFORM_FIELDS("ngli_vert_content_uv"),
        UV_TRANSFORM_FIELDS("ngli_vert_fill_uv"),
        UV_TRANSFORM_FIELDS("ngli_vert_stroke_uv"),
        UV_TRANSFORM_FIELDS("ngli_vert_fill_tex_uv"),
        UV_TRANSFORM_FIELDS("ngli_vert_stroke_tex_uv"),
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
        {.name = "ngli_fill_premult",         .type = NGPU_TYPE_I32},
        {.name = "ngli_stroke_premult",       .type = NGPU_TYPE_I32},
        UV_TRANSFORM_FIELDS("ngli_content_uv"),
        UV_TRANSFORM_FIELDS("ngli_fill_uv"),
        UV_TRANSFORM_FIELDS("ngli_stroke_uv"),
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
            ngli_paint_get_builtin_resource_name(name, sizeof(name), PAINT_SHADER_ROLE_FILL, ud->name);
            const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, name, ud->type, 0);
            if (field_idx < 0)
                return field_idx;
            const struct prebuilt_uniform pu = {
                .field_index = field_idx,
                .data        = ud->data,
            };
            if (ngli_darray_try_push(&s->prebuilt_uniforms, pu) < 0)
                return NGL_ERROR_MEMORY;
        }

        /* CustomPaint user uniforms: add to user block */
        for (size_t i = 0; i < nb_custom_uniforms; i++) {
            const struct paint_custom_uniform_def *cu = &fill_paint->custom_uniforms.data[i];
            const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, cu->name, cu->type, 0);
            if (field_idx < 0)
                return field_idx;
            const struct user_uniform uu = {
                .field_index = field_idx,
                .node        = cu->node,
            };
            if (ngli_darray_try_push(&s->user_uniforms, uu) < 0)
                return NGL_ERROR_MEMORY;
        }

        for (size_t i = 0; i < nb_stroke_uniforms; i++) {
            const struct paint_uniform_def *ud = &stroke_paint->uniforms.data[i];
            char name[NGPU_ID_LEN];
            ngli_paint_get_builtin_resource_name(name, sizeof(name), PAINT_SHADER_ROLE_STROKE, ud->name);
            const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, name, ud->type, 0);
            if (field_idx < 0)
                return field_idx;
            const struct prebuilt_uniform pu = {
                .field_index = field_idx,
                .data        = ud->data,
            };
            if (ngli_darray_try_push(&s->stroke_prebuilt_uniforms, pu) < 0)
                return NGL_ERROR_MEMORY;
        }

        for (size_t i = 0; i < nb_stroke_custom_uniforms; i++) {
            const struct paint_custom_uniform_def *cu = &stroke_paint->custom_uniforms.data[i];
            /* A resource both paints bind is declared once, by the fill */
            if (ngli_paint_get_custom_resource(fill_paint, cu->name))
                continue;
            const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, cu->name, cu->type, 0);
            if (field_idx < 0)
                return field_idx;
            const struct user_uniform uu = {
                .field_index = field_idx,
                .node        = cu->node,
            };
            if (ngli_darray_try_push(&s->stroke_user_uniforms, uu) < 0)
                return NGL_ERROR_MEMORY;
        }

        s->user_block_size = ngpu_block_desc_get_size(&s->user_block_desc, 0);
    }

    NGLI_DARRAY(struct ngpu_pgcraft_texture) textures = {0};

    if (texture) {
        struct texture_info *texture_info = ngli_node_texture_get_texture_info(texture);
        struct ngpu_pgcraft_texture tex = {
            .type        = ngli_node_texture_get_pgcraft_texture_type(texture),
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .format      = texture_info->params.format,
            .clamp_video = texture_info->clamp_video,
            .premult     = texture_info->premult,
        };
        snprintf(tex.name, sizeof(tex.name), "%s", paint_texture_names[PAINT_SHADER_ROLE_FILL]);
        if (ngli_darray_try_push(&textures, tex) < 0) {
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
            .format      = texture_info->params.format,
            .clamp_video = texture_info->clamp_video,
            .premult     = texture_info->premult,
        };
        snprintf(tex.name, sizeof(tex.name), "%s", ct->name);
        if (ngli_darray_try_push(&textures, tex) < 0) {
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    if (stroke_paint && stroke_paint->texture) {
        struct texture_info *texture_info = ngli_node_texture_get_texture_info(stroke_paint->texture);
        struct ngpu_pgcraft_texture tex = {
            .type        = ngli_node_texture_get_pgcraft_texture_type(stroke_paint->texture),
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .format      = texture_info->params.format,
            .clamp_video = texture_info->clamp_video,
            .premult     = texture_info->premult,
        };
        snprintf(tex.name, sizeof(tex.name), "%s", paint_texture_names[PAINT_SHADER_ROLE_STROKE]);
        if (ngli_darray_try_push(&textures, tex) < 0) {
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    if (stroke_paint) {
        for (size_t i = 0; i < stroke_paint->custom_textures.count; i++) {
            const struct paint_custom_texture_def *ct = &stroke_paint->custom_textures.data[i];
            if (ngli_paint_get_custom_resource(fill_paint, ct->name))
                continue;
            struct texture_info *texture_info = ngli_node_texture_get_texture_info(ct->texture_node);
            struct ngpu_pgcraft_texture tex = {
                .type        = ngli_node_texture_get_pgcraft_texture_type(ct->texture_node),
                .stage       = NGPU_PROGRAM_STAGE_FRAG,
                .format      = texture_info->params.format,
                .clamp_video = texture_info->clamp_video,
                .premult     = texture_info->premult,
            };
            snprintf(tex.name, sizeof(tex.name), "%s", ct->name);
            if (ngli_darray_try_push(&textures, tex) < 0) {
                ngli_darray_reset(&textures);
                return NGL_ERROR_MEMORY;
            }
        }
    }

    NGLI_DARRAY(struct ngpu_pgcraft_block) blocks = {0};

    const struct ngpu_pgcraft_block vert_crafter_block = {
        .name          = "ngli_vert",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .stage         = NGPU_PROGRAM_STAGE_VERT,
        .block         = &s->vert_block_desc,
    };
    if (ngli_darray_try_push(&blocks, vert_crafter_block) < 0) {
        ngli_darray_reset(&blocks);
        ngli_darray_reset(&textures);
        return NGL_ERROR_MEMORY;
    }

    const struct ngpu_pgcraft_block frag_crafter_block = {
        .name          = "ngli_frag",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .stage         = NGPU_PROGRAM_STAGE_FRAG,
        .block         = &s->frag_block_desc,
    };
    if (ngli_darray_try_push(&blocks, frag_crafter_block) < 0) {
        ngli_darray_reset(&blocks);
        ngli_darray_reset(&textures);
        return NGL_ERROR_MEMORY;
    }

    if (has_user_uniforms) {
        const struct ngpu_pgcraft_block user_crafter_block = {
            .name          = "ngli_user",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &s->user_block_desc,
        };
        if (ngli_darray_try_push(&blocks, user_crafter_block) < 0) {
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

        ret = ngli_node_block_extend_usage_from_type(cb->node, type);
        if (ret < 0) {
            ngli_darray_reset(&blocks);
            ngli_darray_reset(&textures);
            return ret;
        }

        struct ngpu_pgcraft_block crafter_block = {
            .type   = type,
            .stage  = NGPU_PROGRAM_STAGE_FRAG,
            .block  = block,
        };
        snprintf(crafter_block.name, sizeof(crafter_block.name), "%s", cb->name);

        if (ngli_darray_try_push(&blocks, crafter_block) < 0) {
            ngli_darray_reset(&blocks);
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    if (stroke_paint) {
        for (size_t i = 0; i < stroke_paint->custom_blocks.count; i++) {
            const struct paint_custom_block_def *cb = &stroke_paint->custom_blocks.data[i];
            if (ngli_paint_get_custom_resource(fill_paint, cb->name))
                continue;
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

            ret = ngli_node_block_extend_usage_from_type(cb->node, type);
            if (ret < 0) {
                ngli_darray_reset(&blocks);
                ngli_darray_reset(&textures);
                return ret;
            }

            struct ngpu_pgcraft_block crafter_block = {
                .type   = type,
                .stage  = NGPU_PROGRAM_STAGE_FRAG,
                .block  = block,
            };
            snprintf(crafter_block.name, sizeof(crafter_block.name), "%s", cb->name);

            if (ngli_darray_try_push(&blocks, crafter_block) < 0) {
                ngli_darray_reset(&blocks);
                ngli_darray_reset(&textures);
                return NGL_ERROR_MEMORY;
            }
        }
    }

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "ngli_v_rect_uv",       .type = NGPU_TYPE_VEC2},
        {.name = "ngli_v_clip_pos",      .type = NGPU_TYPE_VEC2},
        {.name = "ngli_v_content_uv",    .type = NGPU_TYPE_VEC2},
        {.name = "ngli_v_fill_tex_uv",   .type = NGPU_TYPE_VEC2},
        {.name = "ngli_v_stroke_tex_uv", .type = NGPU_TYPE_VEC2},
        {.name = "ngli_v_fill_uv",       .type = NGPU_TYPE_VEC2},
        {.name = "ngli_v_stroke_uv",     .type = NGPU_TYPE_VEC2},
    };

    struct shader2d_role roles[PAINT_SHADER_ROLE_NB];
    size_t role_count = 0;
    if (fill_paint->custom)
        roles[role_count++] = shader_roles[PAINT_SHADER_ROLE_FILL];
    if (stroke_paint && stroke_paint->custom)
        roles[role_count++] = shader_roles[PAINT_SHADER_ROLE_STROKE];
    struct bstr *vert = ngli_bstr_create();
    if (!vert) {
        ngli_darray_reset(&textures);
        ngli_darray_reset(&blocks);
        return NGL_ERROR_MEMORY;
    }
    ret = ngli_shader2d_write_vertex(&s->shader, vert, drawrect_vert,
                                     vert_out_vars, NGLI_ARRAY_NB(vert_out_vars), roles, role_count);
    if (ret < 0) {
        ngli_bstr_freep(&vert);
        ngli_darray_reset(&textures);
        ngli_darray_reset(&blocks);
        return ret;
    }

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/drawrect",
        .vert_base        = ngli_bstr_strptr(vert),
        .frag_base        = s->frag_shader,
        .textures         = textures.data,
        .nb_textures      = textures.count,
        .blocks           = blocks.data,
        .nb_blocks        = blocks.count,
        .vert_out_vars    = s->shader.varyings.data,
        .nb_vert_out_vars = s->shader.varyings.count,
        .nb_frag_output   = fill_paint->color_output_count,
    };

    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter) {
        ngli_bstr_freep(&vert);
        ngli_darray_reset(&textures);
        ngli_darray_reset(&blocks);
        return NGL_ERROR_MEMORY;
    }

    ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    ngli_bstr_freep(&vert);
    ngli_darray_reset(&textures);
    ngli_darray_reset(&blocks);
    if (ret < 0) {
        LOG(ERROR, "DrawRect2D %s: GLSL source 1 is fill %s; source 2 is stroke %s",
            node->label, o->fill_node->label, stroke ? stroke->paint->label : "(none)");
        return ret;
    }

    s->vert_block_index = ngpu_pgcraft_get_block_index(s->crafter, "ngli_vert", NGPU_PROGRAM_STAGE_VERT);
    s->frag_block_index = ngpu_pgcraft_get_block_index(s->crafter, "ngli_frag", NGPU_PROGRAM_STAGE_FRAG);
    if (has_user_uniforms)
        s->user_block_index = ngpu_pgcraft_get_block_index(s->crafter, "ngli_user", NGPU_PROGRAM_STAGE_FRAG);

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

    s->pipeline = ngli_pipeline_create(gpu_ctx);
    if (!s->pipeline)
        return NGL_ERROR_MEMORY;

    const struct ngli_pipeline_params params = {
        .type = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .state        = state,
            .rt_layout    = *rendertarget_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(s->crafter),
        },
        .program          = ngpu_pgcraft_get_program(s->crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(s->crafter),
        .texture_infos    = ngpu_pgcraft_get_texture_infos(s->crafter),
    };

    ret = ngli_pipeline_init(s->pipeline, &params);
    if (ret < 0)
        return ret;

    ret = register_image_sources(s);
    if (ret < 0)
        return ret;

    const struct paint_info *fill_paint = s->fill_paint;
    const size_t nb_cblocks = fill_paint->custom_blocks.count;
    for (size_t i = 0; i < nb_cblocks; i++) {
        const struct paint_custom_block_def *cb = &fill_paint->custom_blocks.data[i];
        const struct block_info *info = cb->node->priv_data;
        const int32_t index = ngpu_pgcraft_get_block_index(s->crafter, cb->name, NGPU_PROGRAM_STAGE_FRAG);
        ret = ngli_pipeline_set_buffer_source(s->pipeline, index, info->resource, 0, NGPU_BUFFER_WHOLE_SIZE);
        if (ret < 0 && ret != NGL_ERROR_NOT_FOUND)
            return ret;
    }

    const struct paint_info *stroke_paint = s->stroke_paint;
    if (stroke_paint) {
        for (size_t i = 0; i < stroke_paint->custom_blocks.count; i++) {
            const struct paint_custom_block_def *cb = &stroke_paint->custom_blocks.data[i];
            if (ngli_paint_get_custom_resource(fill_paint, cb->name))
                continue;
            const struct block_info *info = cb->node->priv_data;
            const int32_t index = ngpu_pgcraft_get_block_index(s->crafter, cb->name, NGPU_PROGRAM_STAGE_FRAG);
            ret = ngli_pipeline_set_buffer_source(s->pipeline, index, info->resource, 0, NGPU_BUFFER_WHOLE_SIZE);
            if (ret < 0 && ret != NGL_ERROR_NOT_FOUND)
                return ret;
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
    ngli_mat4_mul(modelview_matrix.m, ctx->transform_2d_matrix.m, trs_matrix.m);

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
    const struct pipeline_execution execution = {.staging = node->ctx->current_staging_buffer};

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
    ngli_mat4_mul(modelview_matrix.m, ctx->transform_2d_matrix.m, trs_matrix.m);

    struct ngli_pipeline *pl = s->pipeline;

    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    node2d_info->transform_matrix = modelview_matrix;
    node2d_info->screen_aabb = ngli_aabb_apply_transform(&node2d_info->aabb, modelview_matrix.m);

    const struct stroke2d_info *stroke = s->stroke;
    const float stroke_width = ngli_stroke2d_get_width(stroke);

    const struct paint_info *fill_paint = s->fill_paint;
    const struct paint_base_opts *fill_opts = (const struct paint_base_opts *)fill_paint->opts;
    const struct paint_info *stroke_paint = s->stroke_paint;
    const struct paint_base_opts *stroke_opts = stroke_paint ? (const struct paint_base_opts *)stroke_paint->opts : NULL;

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

    const float *cos_sin = orientation_cos_sin[orientation_quarter];

    float fill_uv_transform[4], stroke_uv_transform[4];
    get_texture_uv_transform(fill_paint, fill_uv_transform);
    get_texture_uv_transform(stroke_paint, stroke_uv_transform);

    const float unit_scale[] = {1.f, 1.f};
    const float zero_translate[] = {0.f, 0.f};
    const struct uv_transform content_uv = compute_uv_transform(cos_sin, unit_scale, content_zoom, content_translate);
    const struct uv_transform fill_uv = compute_uv_transform(cos_sin, uv_scale, content_zoom, content_translate);
    const struct uv_transform stroke_uv = compute_uv_transform(orientation_cos_sin[0], stroke_uv_scale, 1.f, zero_translate);
    const struct uv_transform fill_tex_uv = apply_texture_uv_transform(fill_uv, fill_uv_transform);
    const struct uv_transform stroke_tex_uv = apply_texture_uv_transform(stroke_uv, stroke_uv_transform);

    /* Compute Geometry dilation: expand quad to cover outside stroke + AA border */
    const float outer_edge = ngli_stroke2d_get_outer_edge(stroke);
    const float margin_px = outer_edge + 2.f;
    /* Keep SDF and paint coordinates aligned with the expanded geometry */
    const float margin_uv[2] = {
        s->rect[2] > 0.f ? margin_px / s->rect[2] : 0.f,
        s->rect[3] > 0.f ? margin_px / s->rect[3] : 0.f,
    };

    /* Compute opacity: multiply local opacity by cascaded group opacity */
    const float group_opacity = ctx->opacity_2d;
    const float local_opacity = *(const float *)ngli_node_get_data_ptr(o->node2d.opacity_node, &o->node2d.opacity);
    const float final_opacity = local_opacity * group_opacity;

    /* Fill and push vertex block to staging buffer */
    {
        struct drawrect2d_vert_block vert_data = {0};
        vert_data.projection_matrix = ctx->projection_2d_matrix;
        vert_data.modelview_matrix = modelview_matrix;
        memcpy(vert_data.rect, s->rect, sizeof(vert_data.rect));
        vert_data.margin_px = margin_px;
        vert_data.content_uv = content_uv;
        vert_data.fill_uv = fill_uv;
        vert_data.stroke_uv = stroke_uv;
        vert_data.fill_tex_uv = fill_tex_uv;
        vert_data.stroke_tex_uv = stroke_tex_uv;
        memcpy(vert_data.margin_uv, margin_uv, sizeof(vert_data.margin_uv));

        const size_t vert_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &vert_data, s->vert_block_size);
        if (vert_offset == SIZE_MAX)
            return;
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_update_buffer(pl, s->vert_block_index,
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
        frag_data.content_uv = content_uv;
        frag_data.fill_uv = fill_uv;
        frag_data.stroke_uv = stroke_uv;
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
        ngli_pipeline_update_buffer(pl, s->frag_block_index,
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
        ngli_pipeline_update_buffer(pl, s->user_block_index,
                                    buffer, offset, s->user_block_size);
    }

    if (!ngpu_ctx_is_render_pass_active(gpu_ctx))
        ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);

    ngpu_ctx_set_viewport(gpu_ctx, &ctx->viewport);
    ngpu_ctx_set_scissor(gpu_ctx, &ctx->scissor);

    ngli_pipeline_draw(pl, &execution, 4, 1, 0);
}

static void drawrect2d_release(struct ngl_node *node)
{
    struct drawrect2d_priv *s = node->priv_data;
    ngli_pipeline_discard_resources(s->pipeline);
}

static void drawrect2d_unprepare(struct ngl_node *node)
{
    struct drawrect2d_priv *s = node->priv_data;
    ngli_pipeline_freep(&s->pipeline);
}

static void drawrect2d_uninit(struct ngl_node *node)
{
    struct drawrect2d_priv *s = node->priv_data;
    ngli_darray_reset(&s->user_uniforms);
    ngli_darray_reset(&s->prebuilt_uniforms);
    ngli_darray_reset(&s->stroke_user_uniforms);
    ngli_darray_reset(&s->stroke_prebuilt_uniforms);
    ngpu_block_desc_reset(&s->vert_block_desc);
    ngpu_block_desc_reset(&s->frag_block_desc);
    ngpu_block_desc_reset(&s->user_block_desc);
    ngpu_pgcraft_freep(&s->crafter);
    ngli_freep(&s->frag_shader);
    ngli_shader2d_reset(&s->shader);
}

const struct node_class ngli_drawrect2d_class = {
    .id        = NGL_NODE_DRAWRECT2D,
    .category  = NGLI_NODE_CATEGORY_DRAW,
    .name      = "DrawRect2D",
    .init      = drawrect2d_init,
    .prepare   = drawrect2d_prepare,
    .unprepare = drawrect2d_unprepare,
    .update    = drawrect2d_update,
    .pre_draw  = drawrect2d_pre_draw,
    .draw      = drawrect2d_draw,
    .release   = drawrect2d_release,
    .uninit    = drawrect2d_uninit,
    .opts_size = sizeof(struct drawrect2d_opts),
    .priv_size = sizeof(struct drawrect2d_priv),
    .params    = drawrect2d_params,
    .node2d_offset = offsetof(struct drawrect2d_opts, node2d),
    .flags     = NGLI_NODE_FLAG_2D,
    .file      = __FILE__,
};
