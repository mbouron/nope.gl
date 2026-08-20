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

#include <stddef.h>
#include <stdio.h>

#include "internal.h"
#include "log.h"
#include "node_paint.h"
#include "node_texture.h"
#include "node_uniform.h"
#include "utils/bstr.h"
#include "utils/memory.h"
#include "nopegl/nopegl.h"

#include <ngpu/ngpu.h>

const uint32_t ngli_paint_node_types[] = {
    NGL_NODE_COLORPAINT,
    NGL_NODE_TEXTUREPAINT,
    NGL_NODE_GRADIENTPAINT,
    NGL_NODE_GRADIENT4PAINT,
    NGL_NODE_NOISEPAINT,
    NGL_NODE_CUSTOMPAINT,
    NGLI_NODE_NONE,
};

void ngli_paint_info_reset(struct paint_info *info)
{
    ngli_darray_reset(&info->uniforms);
    ngli_darray_reset(&info->custom_uniforms);
    ngli_darray_reset(&info->custom_textures);
    ngli_darray_reset(&info->custom_blocks);
}

#define REGISTER_UNIFORM(info_, name_, type_, opts_struct_, field_) do {    \
    const struct paint_uniform_def _ud = {                                  \
        .type = (type_),                                                    \
        .opts_offset = offsetof(opts_struct_, field_),                      \
    };                                                                      \
    if (ngli_darray_push(&(info_)->uniforms, _ud) < 0)                      \
        return NGL_ERROR_MEMORY;                                            \
    struct paint_uniform_def *_p = ngli_darray_tail(&(info_)->uniforms);    \
    snprintf(_p->name, sizeof(_p->name), "%s", (name_));                    \
} while (0)

struct colorpaint_priv {
    struct paint_info info;
};

struct colorpaint_opts {
    struct paint_base_opts base_opts;
    float color[4];
};

#define COLORPAINT_GLSL(entrypoint_, prefix_) \
    "vec4 " entrypoint_ "(vec2 uv, vec2 tex_coord) { return " prefix_ "_color; }\n"

static const char colorpaint_glsl[]        = COLORPAINT_GLSL("ngli_color",  "ngli_fill");
static const char colorpaint_stroke_glsl[] = COLORPAINT_GLSL("ngli_stroke", "ngli_stroke");

#undef COLORPAINT_GLSL

static int colorpaint_init(struct ngl_node *node)
{
    struct colorpaint_priv *s = node->priv_data;
    const struct colorpaint_opts *o = node->opts;
    struct paint_info *info = &s->info;
    info->glsl[PAINT_SHADER_ROLE_FILL] = colorpaint_glsl;
    info->glsl[PAINT_SHADER_ROLE_STROKE] = colorpaint_stroke_glsl;
    info->opts = o;
    REGISTER_UNIFORM(info, "color", NGPU_TYPE_VEC4, struct colorpaint_opts, color);
    return 0;
}

static void paint_uninit(struct ngl_node *node)
{
    struct paint_info *info = node->priv_data;
    ngli_paint_info_reset(info);
}

NGLI_STATIC_ASSERT(offsetof(struct colorpaint_priv, info) == 0,
                   "paint_info must be first in colorpaint_priv");

#define OFFSET(x) offsetof(struct colorpaint_opts, x)
static const struct node_param colorpaint_params[] = {
    {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(base_opts.opacity),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the paint content"),
    },
    {
        .key       = "premult",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(base_opts.premult),
        .def_value = {.i32 = 1},
        .desc      = NGLI_DOCSTRING("premultiply paint color by its alpha"),
    },
    {
        .key       = "color",
        .type      = NGLI_PARAM_TYPE_VEC4,
        .offset    = OFFSET(color),
        .def_value = {.vec={1.f, 1.f, 1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("paint color (RGBA)"),
    },
    {NULL}
};
#undef OFFSET

const struct node_class ngli_colorpaint_class = {
    .id        = NGL_NODE_COLORPAINT,
    .name      = "ColorPaint",
    .init      = colorpaint_init,
    .uninit    = paint_uninit,
    .opts_size = sizeof(struct colorpaint_opts),
    .priv_size = sizeof(struct colorpaint_priv),
    .params    = colorpaint_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};

struct texturepaint_priv {
    struct paint_info info;
};

struct texturepaint_opts {
    struct paint_base_opts base_opts;
    struct ngl_node *texture;
};

static const struct param_choices texturepaint_wrap_choices = {
    .name = "paint_wrap",
    .consts = {
        {
            .key   = "default",
            .value = PAINT_WRAP_DEFAULT,
            .desc  = NGLI_DOCSTRING("use texture wrap parameters"),
        },
        {
            .key   = "discard",
            .value = PAINT_WRAP_DISCARD,
            .desc  = NGLI_DOCSTRING("discard fragment if coordinates are outside texture boundaries"),
        },
        {NULL}
    }
};

static const struct param_choices texturepaint_scaling_choices = {
    .name = "paint_scaling",
    .consts = {
        {
            .key   = "none",
            .value = PAINT_SCALING_NONE,
            .desc  = NGLI_DOCSTRING("no scaling, texture is stretched to the target shape bounds"),
        },
        {
            .key   = "fit",
            .value = PAINT_SCALING_FIT,
            .desc  = NGLI_DOCSTRING("scale to fit within the target shape bounds preserving aspect ratio (may leave empty areas)"),
        },
        {
            .key   = "fill",
            .value = PAINT_SCALING_FILL,
            .desc  = NGLI_DOCSTRING("scale to fill the target shape bounds preserving aspect ratio (may crop)"),
        },
        {NULL}
    }
};

#define TEXTUREPAINT_GLSL(entrypoint_, prefix_, content_wrap_) \
    "vec4 " entrypoint_ "(vec2 uv, vec2 tex_coord) {\n" \
    "    if (" content_wrap_ " == 1 && (any(lessThan(tex_coord, vec2(0.0))) || any(greaterThan(tex_coord, vec2(1.0)))))\n" \
    "        return vec4(0.0);\n" \
    "    return ngl_texvideo(" prefix_ "_tex, tex_coord);\n" \
    "}\n"

static const char texturepaint_glsl[] =
    TEXTUREPAINT_GLSL("ngli_color", "ngli_fill", "ngli_content_wrap");
static const char texturepaint_stroke_glsl[] =
    TEXTUREPAINT_GLSL("ngli_stroke", "ngli_stroke", "ngli_stroke_content_wrap");

#undef TEXTUREPAINT_GLSL

static int texturepaint_init(struct ngl_node *node)
{
    struct texturepaint_priv *s = node->priv_data;
    const struct texturepaint_opts *o = node->opts;

    if (!o->texture) {
        LOG(ERROR, "TexturePaint: texture param is required");
        return NGL_ERROR_INVALID_USAGE;
    }

    struct paint_info *info = &s->info;
    info->glsl[PAINT_SHADER_ROLE_FILL] = texturepaint_glsl;
    info->glsl[PAINT_SHADER_ROLE_STROKE] = texturepaint_stroke_glsl;
    info->texture = o->texture;
    info->opts = o;
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct texturepaint_priv, info) == 0,
                   "paint_info must be first in texturepaint_priv");

#define OFFSET(x) offsetof(struct texturepaint_opts, x)
static const struct node_param texturepaint_params[] = {
    {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(base_opts.opacity),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the paint content"),
    },
    {
        .key     = "scaling",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(base_opts.scaling),
        .choices = &texturepaint_scaling_choices,
        .desc    = NGLI_DOCSTRING("texture scaling mode relative to the target shape bounds"),
    },
    {
        .key       = "wrap",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(base_opts.wrap),
        .def_value = {.i32 = PAINT_WRAP_DISCARD},
        .choices   = &texturepaint_wrap_choices,
        .desc      = NGLI_DOCSTRING("texture wrap behaviour"),
    },
    {
        .key       = "premult",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(base_opts.premult),
        .def_value = {.i32 = 1},
        .desc      = NGLI_DOCSTRING("premultiply texture color by its alpha"),
    },
    {
        .key        = "texture",
        .type       = NGLI_PARAM_TYPE_NODE,
        .offset     = OFFSET(texture),
        .node_types = (const uint32_t[]){
            NGL_NODE_TEXTURE2D,
            NGL_NODE_CUSTOMTEXTURE,
            NGLI_NODE_NONE,
        },
        .flags = NGLI_PARAM_FLAG_NON_NULL,
        .desc  = NGLI_DOCSTRING("texture to draw"),
    },
    {NULL}
};
#undef OFFSET

const struct node_class ngli_texturepaint_class = {
    .id        = NGL_NODE_TEXTUREPAINT,
    .name      = "TexturePaint",
    .init      = texturepaint_init,
    .uninit    = paint_uninit,
    .update    = ngli_node_update_children,
    .pre_draw  = ngli_node_pre_draw_children,
    .draw      = ngli_node_draw_children,
    .opts_size = sizeof(struct texturepaint_opts),
    .priv_size = sizeof(struct texturepaint_priv),
    .params    = texturepaint_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};

struct gradientpaint_priv {
    struct paint_info info;
};

struct gradientpaint_opts {
    struct paint_base_opts base_opts;
    float color0[3];
    float color1[3];
    float opacity0;
    float opacity1;
    float pos0[2];
    float pos1[2];
    int gradient_mode;
    int gradient_linear;
};

static const struct param_choices gradientpaint_mode_choices = {
    .name = "paint_gradient_mode",
    .consts = {
        {
            .key   = "ramp",
            .value = 0,
            .desc  = NGLI_DOCSTRING("straight line gradient"),
        },
        {
            .key   = "radial",
            .value = 1,
            .desc  = NGLI_DOCSTRING("radial gradient between the two points"),
        },
        {NULL}
    }
};

#define GRADIENTPAINT_GLSL(entrypoint_, prefix_) \
    "vec4 " entrypoint_ "(vec2 uv, vec2 tex_coord) {\n" \
    "    vec3 c0 = " prefix_ "_color0 * " prefix_ "_opacity0;\n" \
    "    vec3 c1 = " prefix_ "_color1 * " prefix_ "_opacity1;\n" \
    "    float aspect = ngli_rect_size.x / ngli_rect_size.y;\n" \
    "    float t = 0.0;\n" \
    "    if (" prefix_ "_gradient_mode == 0) {\n" \
    "        vec2 pa = uv - " prefix_ "_pos0, ba = " prefix_ "_pos1 - " prefix_ "_pos0;\n" \
    "        pa.x *= aspect; ba.x *= aspect;\n" \
    "        t = dot(pa, ba) / dot(ba, ba);\n" \
    "    } else {\n" \
    "        vec2 pa = uv - " prefix_ "_pos0, pb = uv - " prefix_ "_pos1;\n" \
    "        pa.x *= aspect; pb.x *= aspect;\n" \
    "        float lpa = length(pa);\n" \
    "        t = lpa / (lpa + length(pb));\n" \
    "    }\n" \
    "    float a = mix(" prefix_ "_opacity0, " prefix_ "_opacity1, t);\n" \
    "    if (" prefix_ "_gradient_linear != 0) return vec4(ngli_srgbmix(c0, c1, t), a);\n" \
    "    return vec4(mix(c0, c1, t), a);\n" \
    "}\n"

static const char gradientpaint_glsl[]        = GRADIENTPAINT_GLSL("ngli_color",  "ngli_fill");
static const char gradientpaint_stroke_glsl[] = GRADIENTPAINT_GLSL("ngli_stroke", "ngli_stroke");

#undef GRADIENTPAINT_GLSL

static int gradientpaint_init(struct ngl_node *node)
{
    struct gradientpaint_priv *s = node->priv_data;
    const struct gradientpaint_opts *o = node->opts;
    struct paint_info *info = &s->info;
    info->helper_flags = PAINT_HELPER_SRGB;
    info->glsl[PAINT_SHADER_ROLE_FILL] = gradientpaint_glsl;
    info->glsl[PAINT_SHADER_ROLE_STROKE] = gradientpaint_stroke_glsl;
    info->opts = o;
    REGISTER_UNIFORM(info, "color0",          NGPU_TYPE_VEC3, struct gradientpaint_opts, color0);
    REGISTER_UNIFORM(info, "color1",          NGPU_TYPE_VEC3, struct gradientpaint_opts, color1);
    REGISTER_UNIFORM(info, "opacity0",        NGPU_TYPE_F32,  struct gradientpaint_opts, opacity0);
    REGISTER_UNIFORM(info, "opacity1",        NGPU_TYPE_F32,  struct gradientpaint_opts, opacity1);
    REGISTER_UNIFORM(info, "pos0",            NGPU_TYPE_VEC2, struct gradientpaint_opts, pos0);
    REGISTER_UNIFORM(info, "pos1",            NGPU_TYPE_VEC2, struct gradientpaint_opts, pos1);
    REGISTER_UNIFORM(info, "gradient_mode",   NGPU_TYPE_I32,  struct gradientpaint_opts, gradient_mode);
    REGISTER_UNIFORM(info, "gradient_linear", NGPU_TYPE_I32,  struct gradientpaint_opts, gradient_linear);
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct gradientpaint_priv, info) == 0,
                   "paint_info must be first in gradientpaint_priv");

#define OFFSET(x) offsetof(struct gradientpaint_opts, x)
static const struct node_param gradientpaint_params[] = {
    {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(base_opts.opacity),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the paint content"),
    },
    {
        .key       = "premult",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(base_opts.premult),
        .def_value = {.i32 = 1},
        .desc      = NGLI_DOCSTRING("premultiply gradient color by its alpha"),
    },
    {
        .key       = "color0",
        .type      = NGLI_PARAM_TYPE_VEC3,
        .offset    = OFFSET(color0),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("first gradient color (linear RGB)"),
    },
    {
        .key       = "color1",
        .type      = NGLI_PARAM_TYPE_VEC3,
        .offset    = OFFSET(color1),
        .def_value = {.vec={1.f, 1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("second gradient color (linear RGB)"),
    },
    {
        .key       = "opacity0",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(opacity0),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the first gradient color"),
    },
    {
        .key       = "opacity1",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(opacity1),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the second gradient color"),
    },
    {
        .key       = "pos0",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(pos0),
        .def_value = {.vec={0.f, 0.5f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("first gradient point (UV space)"),
    },
    {
        .key       = "pos1",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(pos1),
        .def_value = {.vec={1.f, 0.5f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("second gradient point (UV space)"),
    },
    {
        .key     = "mode",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(gradient_mode),
        .choices = &gradientpaint_mode_choices,
        .desc    = NGLI_DOCSTRING("gradient interpolation mode"),
    },
    {
        .key       = "linear",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(gradient_linear),
        .def_value = {.i32=1},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("interpolate colors in linear light"),
    },
    {NULL}
};
#undef OFFSET

const struct node_class ngli_gradientpaint_class = {
    .id        = NGL_NODE_GRADIENTPAINT,
    .name      = "GradientPaint",
    .init      = gradientpaint_init,
    .uninit    = paint_uninit,
    .opts_size = sizeof(struct gradientpaint_opts),
    .priv_size = sizeof(struct gradientpaint_priv),
    .params    = gradientpaint_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};

struct gradient4paint_priv {
    struct paint_info info;
};

struct gradient4paint_opts {
    struct paint_base_opts base_opts;
    float color_tl[3];
    float color_tr[3];
    float color_br[3];
    float color_bl[3];
    float opacity_tl;
    float opacity_tr;
    float opacity_br;
    float opacity_bl;
    int gradient_linear;
};

#define GRADIENT4PAINT_GLSL(entrypoint_, prefix_) \
    "float " prefix_ "_g4(float tl, float tr, float br, float bl, vec2 uv) {\n" \
    "    return mix(mix(tl, tr, uv.x), mix(bl, br, uv.x), uv.y);\n" \
    "}\n" \
    "vec3 " prefix_ "_g4(vec3 tl, vec3 tr, vec3 br, vec3 bl, vec2 uv) {\n" \
    "    return mix(mix(tl, tr, uv.x), mix(bl, br, uv.x), uv.y);\n" \
    "}\n" \
    "vec4 " entrypoint_ "(vec2 uv, vec2 tex_coord) {\n" \
    "    vec3 tl = " prefix_ "_color_tl * " prefix_ "_opacity_tl;\n" \
    "    vec3 tr = " prefix_ "_color_tr * " prefix_ "_opacity_tr;\n" \
    "    vec3 br = " prefix_ "_color_br * " prefix_ "_opacity_br;\n" \
    "    vec3 bl = " prefix_ "_color_bl * " prefix_ "_opacity_bl;\n" \
    "    float a = " prefix_ "_g4(" prefix_ "_opacity_tl, " prefix_ "_opacity_tr, " prefix_ "_opacity_br, " prefix_ "_opacity_bl, uv);\n" \
    "    if (" prefix_ "_gradient_linear != 0)\n" \
    "        return vec4(ngli_linear2srgb(" prefix_ "_g4(ngli_srgb2linear(tl),\n" \
    "                                                    ngli_srgb2linear(tr),\n" \
    "                                                    ngli_srgb2linear(br),\n" \
    "                                                    ngli_srgb2linear(bl), uv)), a);\n" \
    "    return vec4(" prefix_ "_g4(tl, tr, br, bl, uv), a);\n" \
    "}\n"

static const char gradient4paint_glsl[]        = GRADIENT4PAINT_GLSL("ngli_color",  "ngli_fill");
static const char gradient4paint_stroke_glsl[] = GRADIENT4PAINT_GLSL("ngli_stroke", "ngli_stroke");

#undef GRADIENT4PAINT_GLSL

static int gradient4paint_init(struct ngl_node *node)
{
    struct gradient4paint_priv *s = node->priv_data;
    const struct gradient4paint_opts *o = node->opts;
    struct paint_info *info = &s->info;
    info->helper_flags = PAINT_HELPER_SRGB;
    info->glsl[PAINT_SHADER_ROLE_FILL] = gradient4paint_glsl;
    info->glsl[PAINT_SHADER_ROLE_STROKE] = gradient4paint_stroke_glsl;
    info->opts = o;
    REGISTER_UNIFORM(info, "color_tl",        NGPU_TYPE_VEC3, struct gradient4paint_opts, color_tl);
    REGISTER_UNIFORM(info, "color_tr",        NGPU_TYPE_VEC3, struct gradient4paint_opts, color_tr);
    REGISTER_UNIFORM(info, "color_br",        NGPU_TYPE_VEC3, struct gradient4paint_opts, color_br);
    REGISTER_UNIFORM(info, "color_bl",        NGPU_TYPE_VEC3, struct gradient4paint_opts, color_bl);
    REGISTER_UNIFORM(info, "opacity_tl",      NGPU_TYPE_F32,  struct gradient4paint_opts, opacity_tl);
    REGISTER_UNIFORM(info, "opacity_tr",      NGPU_TYPE_F32,  struct gradient4paint_opts, opacity_tr);
    REGISTER_UNIFORM(info, "opacity_br",      NGPU_TYPE_F32,  struct gradient4paint_opts, opacity_br);
    REGISTER_UNIFORM(info, "opacity_bl",      NGPU_TYPE_F32,  struct gradient4paint_opts, opacity_bl);
    REGISTER_UNIFORM(info, "gradient_linear", NGPU_TYPE_I32,  struct gradient4paint_opts, gradient_linear);
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct gradient4paint_priv, info) == 0,
                   "paint_info must be first in gradient4paint_priv");

#define OFFSET(x) offsetof(struct gradient4paint_opts, x)
static const struct node_param gradient4paint_params[] = {
    {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(base_opts.opacity),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the paint content"),
    },
    {
        .key       = "premult",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(base_opts.premult),
        .def_value = {.i32 = 1},
        .desc      = NGLI_DOCSTRING("premultiply gradient color by its alpha"),
    },
    {
        .key       = "color_tl",
        .type      = NGLI_PARAM_TYPE_VEC3,
        .offset    = OFFSET(color_tl),
        .def_value = {.vec={1.f, 0.5f, 0.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("top-left color"),
    },
    {
        .key       = "color_tr",
        .type      = NGLI_PARAM_TYPE_VEC3,
        .offset    = OFFSET(color_tr),
        .def_value = {.vec={0.f, 1.f, 0.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("top-right color"),
    },
    {
        .key       = "color_br",
        .type      = NGLI_PARAM_TYPE_VEC3,
        .offset    = OFFSET(color_br),
        .def_value = {.vec={0.f, 0.5f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("bottom-right color"),
    },
    {
        .key       = "color_bl",
        .type      = NGLI_PARAM_TYPE_VEC3,
        .offset    = OFFSET(color_bl),
        .def_value = {.vec={1.f, 0.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("bottom-left color"),
    },
    {
        .key       = "opacity_tl",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(opacity_tl),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the top-left color"),
    },
    {
        .key       = "opacity_tr",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(opacity_tr),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the top-right color"),
    },
    {
        .key       = "opacity_br",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(opacity_br),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the bottom-right color"),
    },
    {
        .key       = "opacity_bl",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(opacity_bl),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the bottom-left color"),
    },
    {
        .key       = "linear",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(gradient_linear),
        .def_value = {.i32=1},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("interpolate colors in linear light"),
    },
    {NULL}
};
#undef OFFSET

const struct node_class ngli_gradient4paint_class = {
    .id        = NGL_NODE_GRADIENT4PAINT,
    .name      = "Gradient4Paint",
    .init      = gradient4paint_init,
    .uninit    = paint_uninit,
    .opts_size = sizeof(struct gradient4paint_opts),
    .priv_size = sizeof(struct gradient4paint_priv),
    .params    = gradient4paint_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};

struct noisepaint_priv {
    struct paint_info info;
};

struct noisepaint_opts {
    struct paint_base_opts base_opts;
    int noise_type;
    float amplitude;
    uint32_t octaves;
    float lacunarity;
    float gain;
    uint32_t seed;
    float scale[2];
    float evolution;
};

static const struct param_choices noisepaint_type_choices = {
    .name = "paint_noise_type",
    .consts = {
        {
            .key   = "blocky",
            .value = 0,
            .desc  = NGLI_DOCSTRING("blocky noise"),
        },
        {
            .key   = "perlin",
            .value = 1,
            .desc  = NGLI_DOCSTRING("perlin noise"),
        },
        {NULL}
    }
};

#define NOISEPAINT_GLSL(entrypoint_, prefix_) \
    "vec4 " entrypoint_ "(vec2 uv, vec2 tex_coord) {\n" \
    "    vec2 st = uv * " prefix_ "_noise_scale;\n" \
    "    float n = fbm(vec3(st, " prefix_ "_noise_evolution), " prefix_ "_noise_type, " prefix_ "_noise_amplitude,\n" \
    "                  " prefix_ "_noise_octaves, " prefix_ "_noise_lacunarity, " prefix_ "_noise_gain, " prefix_ "_noise_seed);\n" \
    "    n = (n + 1.0) / 2.0;\n" \
    "    return vec4(vec3(n), 1.0);\n" \
    "}\n"

static const char noisepaint_glsl[]        = NOISEPAINT_GLSL("ngli_color",  "ngli_fill");
static const char noisepaint_stroke_glsl[] = NOISEPAINT_GLSL("ngli_stroke", "ngli_stroke");

#undef NOISEPAINT_GLSL

static int noisepaint_init(struct ngl_node *node)
{
    struct noisepaint_priv *s = node->priv_data;
    const struct noisepaint_opts *o = node->opts;
    struct paint_info *info = &s->info;
    info->helper_flags = PAINT_HELPER_MISC_UTILS | PAINT_HELPER_NOISE;
    info->glsl[PAINT_SHADER_ROLE_FILL] = noisepaint_glsl;
    info->glsl[PAINT_SHADER_ROLE_STROKE] = noisepaint_stroke_glsl;
    info->opts = o;
    REGISTER_UNIFORM(info, "noise_type",       NGPU_TYPE_I32,  struct noisepaint_opts, noise_type);
    REGISTER_UNIFORM(info, "noise_amplitude",  NGPU_TYPE_F32,  struct noisepaint_opts, amplitude);
    REGISTER_UNIFORM(info, "noise_octaves",    NGPU_TYPE_U32,  struct noisepaint_opts, octaves);
    REGISTER_UNIFORM(info, "noise_lacunarity", NGPU_TYPE_F32,  struct noisepaint_opts, lacunarity);
    REGISTER_UNIFORM(info, "noise_gain",       NGPU_TYPE_F32,  struct noisepaint_opts, gain);
    REGISTER_UNIFORM(info, "noise_seed",       NGPU_TYPE_U32,  struct noisepaint_opts, seed);
    REGISTER_UNIFORM(info, "noise_scale",      NGPU_TYPE_VEC2, struct noisepaint_opts, scale);
    REGISTER_UNIFORM(info, "noise_evolution",  NGPU_TYPE_F32,  struct noisepaint_opts, evolution);
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct noisepaint_priv, info) == 0,
                   "paint_info must be first in noisepaint_priv");

#define OFFSET(x) offsetof(struct noisepaint_opts, x)
static const struct node_param noisepaint_params[] = {
    {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(base_opts.opacity),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the paint content"),
    },
    {
        .key       = "premult",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(base_opts.premult),
        .def_value = {.i32 = 1},
        .desc      = NGLI_DOCSTRING("premultiply noise color by its alpha"),
    },
    {
        .key     = "type",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(noise_type),
        .choices = &noisepaint_type_choices,
        .desc    = NGLI_DOCSTRING("noise algorithm"),
    },
    {
        .key       = "amplitude",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(amplitude),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("noise amplitude"),
    },
    {
        .key       = "octaves",
        .type      = NGLI_PARAM_TYPE_U32,
        .offset    = OFFSET(octaves),
        .def_value = {.u32=8},
        .desc      = NGLI_DOCSTRING("number of noise octaves"),
    },
    {
        .key       = "lacunarity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(lacunarity),
        .def_value = {.f32=2.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("frequency multiplier between octaves"),
    },
    {
        .key       = "gain",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(gain),
        .def_value = {.f32=0.5f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("amplitude multiplier between octaves"),
    },
    {
        .key    = "seed",
        .type   = NGLI_PARAM_TYPE_U32,
        .offset = OFFSET(seed),
        .desc   = NGLI_DOCSTRING("noise seed"),
    },
    {
        .key       = "scale",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(scale),
        .def_value = {.vec={1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("UV scale applied before noise sampling"),
    },
    {
        .key   = "evolution",
        .type  = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(evolution),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("temporal evolution coordinate for the noise"),
    },
    {NULL}
};
#undef OFFSET

const struct node_class ngli_noisepaint_class = {
    .id        = NGL_NODE_NOISEPAINT,
    .name      = "NoisePaint",
    .init      = noisepaint_init,
    .uninit    = paint_uninit,
    .opts_size = sizeof(struct noisepaint_opts),
    .priv_size = sizeof(struct noisepaint_priv),
    .params    = noisepaint_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};

struct custompaint_priv {
    struct paint_info info;
    char *glsl[PAINT_SHADER_ROLE_NB];
};

struct custompaint_opts {
    struct paint_base_opts base_opts;
    char *glsl_header;
    char *glsl_color;
    struct hmap *resources;
    int color_output_count;
};


static int register_uniform(struct paint_info *info, const char *name, struct ngl_node *res)
{
    const struct variable_info *var = res->priv_data;
    struct paint_custom_uniform_def cu = {
        .type = var->data_type,
        .node = res,
    };
    snprintf(cu.name, sizeof(cu.name), "%s", name);
    return ngli_darray_push(&info->custom_uniforms, cu);
}

static int register_texture(struct paint_info *info, const char *name, struct ngl_node *res)
{
    struct paint_custom_texture_def ct = {
        .texture_node = res,
    };
    snprintf(ct.name, sizeof(ct.name), "%s", name);
    return ngli_darray_push(&info->custom_textures, ct);
}

static int register_block(struct paint_info *info, const char *name, struct ngl_node *res)
{
    struct paint_custom_block_def cb = {
        .node = res,
    };
    snprintf(cb.name, sizeof(cb.name), "%s", name);
    return ngli_darray_push(&info->custom_blocks, cb);
}

static int register_resource(struct paint_info *info, const char *name, struct ngl_node *res)
{
    switch (res->cls->category) {
    case NGLI_NODE_CATEGORY_VARIABLE: return register_uniform(info, name, res);
    case NGLI_NODE_CATEGORY_TEXTURE:  return register_texture(info, name, res);
    case NGLI_NODE_CATEGORY_BLOCK:    return register_block(info, name, res);
    default:
        ngli_assert(0);
    }
}

static char *custompaint_build_glsl(const struct custompaint_opts *o, const char *entrypoint)
{
    struct bstr *bstr = ngli_bstr_create();
    if (!bstr)
        return NULL;

    ngli_bstr_printf(bstr, "%s %s(vec2 uv, vec2 tex_coord) {\n",
                     o->color_output_count ? "void" : "vec4", entrypoint);
    ngli_bstr_print(bstr, o->glsl_color);
    ngli_bstr_print(bstr, "\n}\n");

    char *glsl = ngli_bstr_check(bstr) < 0 ? NULL : ngli_bstr_strdup(bstr);
    ngli_bstr_freep(&bstr);
    return glsl;
}

static int custompaint_init(struct ngl_node *node)
{
    struct custompaint_priv *s = node->priv_data;
    const struct custompaint_opts *o = node->opts;

    if (!o->glsl_color || !o->glsl_color[0]) {
        LOG(ERROR, "CustomPaint: glsl_color param is required");
        return NGL_ERROR_INVALID_USAGE;
    }

    struct paint_info *info = &s->info;
    info->glsl_header = o->glsl_header && o->glsl_header[0] ? o->glsl_header : NULL;
    info->opts = o;
    info->color_output_count = (size_t)o->color_output_count;

    s->glsl[PAINT_SHADER_ROLE_FILL] = custompaint_build_glsl(
        o, o->color_output_count ? "ngli_colors" : "ngli_color");
    s->glsl[PAINT_SHADER_ROLE_STROKE] = custompaint_build_glsl(o, "ngli_stroke");
    if (!s->glsl[PAINT_SHADER_ROLE_FILL] || !s->glsl[PAINT_SHADER_ROLE_STROKE]) {
        ngli_freep(&s->glsl[PAINT_SHADER_ROLE_FILL]);
        ngli_freep(&s->glsl[PAINT_SHADER_ROLE_STROKE]);
        return NGL_ERROR_MEMORY;
    }
    info->glsl[PAINT_SHADER_ROLE_FILL] = s->glsl[PAINT_SHADER_ROLE_FILL];
    info->glsl[PAINT_SHADER_ROLE_STROKE] = s->glsl[PAINT_SHADER_ROLE_STROKE];

    if (o->resources) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(o->resources, entry))) {
            int ret = register_resource(info, entry->key.str, entry->data);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static void custompaint_uninit(struct ngl_node *node)
{
    struct custompaint_priv *s = node->priv_data;
    ngli_paint_info_reset(&s->info);
    ngli_freep(&s->glsl[PAINT_SHADER_ROLE_FILL]);
    ngli_freep(&s->glsl[PAINT_SHADER_ROLE_STROKE]);
}

NGLI_STATIC_ASSERT(offsetof(struct custompaint_priv, info) == 0,
                   "paint_info must be first in custompaint_priv");

#define OFFSET(x) offsetof(struct custompaint_opts, x)
static const struct node_param custompaint_params[] = {
    {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(base_opts.opacity),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("opacity of the paint content"),
    },
    {
        .key     = "scaling",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(base_opts.scaling),
        .choices = &texturepaint_scaling_choices,
        .desc    = NGLI_DOCSTRING("texture scaling mode applied to custom paint content"),
    },
    {
        .key     = "wrap",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(base_opts.wrap),
        .choices = &texturepaint_wrap_choices,
        .desc    = NGLI_DOCSTRING("wrap mode for out-of-bounds coordinates"),
    },
    {
        .key       = "premult",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(base_opts.premult),
        .def_value = {.i32 = 1},
        .desc      = NGLI_DOCSTRING("premultiply glsl_color() output color by its alpha"),
    },
    {
        .key    = "glsl_header",
        .type   = NGLI_PARAM_TYPE_STR,
        .offset = OFFSET(glsl_header),
        .desc   = NGLI_DOCSTRING("GLSL code prepended before the color function (helper functions, etc.)"),
    },
    {
        .key    = "glsl_color",
        .type   = NGLI_PARAM_TYPE_STR,
        .offset = OFFSET(glsl_color),
        .desc   = NGLI_DOCSTRING("GLSL body of the color function"),
    },
    {
        .key        = "resources",
        .type       = NGLI_PARAM_TYPE_NODEDICT,
        .offset     = OFFSET(resources),
        .node_types = (const uint32_t[]){
            NGL_NODE_UNIFORMFLOAT,
            NGL_NODE_UNIFORMVEC2,
            NGL_NODE_UNIFORMVEC3,
            NGL_NODE_UNIFORMVEC4,
            NGL_NODE_UNIFORMCOLOR,
            NGL_NODE_UNIFORMQUAT,
            NGL_NODE_UNIFORMMAT4,
            NGL_NODE_UNIFORMINT,
            NGL_NODE_UNIFORMIVEC2,
            NGL_NODE_UNIFORMIVEC3,
            NGL_NODE_UNIFORMIVEC4,
            NGL_NODE_UNIFORMUINT,
            NGL_NODE_UNIFORMUIVEC2,
            NGL_NODE_UNIFORMUIVEC3,
            NGL_NODE_UNIFORMUIVEC4,
            NGL_NODE_UNIFORMBOOL,
            NGL_NODE_EVALFLOAT,
            NGL_NODE_EVALVEC2,
            NGL_NODE_EVALVEC3,
            NGL_NODE_EVALVEC4,
            NGL_NODE_ANIMATEDFLOAT,
            NGL_NODE_ANIMATEDVEC2,
            NGL_NODE_ANIMATEDVEC3,
            NGL_NODE_ANIMATEDVEC4,
            NGL_NODE_ANIMATEDQUAT,
            NGL_NODE_ANIMATEDCOLOR,
            NGL_NODE_NOISEFLOAT,
            NGL_NODE_NOISEVEC2,
            NGL_NODE_NOISEVEC3,
            NGL_NODE_NOISEVEC4,
            NGL_NODE_TIME,
            NGL_NODE_TEXTURE2D,
            NGL_NODE_TEXTURE2DARRAY,
            NGL_NODE_TEXTURE3D,
            NGL_NODE_TEXTURECUBE,
            NGL_NODE_CUSTOMTEXTURE,
            NGL_NODE_BLOCK,
            NGLI_NODE_NONE,
        },
        .desc = NGLI_DOCSTRING("uniform and texture nodes available to glsl_color; each node's label "
                               "is used as the GLSL name"),
    },
    {
        .key    = "color_output_count",
        .type   = NGLI_PARAM_TYPE_I32,
        .offset = OFFSET(color_output_count),
        .desc   = NGLI_DOCSTRING("number of fragment outputs for MRT rendering "
                                 "(0 means single output using ngli_color(), "
                                 ">0 uses ngli_colors() writing to ngl_out_color[])"),
    },
    {NULL}
};
#undef OFFSET

const struct node_class ngli_custompaint_class = {
    .id        = NGL_NODE_CUSTOMPAINT,
    .name      = "CustomPaint",
    .init      = custompaint_init,
    .update    = ngli_node_update_children,
    .pre_draw  = ngli_node_pre_draw_children,
    .draw      = ngli_node_draw_children,
    .uninit    = custompaint_uninit,
    .opts_size = sizeof(struct custompaint_opts),
    .priv_size = sizeof(struct custompaint_priv),
    .params    = custompaint_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};
