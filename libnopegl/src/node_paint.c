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
#include <stdio.h>
#include <string.h>

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
    NGL_NODE_LINEARGRADIENTPAINT,
    NGL_NODE_RADIALGRADIENTPAINT,
    NGL_NODE_SWEEPGRADIENTPAINT,
    NGL_NODE_NOISEPAINT,
    NGL_NODE_CUSTOMPAINT,
    NGLI_NODE_NONE,
};

static const char *const paint_resource_prefixes[PAINT_SHADER_ROLE_NB] = {
    [PAINT_SHADER_ROLE_FILL]   = "ngli_fill_",
    [PAINT_SHADER_ROLE_STROKE] = "ngli_stroke_",
};

void ngli_paint_get_resource_name(char *dst, size_t size,
                                  enum paint_shader_role role, const char *name)
{
    snprintf(dst, size, "%s%s", paint_resource_prefixes[role], name);
}

static int is_glsl_ident(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

void ngli_paint_glsl_write(struct bstr *b, const char *glsl,
                           enum paint_shader_role role, const char *entrypoint)
{
    const char *prefix = paint_resource_prefixes[role];

    const char *segment = glsl;
    const char *p = glsl;
    while (*p) {
        if (*p == '$') {
            ngli_bstr_write(b, segment, (size_t)(p - segment));
            ngli_bstr_print(b, prefix);
            segment = ++p;
            while (is_glsl_ident(*p))
                p++;
            continue;
        }

        if (!is_glsl_ident(*p)) {
            p++;
            continue;
        }

        const char *ident = p;
        while (is_glsl_ident(*p))
            p++;
        if (p - ident != 4 || memcmp(ident, "main", 4))
            continue;

        const char *q = p;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            q++;
        if (*q == '(') {
            ngli_bstr_write(b, segment, (size_t)(ident - segment));
            ngli_bstr_print(b, entrypoint);
            segment = p;
        }
    }
    ngli_bstr_print(b, segment);
}

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
        .data = (const uint8_t *)(info_)->opts + offsetof(opts_struct_, field_), \
    };                                                                      \
    if (ngli_darray_try_push(&(info_)->uniforms, _ud) < 0)                  \
        return NGL_ERROR_MEMORY;                                            \
    struct paint_uniform_def *_p = ngli_darray_tail(&(info_)->uniforms);    \
    snprintf(_p->name, sizeof(_p->name), "%s", (name_));                    \
} while (0)

#define REGISTER_UNIFORM_DATA(info_, name_, type_, count_, data_) do {      \
    const struct paint_uniform_def _ud = {                                  \
        .type = (type_),                                                    \
        .count = (count_),                                                  \
        .data = (const uint8_t *)(data_),                                   \
    };                                                                      \
    if (ngli_darray_try_push(&(info_)->uniforms, _ud) < 0)                  \
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

static const char colorpaint_glsl[] =
    "vec4 main(vec2 uv, vec2 tex_coord) { return $color; }\n";

static int colorpaint_init(struct ngl_node *node)
{
    struct colorpaint_priv *s = node->priv_data;
    const struct colorpaint_opts *o = node->opts;
    struct paint_info *info = &s->info;
    info->glsl = colorpaint_glsl;
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

static const char texturepaint_glsl[] =
    "vec4 main(vec2 uv, vec2 tex_coord) {\n"
    "    if ($content_wrap == 1 && (any(lessThan(tex_coord, vec2(0.0))) || any(greaterThan(tex_coord, vec2(1.0)))))\n"
    "        return vec4(0.0);\n"
    "    return ngl_texvideo($tex, tex_coord);\n"
    "}\n";

static int texturepaint_init(struct ngl_node *node)
{
    struct texturepaint_priv *s = node->priv_data;
    const struct texturepaint_opts *o = node->opts;

    if (!o->texture) {
        LOG(ERROR, "TexturePaint: texture param is required");
        return NGL_ERROR_INVALID_USAGE;
    }

    struct paint_info *info = &s->info;
    info->glsl = texturepaint_glsl;
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
        .flags   = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
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

static const char gradientpaint_glsl[] =
    "vec4 main(vec2 uv, vec2 tex_coord) {\n"
    "    vec3 c0 = $color0 * $opacity0;\n"
    "    vec3 c1 = $color1 * $opacity1;\n"
    "    float aspect = ngli_rect_size.x / ngli_rect_size.y;\n"
    "    float t = 0.0;\n"
    "    if ($gradient_mode == 0) {\n"
    "        vec2 pa = uv - $pos0, ba = $pos1 - $pos0;\n"
    "        pa.x *= aspect; ba.x *= aspect;\n"
    "        t = dot(pa, ba) / dot(ba, ba);\n"
    "    } else {\n"
    "        vec2 pa = uv - $pos0, pb = uv - $pos1;\n"
    "        pa.x *= aspect; pb.x *= aspect;\n"
    "        float lpa = length(pa);\n"
    "        t = lpa / (lpa + length(pb));\n"
    "    }\n"
    "    float a = mix($opacity0, $opacity1, t);\n"
    "    if ($gradient_linear != 0) return vec4(ngli_srgbmix(c0, c1, t), a);\n"
    "    return vec4(mix(c0, c1, t), a);\n"
    "}\n";

static int gradientpaint_init(struct ngl_node *node)
{
    struct gradientpaint_priv *s = node->priv_data;
    const struct gradientpaint_opts *o = node->opts;
    struct paint_info *info = &s->info;
    info->helper_flags = PAINT_HELPER_SRGB;
    info->glsl = gradientpaint_glsl;
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

static const char gradient4paint_glsl[] =
    "float $g4(float tl, float tr, float br, float bl, vec2 uv) {\n"
    "    return mix(mix(tl, tr, uv.x), mix(bl, br, uv.x), uv.y);\n"
    "}\n"
    "vec3 $g4(vec3 tl, vec3 tr, vec3 br, vec3 bl, vec2 uv) {\n"
    "    return mix(mix(tl, tr, uv.x), mix(bl, br, uv.x), uv.y);\n"
    "}\n"
    "vec4 main(vec2 uv, vec2 tex_coord) {\n"
    "    vec3 tl = $color_tl * $opacity_tl;\n"
    "    vec3 tr = $color_tr * $opacity_tr;\n"
    "    vec3 br = $color_br * $opacity_br;\n"
    "    vec3 bl = $color_bl * $opacity_bl;\n"
    "    float a = $g4($opacity_tl, $opacity_tr, $opacity_br, $opacity_bl, uv);\n"
    "    if ($gradient_linear != 0) {\n"
    "        vec3 lin = $g4(\n"
    "            ngli_srgb2linear(tl),\n"
    "            ngli_srgb2linear(tr),\n"
    "            ngli_srgb2linear(br),\n"
    "            ngli_srgb2linear(bl), uv);\n"
    "        return vec4(ngli_linear2srgb(lin), a);\n"
    "    }\n"
    "    return vec4($g4(tl, tr, br, bl, uv), a);\n"
    "}\n";

static int gradient4paint_init(struct ngl_node *node)
{
    struct gradient4paint_priv *s = node->priv_data;
    const struct gradient4paint_opts *o = node->opts;
    struct paint_info *info = &s->info;
    info->helper_flags = PAINT_HELPER_SRGB;
    info->glsl = gradient4paint_glsl;
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

static const char noisepaint_glsl[] =
    "vec4 main(vec2 uv, vec2 tex_coord) {\n"
    "    vec2 st = uv * $noise_scale;\n"
    "    float n = fbm(vec3(st, $noise_evolution), $noise_type, $noise_amplitude,\n"
    "                  $noise_octaves, $noise_lacunarity, $noise_gain, $noise_seed);\n"
    "    n = (n + 1.0) / 2.0;\n"
    "    return vec4(vec3(n), 1.0);\n"
    "}\n";

static int noisepaint_init(struct ngl_node *node)
{
    struct noisepaint_priv *s = node->priv_data;
    const struct noisepaint_opts *o = node->opts;
    struct paint_info *info = &s->info;
    info->helper_flags = PAINT_HELPER_MISC_UTILS | PAINT_HELPER_NOISE;
    info->glsl = noisepaint_glsl;
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
    char *glsl;
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
    return ngli_darray_try_push(&info->custom_uniforms, cu);
}

static int register_texture(struct paint_info *info, const char *name, struct ngl_node *res)
{
    struct paint_custom_texture_def ct = {
        .texture_node = res,
    };
    snprintf(ct.name, sizeof(ct.name), "%s", name);
    return ngli_darray_try_push(&info->custom_textures, ct);
}

static int register_block(struct paint_info *info, const char *name, struct ngl_node *res)
{
    struct paint_custom_block_def cb = {
        .node = res,
    };
    snprintf(cb.name, sizeof(cb.name), "%s", name);
    return ngli_darray_try_push(&info->custom_blocks, cb);
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

static char *custompaint_build_glsl(const struct custompaint_opts *o)
{
    struct bstr *bstr = ngli_bstr_create();
    if (!bstr)
        return NULL;

    ngli_bstr_printf(bstr, "%s main(vec2 uv, vec2 tex_coord) {\n",
                     o->color_output_count ? "void" : "vec4");
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

    s->glsl = custompaint_build_glsl(o);
    if (!s->glsl)
        return NGL_ERROR_MEMORY;
    info->glsl = s->glsl;

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
    ngli_freep(&s->glsl);
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
        .flags   = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
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
        .desc   = NGLI_DOCSTRING("GLSL code prepended before the color function (helper functions, etc.); "
                                 "symbols declared here must be written $name so that they are namespaced "
                                 "per shader role"),
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
        .desc = NGLI_DOCSTRING("uniform, texture and block nodes available to glsl_color; each node is "
                               "referenced in the GLSL as $name, where name is the key it is bound to"),
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

struct gradientstop_opts {
    struct ngl_node *position_node;
    float position;
    struct ngl_node *color_node;
    float color[4];
};

struct gradientstops_opts {
    struct ngl_node **stops;
    size_t nb_stops;
};

struct gradientstops_priv {
    struct ngli_gradient_stops_info info;
    int dynamic;
    int dirty;
};

static const uint32_t gradientstop_types[] = {
    NGL_NODE_GRADIENTSTOP,
    NGLI_NODE_NONE,
};

static int update_gradient_stops(struct ngl_node *node)
{
    const struct gradientstops_opts *o = node->opts;
    struct gradientstops_priv *s = node->priv_data;

    if (o->nb_stops < 2 || o->nb_stops > NGLI_MAX_GRADIENT_STOPS) {
        LOG(ERROR, "GradientStops.stops must contain between 2 and %d stops", NGLI_MAX_GRADIENT_STOPS);
        return NGL_ERROR_INVALID_USAGE;
    }

    float previous_position = -INFINITY;
    for (size_t i = 0; i < o->nb_stops; i++) {
        const struct gradientstop_opts *stop = o->stops[i]->opts;
        const float position = *(const float *)ngli_node_get_data_ptr(stop->position_node, &stop->position);
        const float *color = ngli_node_get_data_ptr(stop->color_node, stop->color);
        if (!isfinite(position) || position < 0.f || position > 1.f) {
            LOG(ERROR, "GradientStops.stops[%zu].position must be finite and within [0,1]", i);
            return NGL_ERROR_INVALID_ARG;
        }
        if (position < previous_position) {
            LOG(ERROR, "GradientStops.stops must be ordered by non-decreasing position");
            return NGL_ERROR_INVALID_ARG;
        }
        for (size_t j = 0; j < NGLI_ARRAY_NB(stop->color); j++) {
            if (!isfinite(color[j])) {
                LOG(ERROR, "GradientStops.stops[%zu].color contains a non-finite component", i);
                return NGL_ERROR_INVALID_ARG;
            }
        }
        memcpy(s->info.colors[i], color, sizeof(s->info.colors[i]));
        s->info.positions[i] = position;
        previous_position = position;
    }
    s->info.nb_stops = (int32_t)o->nb_stops;
    s->info.revision++;
    return 0;
}

static int gradientstops_init(struct ngl_node *node)
{
    const struct gradientstops_opts *o = node->opts;
    struct gradientstops_priv *s = node->priv_data;

    int ret = update_gradient_stops(node);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < o->nb_stops; i++) {
        const struct gradientstop_opts *stop = o->stops[i]->opts;
        const struct ngl_node *nodes[] = {stop->position_node, stop->color_node};
        for (size_t j = 0; j < NGLI_ARRAY_NB(nodes); j++) {
            if (!nodes[j])
                continue;
            ngli_assert(nodes[j]->cls->category == NGLI_NODE_CATEGORY_VARIABLE);
            const struct variable_info *var = nodes[j]->priv_data;
            s->dynamic |= var->dynamic;
        }
    }

    return 0;
}

static int gradientstops_invalidate(struct ngl_node *node)
{
    struct gradientstops_priv *s = node->priv_data;
    s->dirty = 1;
    return 0;
}

static int gradientstops_update(struct ngl_node *node, double t)
{
    struct gradientstops_priv *s = node->priv_data;

    if (!s->dynamic && !s->dirty)
        return 0;

    int ret = ngli_node_update_children(node, t);
    if (ret < 0)
        return ret;

    ret = update_gradient_stops(node);
    if (ret < 0)
        return ret;
    s->dirty = 0;
    return 0;
}

const struct ngli_gradient_stops_info *ngli_gradient_stops_get_info(const struct ngl_node *node)
{
    ngli_assert(node->cls->id == NGL_NODE_GRADIENTSTOPS);
    const struct gradientstops_priv *s = node->priv_data;
    return &s->info;
}

#define OFFSET(type, x) offsetof(type, x)
static const struct node_param gradientstop_params[] = {
    {
        .key = "position",
        .type = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(struct gradientstop_opts, position_node),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc = NGLI_DOCSTRING("normalized stop position within [0,1]"),
    }, {
        .key = "color",
        .type = NGLI_PARAM_TYPE_VEC4,
        .offset = OFFSET(struct gradientstop_opts, color_node),
        .def_value = {.vec = {1.f, 1.f, 1.f, 1.f}},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc = NGLI_DOCSTRING("unpremultiplied RGBA stop color"),
    },
    {NULL},
};

static const struct node_param gradientstops_params[] = {
    {
        .key = "stops",
        .type = NGLI_PARAM_TYPE_NODELIST,
        .offset = OFFSET(struct gradientstops_opts, stops),
        .node_types = gradientstop_types,
        .flags = NGLI_PARAM_FLAG_NON_NULL | NGLI_PARAM_FLAG_DOT_DISPLAY_PACKED,
        .desc = NGLI_DOCSTRING("ordered color stops shared by one or more gradient paints"),
    },
    {NULL},
};
#undef OFFSET

const struct node_class ngli_gradientstop_class = {
    .id        = NGL_NODE_GRADIENTSTOP,
    .name      = "GradientStop",
    .update    = ngli_node_update_children,
    .opts_size = sizeof(struct gradientstop_opts),
    .params    = gradientstop_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};

const struct node_class ngli_gradientstops_class = {
    .id         = NGL_NODE_GRADIENTSTOPS,
    .name       = "GradientStops",
    .init       = gradientstops_init,
    .invalidate = gradientstops_invalidate,
    .update     = gradientstops_update,
    .opts_size  = sizeof(struct gradientstops_opts),
    .priv_size  = sizeof(struct gradientstops_priv),
    .params     = gradientstops_params,
    .flags      = NGLI_NODE_FLAG_SHAREABLE,
    .file       = __FILE__,
};

enum gradient_spread {
    GRADIENT_SPREAD_CLAMP,
    GRADIENT_SPREAD_REPEAT,
    GRADIENT_SPREAD_MIRROR,
};

struct gradientpaint_base_opts {
    struct paint_base_opts base_opts;
    struct ngl_node *stops;
    struct ngl_node *transform_node;
    float transform[16];
    int spread;
    int linear;
};

struct multigradientpaint_priv {
    struct paint_info fi;
    float transform[16];
};

struct lineargradientpaint_opts {
    struct gradientpaint_base_opts base;
    float start[2];
    float end[2];
};

struct radialgradientpaint_opts {
    struct gradientpaint_base_opts base;
    float center[2];
    float radius;
};

struct sweepgradientpaint_opts {
    struct gradientpaint_base_opts base;
    float center[2];
    float start_angle;
    float sweep_angle;
};

static const struct param_choices gradient_spread_choices = {
    .name = "gradient_spread",
    .consts = {
        {"clamp",  GRADIENT_SPREAD_CLAMP,  .desc = NGLI_DOCSTRING("extend the first and last colors beyond the stop range")},
        {"repeat", GRADIENT_SPREAD_REPEAT, .desc = NGLI_DOCSTRING("repeat the stop range")},
        {"mirror", GRADIENT_SPREAD_MIRROR, .desc = NGLI_DOCSTRING("repeat the stop range while reversing alternate repetitions")},
        {NULL},
    },
};

#define MULTIGRADIENTPAINT_GLSL \
    "vec2 $gradient_coord(vec2 uv)\n" \
    "{\n" \
    "    vec4 coord = $gradient_transform * vec4(uv, 0.0, 1.0);\n" \
    "    return coord.w != 0.0 ? coord.xy / coord.w : coord.xy;\n" \
    "}\n" \
    "float $gradient_apply_spread(float t)\n" \
    "{\n" \
    "    if ($gradient_spread == 0) return clamp(t, 0.0, 1.0);\n" \
    "    if ($gradient_spread == 1) return fract(t);\n" \
    "    float x = mod(t, 2.0);\n" \
    "    return 1.0 - abs(x - 1.0);\n" \
    "}\n" \
    "vec4 $gradient_sample(float t)\n" \
    "{\n" \
    "    t = $gradient_apply_spread(t);\n" \
    "    if (t <= $gradient_stop_positions[0]) return $gradient_stop_colors[0];\n" \
    "    for (int i = 1; i < " NGLI_STRINGIFY(NGLI_MAX_GRADIENT_STOPS) "; i++) {\n" \
    "        if (i >= $gradient_stop_count) break;\n" \
    "        if (t <= $gradient_stop_positions[i]) {\n" \
    "            float span = $gradient_stop_positions[i] - $gradient_stop_positions[i - 1];\n" \
    "            float f = span > 0.0 ? (t - $gradient_stop_positions[i - 1]) / span : 1.0;\n" \
    "            vec4 c0 = $gradient_stop_colors[i - 1];\n" \
    "            vec4 c1 = $gradient_stop_colors[i];\n" \
    "            if ($gradient_linear != 0)\n" \
    "                return vec4(ngli_srgbmix(c0.rgb, c1.rgb, f), mix(c0.a, c1.a, f));\n" \
    "            return mix(c0, c1, f);\n" \
    "        }\n" \
    "    }\n" \
    "    return $gradient_stop_colors[$gradient_stop_count - 1];\n" \
    "}\n"

#define LINEARGRADIENTPAINT_GLSL \
    MULTIGRADIENTPAINT_GLSL \
    "vec4 main(vec2 uv, vec2 tex_coord)\n" \
    "{\n" \
    "    vec2 p = $gradient_coord(uv);\n" \
    "    vec2 d = $gradient_end - $gradient_start;\n" \
    "    float aspect = ngli_rect_size.x / ngli_rect_size.y;\n" \
    "    p.x *= aspect; d.x *= aspect;\n" \
    "    vec2 start = $gradient_start; start.x *= aspect;\n" \
    "    float denom = dot(d, d);\n" \
    "    return $gradient_sample(denom > 0.0 ? dot(p - start, d) / denom : 0.0);\n" \
    "}\n"

#define RADIALGRADIENTPAINT_GLSL \
    MULTIGRADIENTPAINT_GLSL \
    "vec4 main(vec2 uv, vec2 tex_coord)\n" \
    "{\n" \
    "    vec2 p = $gradient_coord(uv) - $gradient_center;\n" \
    "    p.x *= ngli_rect_size.x / ngli_rect_size.y;\n" \
    "    return $gradient_sample(length(p) / max($gradient_radius, 1e-6));\n" \
    "}\n"

#define SWEEPGRADIENTPAINT_GLSL \
    MULTIGRADIENTPAINT_GLSL \
    "vec4 main(vec2 uv, vec2 tex_coord)\n" \
    "{\n" \
    "    vec2 p = $gradient_coord(uv) - $gradient_center;\n" \
    "    p.x *= ngli_rect_size.x / ngli_rect_size.y;\n" \
    "    float angle = degrees(atan(p.y, p.x));\n" \
    "    return $gradient_sample((angle - $gradient_start_angle) / $gradient_sweep_angle);\n" \
    "}\n"

static const char lineargradientpaint_glsl[] = LINEARGRADIENTPAINT_GLSL;
static const char radialgradientpaint_glsl[] = RADIALGRADIENTPAINT_GLSL;
static const char sweepgradientpaint_glsl[] = SWEEPGRADIENTPAINT_GLSL;

#undef SWEEPGRADIENTPAINT_GLSL
#undef RADIALGRADIENTPAINT_GLSL
#undef LINEARGRADIENTPAINT_GLSL
#undef MULTIGRADIENTPAINT_GLSL

static int register_multigradient_uniforms(struct paint_info *fi,
                                            const struct gradientpaint_base_opts *o,
                                            const float *transform)
{
    const struct ngli_gradient_stops_info *stops = ngli_gradient_stops_get_info(o->stops);
    REGISTER_UNIFORM_DATA(fi, "gradient_stop_colors",    NGPU_TYPE_VEC4, NGLI_MAX_GRADIENT_STOPS, stops->colors);
    REGISTER_UNIFORM_DATA(fi, "gradient_stop_positions", NGPU_TYPE_F32,  NGLI_MAX_GRADIENT_STOPS, stops->positions);
    REGISTER_UNIFORM_DATA(fi, "gradient_stop_count",     NGPU_TYPE_I32,  0,                       &stops->nb_stops);
    REGISTER_UNIFORM_DATA(fi, "gradient_transform",      NGPU_TYPE_MAT4, 0,                       transform);
    REGISTER_UNIFORM(fi,      "gradient_spread",         NGPU_TYPE_I32,  struct gradientpaint_base_opts, spread);
    REGISTER_UNIFORM(fi,      "gradient_linear",         NGPU_TYPE_I32,  struct gradientpaint_base_opts, linear);
    return 0;
}

static int update_multigradient_transform(struct ngl_node *node)
{
    const struct gradientpaint_base_opts *o = node->opts;
    struct multigradientpaint_priv *s = node->priv_data;
    const float *transform = ngli_node_get_data_ptr(o->transform_node, o->transform);
    for (size_t i = 0; i < NGLI_ARRAY_NB(s->transform); i++) {
        if (!isfinite(transform[i])) {
            LOG(ERROR, "gradient transform contains a non-finite component");
            return NGL_ERROR_INVALID_ARG;
        }
    }
    memcpy(s->transform, transform, sizeof(s->transform));
    return 0;
}

static int init_multigradientpaint(struct ngl_node *node, const char *glsl)
{
    struct multigradientpaint_priv *s = node->priv_data;
    const struct gradientpaint_base_opts *o = node->opts;
    int ret = update_multigradient_transform(node);
    if (ret < 0)
        return ret;
    struct paint_info *fi = &s->fi;
    fi->helper_flags = PAINT_HELPER_SRGB;
    fi->glsl = glsl;
    fi->opts = o;
    return register_multigradient_uniforms(fi, o, s->transform);
}

static void multigradientpaint_uninit(struct ngl_node *node)
{
    struct multigradientpaint_priv *s = node->priv_data;
    ngli_paint_info_reset(&s->fi);
}

static int update_multigradientpaint(struct ngl_node *node, double t)
{
    int ret = ngli_node_update_children(node, t);
    if (ret < 0)
        return ret;
    return update_multigradient_transform(node);
}

static int validate_radialgradientpaint(struct ngl_node *node)
{
    const struct radialgradientpaint_opts *o = node->opts;
    if (!isfinite(o->radius) || o->radius <= 0.f) {
        LOG(ERROR, "RadialGradientPaint.radius must be finite and greater than zero");
        return NGL_ERROR_INVALID_ARG;
    }
    return 0;
}

static int validate_sweepgradientpaint(struct ngl_node *node)
{
    const struct sweepgradientpaint_opts *o = node->opts;
    if (!isfinite(o->sweep_angle) || o->sweep_angle == 0.f) {
        LOG(ERROR, "SweepGradientPaint.sweep_angle must be finite and non-zero");
        return NGL_ERROR_INVALID_ARG;
    }
    return 0;
}

static int lineargradientpaint_init(struct ngl_node *node)
{
    int ret = init_multigradientpaint(node, lineargradientpaint_glsl);
    if (ret < 0)
        return ret;
    struct multigradientpaint_priv *s = node->priv_data;
    struct paint_info *fi = &s->fi;
    REGISTER_UNIFORM(fi, "gradient_start", NGPU_TYPE_VEC2, struct lineargradientpaint_opts, start);
    REGISTER_UNIFORM(fi, "gradient_end",   NGPU_TYPE_VEC2, struct lineargradientpaint_opts, end);
    return 0;
}

static int radialgradientpaint_init(struct ngl_node *node)
{
    int ret = validate_radialgradientpaint(node);
    if (ret < 0)
        return ret;
    ret = init_multigradientpaint(node, radialgradientpaint_glsl);
    if (ret < 0)
        return ret;
    struct multigradientpaint_priv *s = node->priv_data;
    struct paint_info *fi = &s->fi;
    REGISTER_UNIFORM(fi, "gradient_center", NGPU_TYPE_VEC2, struct radialgradientpaint_opts, center);
    REGISTER_UNIFORM(fi, "gradient_radius", NGPU_TYPE_F32,  struct radialgradientpaint_opts, radius);
    return 0;
}

static int sweepgradientpaint_init(struct ngl_node *node)
{
    int ret = validate_sweepgradientpaint(node);
    if (ret < 0)
        return ret;
    ret = init_multigradientpaint(node, sweepgradientpaint_glsl);
    if (ret < 0)
        return ret;
    struct multigradientpaint_priv *s = node->priv_data;
    struct paint_info *fi = &s->fi;
    REGISTER_UNIFORM(fi, "gradient_center",      NGPU_TYPE_VEC2, struct sweepgradientpaint_opts, center);
    REGISTER_UNIFORM(fi, "gradient_start_angle", NGPU_TYPE_F32,  struct sweepgradientpaint_opts, start_angle);
    REGISTER_UNIFORM(fi, "gradient_sweep_angle", NGPU_TYPE_F32,  struct sweepgradientpaint_opts, sweep_angle);
    return 0;
}

static int radialgradientpaint_update(struct ngl_node *node, double t)
{
    int ret = update_multigradientpaint(node, t);
    if (ret < 0)
        return ret;
    return validate_radialgradientpaint(node);
}

static int sweepgradientpaint_update(struct ngl_node *node, double t)
{
    int ret = update_multigradientpaint(node, t);
    if (ret < 0)
        return ret;
    return validate_sweepgradientpaint(node);
}

#define GRADIENT_PAINT_BASE_PARAMS(opts_type) \
    { \
        .key = "opacity", \
        .type = NGLI_PARAM_TYPE_F32, \
        .offset = offsetof(opts_type, base.base_opts.opacity), \
        .def_value = {.f32 = 1.f}, \
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE, \
        .desc = NGLI_DOCSTRING("opacity of the paint content"), \
    }, { \
        .key = "premult", \
        .type = NGLI_PARAM_TYPE_BOOL, \
        .offset = offsetof(opts_type, base.base_opts.premult), \
        .def_value = {.i32 = 1}, \
        .desc = NGLI_DOCSTRING("premultiply the gradient color by its alpha"), \
    }, { \
        .key = "stops", \
        .type = NGLI_PARAM_TYPE_NODE, \
        .offset = offsetof(opts_type, base.stops), \
        .node_types = (const uint32_t[]){NGL_NODE_GRADIENTSTOPS, NGLI_NODE_NONE}, \
        .flags = NGLI_PARAM_FLAG_NON_NULL, \
        .desc = NGLI_DOCSTRING("ordered color stops sampled by the gradient"), \
    }, { \
        .key = "transform", \
        .type = NGLI_PARAM_TYPE_MAT4, \
        .offset = offsetof(opts_type, base.transform_node), \
        .def_value = {.mat = NGLI_MAT4_IDENTITY}, \
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE, \
        .update_func = update_multigradient_transform, \
        .desc = NGLI_DOCSTRING("matrix mapping target-local UV coordinates to paint-local UV coordinates"), \
    }, { \
        .key = "spread", \
        .type = NGLI_PARAM_TYPE_SELECT, \
        .offset = offsetof(opts_type, base.spread), \
        .choices = &gradient_spread_choices, \
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE, \
        .desc = NGLI_DOCSTRING("behavior outside the normalized stop range"), \
    }, { \
        .key = "linear", \
        .type = NGLI_PARAM_TYPE_BOOL, \
        .offset = offsetof(opts_type, base.linear), \
        .def_value = {.i32 = 1}, \
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE, \
        .desc = NGLI_DOCSTRING("interpolate stop colors in linear light"), \
    }

static const struct node_param lineargradientpaint_params[] = {
    GRADIENT_PAINT_BASE_PARAMS(struct lineargradientpaint_opts),
    {
        .key = "start",
        .type = NGLI_PARAM_TYPE_VEC2,
        .offset = offsetof(struct lineargradientpaint_opts, start),
        .def_value = {.vec = {0.f, .5f}},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("first point of the gradient axis in local UV coordinates"),
    }, {
        .key = "end",
        .type = NGLI_PARAM_TYPE_VEC2,
        .offset = offsetof(struct lineargradientpaint_opts, end),
        .def_value = {.vec = {1.f, .5f}},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("second point of the gradient axis in local UV coordinates"),
    },
    {NULL},
};

static const struct node_param radialgradientpaint_params[] = {
    GRADIENT_PAINT_BASE_PARAMS(struct radialgradientpaint_opts),
    {
        .key = "center",
        .type = NGLI_PARAM_TYPE_VEC2,
        .offset = offsetof(struct radialgradientpaint_opts, center),
        .def_value = {.vec = {.5f, .5f}},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("center of the radial gradient in local UV coordinates"),
    }, {
        .key = "radius",
        .type = NGLI_PARAM_TYPE_F32,
        .offset = offsetof(struct radialgradientpaint_opts, radius),
        .def_value = {.f32 = .5f},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .update_func = validate_radialgradientpaint,
        .desc = NGLI_DOCSTRING("radial gradient radius, relative to target height"),
    },
    {NULL},
};

static const struct node_param sweepgradientpaint_params[] = {
    GRADIENT_PAINT_BASE_PARAMS(struct sweepgradientpaint_opts),
    {
        .key = "center",
        .type = NGLI_PARAM_TYPE_VEC2,
        .offset = offsetof(struct sweepgradientpaint_opts, center),
        .def_value = {.vec = {.5f, .5f}},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("center of the sweep gradient in local UV coordinates"),
    }, {
        .key = "start_angle",
        .type = NGLI_PARAM_TYPE_F32,
        .offset = offsetof(struct sweepgradientpaint_opts, start_angle),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc = NGLI_DOCSTRING("angle in degrees of the first stop, measured clockwise from the positive x axis"),
    }, {
        .key = "sweep_angle",
        .type = NGLI_PARAM_TYPE_F32,
        .offset = offsetof(struct sweepgradientpaint_opts, sweep_angle),
        .def_value = {.f32 = 360.f},
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .update_func = validate_sweepgradientpaint,
        .desc = NGLI_DOCSTRING("signed angular span in degrees; positive follows the canvas clockwise direction"),
    },
    {NULL},
};
#undef GRADIENT_PAINT_BASE_PARAMS

const struct node_class ngli_lineargradientpaint_class = {
    .id        = NGL_NODE_LINEARGRADIENTPAINT,
    .name      = "LinearGradientPaint",
    .init      = lineargradientpaint_init,
    .update    = update_multigradientpaint,
    .uninit    = multigradientpaint_uninit,
    .opts_size = sizeof(struct lineargradientpaint_opts),
    .priv_size = sizeof(struct multigradientpaint_priv),
    .params    = lineargradientpaint_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};

const struct node_class ngli_radialgradientpaint_class = {
    .id        = NGL_NODE_RADIALGRADIENTPAINT,
    .name      = "RadialGradientPaint",
    .init      = radialgradientpaint_init,
    .update    = radialgradientpaint_update,
    .uninit    = multigradientpaint_uninit,
    .opts_size = sizeof(struct radialgradientpaint_opts),
    .priv_size = sizeof(struct multigradientpaint_priv),
    .params    = radialgradientpaint_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};

const struct node_class ngli_sweepgradientpaint_class = {
    .id        = NGL_NODE_SWEEPGRADIENTPAINT,
    .name      = "SweepGradientPaint",
    .init      = sweepgradientpaint_init,
    .update    = sweepgradientpaint_update,
    .uninit    = multigradientpaint_uninit,
    .opts_size = sizeof(struct sweepgradientpaint_opts),
    .priv_size = sizeof(struct multigradientpaint_priv),
    .params    = sweepgradientpaint_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};
