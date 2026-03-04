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

#include "internal.h"
#include "node_stroke.h"
#include "nopegl/nopegl.h"

#define REG_STROKE_UNIFORM(si_, name_, type_, opts_struct_, field_) do {        \
    ngli_assert((si_)->nb_uniforms < STROKE_MAX_UNIFORMS);                      \
    struct stroke_uniform_def *_ud = &(si_)->uniforms[(si_)->nb_uniforms++];    \
    snprintf(_ud->name, sizeof(_ud->name), "%s", (name_));                      \
    _ud->type = (type_);                                                        \
    _ud->opts_offset = offsetof(opts_struct_, field_);                          \
} while (0)

const struct param_choices ngli_stroke_mode_choices = {
    .name = "stroke_mode",
    .consts = {
        {"inside",  STROKE_INSIDE,  .desc=NGLI_DOCSTRING("outline inside the shape boundary")},
        {"center",  STROKE_CENTER,  .desc=NGLI_DOCSTRING("outline centered on the shape boundary")},
        {"outside", STROKE_OUTSIDE, .desc=NGLI_DOCSTRING("outline outside the shape boundary")},
        {NULL}
    }
};

const struct param_choices ngli_stroke_dash_cap_choices = {
    .name = "stroke_dash_cap",
    .consts = {
        {"butt",   STROKE_DASH_CAP_BUTT,   .desc=NGLI_DOCSTRING("flat cap at the dash boundary")},
        {"round",  STROKE_DASH_CAP_ROUND,  .desc=NGLI_DOCSTRING("semicircular cap extending past the dash boundary")},
        {"square", STROKE_DASH_CAP_SQUARE, .desc=NGLI_DOCSTRING("square cap extending past the dash boundary by half the stroke width")},
        {NULL}
    }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Stroke  (solid single color)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct stroke_priv { struct stroke_info si; };

static const char stroke_glsl[] =
    "vec4 ngl_stroke_color(vec2 uv) { return stroke_color; }\n";

static int stroke_init(struct ngl_node *node)
{
    struct stroke_priv *s = node->priv_data;
    const struct stroke_opts *o = node->opts;
    struct stroke_info *si = &s->si;
    si->glsl = stroke_glsl;
    si->opts = o;
    REG_STROKE_UNIFORM(si, "stroke_color", NGPU_TYPE_VEC4, struct stroke_opts, color);
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct stroke_priv, si) == 0,
                   "stroke_info must be first in stroke_priv");

#define OFFSET(x) offsetof(struct stroke_opts, x)
static const struct node_param stroke_params[] = {
    {
        .key   = "width",
        .type  = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(width),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("outline width in pixels"),
    },
    {
        .key     = "mode",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(mode),
        .choices = &ngli_stroke_mode_choices,
        .flags   = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc    = NGLI_DOCSTRING("outline position relative to the shape boundary"),
    },
    {
        .key   = "dash_length",
        .type  = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(dash_length),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("dash period in pixels (0 means solid)"),
    },
    {
        .key       = "dash_ratio",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(dash_ratio),
        .def_value = {.f32=0.5f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("fraction of dash_length that is filled (0..1)"),
    },
    {
        .key   = "dash_offset",
        .type  = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(dash_offset),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("phase offset along the perimeter in pixels"),
    },
    {
        .key     = "dash_cap",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(dash_cap),
        .choices = &ngli_stroke_dash_cap_choices,
        .flags   = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc    = NGLI_DOCSTRING("dash end cap style"),
    },
    {
        .key       = "color",
        .type      = NGLI_PARAM_TYPE_VEC4,
        .offset    = OFFSET(color),
        .def_value = {.vec={1.f, 1.f, 1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("outline color and opacity (RGBA)"),
    },
    {NULL}
};
#undef OFFSET

const struct node_class ngli_stroke_class = {
    .id        = NGL_NODE_STROKE,
    .name      = "Stroke",
    .init      = stroke_init,
    .opts_size = sizeof(struct stroke_opts),
    .priv_size = sizeof(struct stroke_priv),
    .params    = stroke_params,
    .file      = __FILE__,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * StrokeGradient  (two-point gradient stroke color)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct strokegradient_priv { struct stroke_info si; };

struct strokegradient_opts {
    /* stroke_base_opts fields must come first */
    float width;
    int mode;
    float dash_length;
    float dash_ratio;
    float dash_offset;
    int dash_cap;
    /* gradient-specific fields */
    float color0[3];
    float color1[3];
    float opacity0;
    float opacity1;
    float pos0[2];
    float pos1[2];
    int gradient_mode;
    int gradient_linear;
};

static const struct param_choices strokegradient_mode_choices = {
    .name = "stroke_gradient_mode",
    .consts = {
        {"ramp",   0, .desc=NGLI_DOCSTRING("straight line gradient")},
        {"radial", 1, .desc=NGLI_DOCSTRING("radial gradient between the two points")},
        {NULL}
    }
};

static const char strokegradient_glsl[] =
    "vec4 ngl_stroke_color(vec2 uv) {\n"
    "    vec3 c0 = sg_color0 * sg_opacity0;\n"
    "    vec3 c1 = sg_color1 * sg_opacity1;\n"
    "    float aspect = rect_size.x / rect_size.y;\n"
    "    float t = 0.0;\n"
    "    if (sg_gradient_mode == 0) {\n"
    "        vec2 pa = uv - sg_pos0, ba = sg_pos1 - sg_pos0;\n"
    "        pa.x *= aspect; ba.x *= aspect;\n"
    "        t = dot(pa, ba) / dot(ba, ba);\n"
    "    } else {\n"
    "        vec2 pa = uv - sg_pos0, pb = uv - sg_pos1;\n"
    "        pa.x *= aspect; pb.x *= aspect;\n"
    "        float lpa = length(pa);\n"
    "        t = lpa / (lpa + length(pb));\n"
    "    }\n"
    "    float a = mix(sg_opacity0, sg_opacity1, t);\n"
    "    if (sg_gradient_linear != 0) return vec4(ngli_srgbmix(c0, c1, t), a);\n"
    "    return vec4(mix(c0, c1, t), a);\n"
    "}\n";

static int strokegradient_init(struct ngl_node *node)
{
    struct strokegradient_priv *s = node->priv_data;
    const struct strokegradient_opts *o = node->opts;
    struct stroke_info *si = &s->si;
    si->helper_flags = STROKE_HELPER_SRGB;
    si->glsl = strokegradient_glsl;
    si->opts = o;
    REG_STROKE_UNIFORM(si, "sg_color0",          NGPU_TYPE_VEC3, struct strokegradient_opts, color0);
    REG_STROKE_UNIFORM(si, "sg_color1",          NGPU_TYPE_VEC3, struct strokegradient_opts, color1);
    REG_STROKE_UNIFORM(si, "sg_opacity0",        NGPU_TYPE_F32,  struct strokegradient_opts, opacity0);
    REG_STROKE_UNIFORM(si, "sg_opacity1",        NGPU_TYPE_F32,  struct strokegradient_opts, opacity1);
    REG_STROKE_UNIFORM(si, "sg_pos0",            NGPU_TYPE_VEC2, struct strokegradient_opts, pos0);
    REG_STROKE_UNIFORM(si, "sg_pos1",            NGPU_TYPE_VEC2, struct strokegradient_opts, pos1);
    REG_STROKE_UNIFORM(si, "sg_gradient_mode",   NGPU_TYPE_I32,  struct strokegradient_opts, gradient_mode);
    REG_STROKE_UNIFORM(si, "sg_gradient_linear", NGPU_TYPE_I32,  struct strokegradient_opts, gradient_linear);
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct strokegradient_priv, si) == 0,
                   "stroke_info must be first in strokegradient_priv");

#define OFFSET(x) offsetof(struct strokegradient_opts, x)
static const struct node_param strokegradient_params[] = {
    {
        .key   = "width",
        .type  = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(width),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("outline width in pixels"),
    },
    {
        .key     = "mode",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(mode),
        .choices = &ngli_stroke_mode_choices,
        .flags   = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc    = NGLI_DOCSTRING("outline position relative to the shape boundary"),
    },
    {
        .key   = "dash_length",
        .type  = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(dash_length),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("dash period in pixels (0 means solid)"),
    },
    {
        .key       = "dash_ratio",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(dash_ratio),
        .def_value = {.f32=0.5f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("fraction of dash_length that is filled (0..1)"),
    },
    {
        .key   = "dash_offset",
        .type  = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(dash_offset),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("phase offset along the perimeter in pixels"),
    },
    {
        .key     = "dash_cap",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(dash_cap),
        .choices = &ngli_stroke_dash_cap_choices,
        .flags   = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc    = NGLI_DOCSTRING("dash end cap style"),
    },
    {
        .key   = "color0",
        .type  = NGLI_PARAM_TYPE_VEC3,
        .offset = OFFSET(color0),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("first gradient color (linear RGB)"),
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
        .key     = "gradient_mode",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(gradient_mode),
        .choices = &strokegradient_mode_choices,
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

const struct node_class ngli_strokegradient_class = {
    .id        = NGL_NODE_STROKEGRADIENT,
    .name      = "StrokeGradient",
    .init      = strokegradient_init,
    .opts_size = sizeof(struct strokegradient_opts),
    .priv_size = sizeof(struct strokegradient_priv),
    .params    = strokegradient_params,
    .file      = __FILE__,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * StrokeGradient4  (four-corner bilinear gradient stroke color)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct strokegradient4_priv { struct stroke_info si; };

struct strokegradient4_opts {
    /* stroke_base_opts fields must come first */
    float width;
    int mode;
    float dash_length;
    float dash_ratio;
    float dash_offset;
    int dash_cap;
    /* gradient4-specific fields */
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

static const char strokegradient4_glsl[] =
    "#define _sg4(tl,tr,br,bl,u) mix(mix(tl,tr,u.x),mix(bl,br,u.x),u.y)\n"
    "vec4 ngl_stroke_color(vec2 uv) {\n"
    "    vec3 tl = sg4_color_tl * sg4_opacity_tl;\n"
    "    vec3 tr = sg4_color_tr * sg4_opacity_tr;\n"
    "    vec3 br = sg4_color_br * sg4_opacity_br;\n"
    "    vec3 bl = sg4_color_bl * sg4_opacity_bl;\n"
    "    float a = _sg4(sg4_opacity_tl, sg4_opacity_tr, sg4_opacity_br, sg4_opacity_bl, uv);\n"
    "    if (sg4_gradient_linear != 0)\n"
    "        return vec4(ngli_linear2srgb(_sg4(ngli_srgb2linear(tl),\n"
    "                                         ngli_srgb2linear(tr),\n"
    "                                         ngli_srgb2linear(br),\n"
    "                                         ngli_srgb2linear(bl), uv)), a);\n"
    "    return vec4(_sg4(tl, tr, br, bl, uv), a);\n"
    "}\n";

static int strokegradient4_init(struct ngl_node *node)
{
    struct strokegradient4_priv *s = node->priv_data;
    const struct strokegradient4_opts *o = node->opts;
    struct stroke_info *si = &s->si;
    si->helper_flags = STROKE_HELPER_SRGB;
    si->glsl = strokegradient4_glsl;
    si->opts = o;
    REG_STROKE_UNIFORM(si, "sg4_color_tl",        NGPU_TYPE_VEC3, struct strokegradient4_opts, color_tl);
    REG_STROKE_UNIFORM(si, "sg4_color_tr",        NGPU_TYPE_VEC3, struct strokegradient4_opts, color_tr);
    REG_STROKE_UNIFORM(si, "sg4_color_br",        NGPU_TYPE_VEC3, struct strokegradient4_opts, color_br);
    REG_STROKE_UNIFORM(si, "sg4_color_bl",        NGPU_TYPE_VEC3, struct strokegradient4_opts, color_bl);
    REG_STROKE_UNIFORM(si, "sg4_opacity_tl",      NGPU_TYPE_F32,  struct strokegradient4_opts, opacity_tl);
    REG_STROKE_UNIFORM(si, "sg4_opacity_tr",      NGPU_TYPE_F32,  struct strokegradient4_opts, opacity_tr);
    REG_STROKE_UNIFORM(si, "sg4_opacity_br",      NGPU_TYPE_F32,  struct strokegradient4_opts, opacity_br);
    REG_STROKE_UNIFORM(si, "sg4_opacity_bl",      NGPU_TYPE_F32,  struct strokegradient4_opts, opacity_bl);
    REG_STROKE_UNIFORM(si, "sg4_gradient_linear", NGPU_TYPE_I32,  struct strokegradient4_opts, gradient_linear);
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct strokegradient4_priv, si) == 0,
                   "stroke_info must be first in strokegradient4_priv");

#define OFFSET(x) offsetof(struct strokegradient4_opts, x)
static const struct node_param strokegradient4_params[] = {
    {
        .key   = "width",
        .type  = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(width),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("outline width in pixels"),
    },
    {
        .key     = "mode",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(mode),
        .choices = &ngli_stroke_mode_choices,
        .flags   = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc    = NGLI_DOCSTRING("outline position relative to the shape boundary"),
    },
    {
        .key   = "dash_length",
        .type  = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(dash_length),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("dash period in pixels (0 means solid)"),
    },
    {
        .key       = "dash_ratio",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(dash_ratio),
        .def_value = {.f32=0.5f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("fraction of dash_length that is filled (0..1)"),
    },
    {
        .key   = "dash_offset",
        .type  = NGLI_PARAM_TYPE_F32,
        .offset = OFFSET(dash_offset),
        .flags = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc  = NGLI_DOCSTRING("phase offset along the perimeter in pixels"),
    },
    {
        .key     = "dash_cap",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(dash_cap),
        .choices = &ngli_stroke_dash_cap_choices,
        .flags   = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc    = NGLI_DOCSTRING("dash end cap style"),
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

const struct node_class ngli_strokegradient4_class = {
    .id        = NGL_NODE_STROKEGRADIENT4,
    .name      = "StrokeGradient4",
    .init      = strokegradient4_init,
    .opts_size = sizeof(struct strokegradient4_opts),
    .priv_size = sizeof(struct strokegradient4_priv),
    .params    = strokegradient4_params,
    .file      = __FILE__,
};
