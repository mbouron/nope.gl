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
#include <string.h>

#include "internal.h"
#include "log.h"
#include "node_fill.h"
#include "node_texture.h"
#include "transforms.h"
#include "utils/bstr.h"
#include "utils/memory.h"
#include "nopegl/nopegl.h"

#include <ngpu/ngpu.h>

/* ── shared helpers ─────────────────────────────────────────────────────── */

static int node_to_ngpu_type(const struct ngl_node *node, enum ngpu_type *type)
{
    switch (node->cls->id) {
    case NGL_NODE_UNIFORMFLOAT:
    case NGL_NODE_EVALFLOAT:
    case NGL_NODE_ANIMATEDFLOAT:
    case NGL_NODE_NOISEFLOAT:     *type = NGPU_TYPE_F32;   return 0;
    case NGL_NODE_UNIFORMVEC2:
    case NGL_NODE_EVALVEC2:
    case NGL_NODE_ANIMATEDVEC2:
    case NGL_NODE_NOISEVEC2:      *type = NGPU_TYPE_VEC2;  return 0;
    case NGL_NODE_UNIFORMVEC3:
    case NGL_NODE_EVALVEC3:
    case NGL_NODE_ANIMATEDVEC3:
    case NGL_NODE_NOISEVEC3:      *type = NGPU_TYPE_VEC3;  return 0;
    case NGL_NODE_UNIFORMVEC4:
    case NGL_NODE_UNIFORMCOLOR:
    case NGL_NODE_UNIFORMQUAT:
    case NGL_NODE_EVALVEC4:
    case NGL_NODE_ANIMATEDVEC4:
    case NGL_NODE_ANIMATEDQUAT:
    case NGL_NODE_ANIMATEDCOLOR:
    case NGL_NODE_NOISEVEC4:      *type = NGPU_TYPE_VEC4;  return 0;
    case NGL_NODE_UNIFORMMAT4:    *type = NGPU_TYPE_MAT4;  return 0;
    case NGL_NODE_UNIFORMINT:     *type = NGPU_TYPE_I32;   return 0;
    case NGL_NODE_UNIFORMIVEC2:   *type = NGPU_TYPE_IVEC2; return 0;
    case NGL_NODE_UNIFORMIVEC3:   *type = NGPU_TYPE_IVEC3; return 0;
    case NGL_NODE_UNIFORMIVEC4:   *type = NGPU_TYPE_IVEC4; return 0;
    case NGL_NODE_UNIFORMUINT:    *type = NGPU_TYPE_U32;   return 0;
    case NGL_NODE_UNIFORMUIVEC2:  *type = NGPU_TYPE_UVEC2; return 0;
    case NGL_NODE_UNIFORMUIVEC3:  *type = NGPU_TYPE_UVEC3; return 0;
    case NGL_NODE_UNIFORMUIVEC4:  *type = NGPU_TYPE_UVEC4; return 0;
    case NGL_NODE_UNIFORMBOOL:    *type = NGPU_TYPE_BOOL;  return 0;
    case NGL_NODE_TIME:           *type = NGPU_TYPE_F32;   return 0;
    default:
        LOG(ERROR, "unsupported resource node type \"%s\"", node->cls->name);
        return NGL_ERROR_INVALID_ARG;
    }
}


#define REG_UNIFORM(fi_, name_, type_, opts_struct_, field_) do {       \
    ngli_assert((fi_)->nb_uniforms < FILL_MAX_UNIFORMS);                \
    struct fill_uniform_def *_ud = &(fi_)->uniforms[(fi_)->nb_uniforms++]; \
    snprintf(_ud->name, sizeof(_ud->name), "%s", (name_));              \
    _ud->type = (type_);                                                \
    _ud->opts_offset = offsetof(opts_struct_, field_);                  \
} while (0)

/* ═══════════════════════════════════════════════════════════════════════════
 * ColorFill
 * ═══════════════════════════════════════════════════════════════════════════ */

struct colorfill_priv { struct fill_info fi; };

struct colorfill_opts {
    float color[4];
};

static const char colorfill_glsl[] =
    "vec4 ngl_color(vec2 uv, vec2 tex_coord) { return color; }\n";

static int colorfill_init(struct ngl_node *node)
{
    struct colorfill_priv *s = node->priv_data;
    const struct colorfill_opts *o = node->opts;
    struct fill_info *fi = &s->fi;
    fi->glsl = colorfill_glsl;
    fi->opts = o;
    REG_UNIFORM(fi, "color", NGPU_TYPE_VEC4, struct colorfill_opts, color);
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct colorfill_priv, fi) == 0,
                   "fill_info must be first in colorfill_priv");

#define OFFSET(x) offsetof(struct colorfill_opts, x)
static const struct node_param colorfill_params[] = {
    {
        .key       = "color",
        .type      = NGLI_PARAM_TYPE_VEC4,
        .offset    = OFFSET(color),
        .def_value = {.vec={1.f, 1.f, 1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("fill color (RGBA)"),
    },
    {NULL}
};
#undef OFFSET

const struct node_class ngli_colorfill_class = {
    .id        = NGL_NODE_COLORFILL,
    .name      = "ColorFill",
    .init      = colorfill_init,
    .opts_size = sizeof(struct colorfill_opts),
    .priv_size = sizeof(struct colorfill_priv),
    .params    = colorfill_params,
    .file      = __FILE__,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * TextureFill
 * ═══════════════════════════════════════════════════════════════════════════ */

struct texturefill_priv { struct fill_info fi; };

struct texturefill_opts {
    struct ngl_node *texture_node;
    int wrap;
    int scaling;
};

static const struct param_choices texturefill_wrap_choices = {
    .name = "fill_wrap",
    .consts = {
        {"default", FILL_WRAP_DEFAULT, .desc=NGLI_DOCSTRING("use texture wrap parameters")},
        {"discard", FILL_WRAP_DISCARD, .desc=NGLI_DOCSTRING("discard fragment if coordinates are outside texture boundaries")},
        {NULL}
    }
};

static const struct param_choices texturefill_scaling_choices = {
    .name = "fill_scaling",
    .consts = {
        {"none", FILL_SCALING_NONE, .desc=NGLI_DOCSTRING("no scaling, texture is stretched to fill the rect")},
        {"fit",  FILL_SCALING_FIT,  .desc=NGLI_DOCSTRING("scale to fit within the rect preserving aspect ratio (may leave empty areas)")},
        {"fill", FILL_SCALING_FILL, .desc=NGLI_DOCSTRING("scale to fill the rect preserving aspect ratio (may crop)")},
        {NULL}
    }
};

static const char texturefill_glsl[] =
    "vec4 ngl_color(vec2 uv, vec2 tex_coord) { return ngl_texvideo(tex, tex_coord); }\n";

static int texturefill_init(struct ngl_node *node)
{
    struct texturefill_priv *s = node->priv_data;
    const struct texturefill_opts *o = node->opts;

    const struct ngl_node *leaf = o->texture_node
        ? ngli_transform_get_leaf_node(o->texture_node) : NULL;
    if (!leaf) {
        LOG(ERROR, "TextureFill: texture param is required");
        return NGL_ERROR_INVALID_USAGE;
    }

    struct fill_info *fi = &s->fi;
    fi->glsl              = texturefill_glsl;
    fi->texture_transform = o->texture_node;
    fi->scaling           = o->scaling;
    fi->wrap              = o->wrap;
    fi->opts              = o;
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct texturefill_priv, fi) == 0,
                   "fill_info must be first in texturefill_priv");

#define OFFSET(x) offsetof(struct texturefill_opts, x)
static const struct node_param texturefill_params[] = {
    {
        .key        = "texture",
        .type       = NGLI_PARAM_TYPE_NODE,
        .offset     = OFFSET(texture_node),
        .node_types = (const uint32_t[]){
            TRANSFORM_TYPES_ARGS,
            NGL_NODE_TEXTURE2D,
            NGL_NODE_CUSTOMTEXTURE,
            NGLI_NODE_NONE,
        },
        .flags = NGLI_PARAM_FLAG_NON_NULL,
        .desc  = NGLI_DOCSTRING("texture to draw"),
    },
    {
        .key     = "wrap",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(wrap),
        .choices = &texturefill_wrap_choices,
        .desc    = NGLI_DOCSTRING("texture wrap behaviour"),
    },
    {
        .key     = "scaling",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(scaling),
        .choices = &texturefill_scaling_choices,
        .desc    = NGLI_DOCSTRING("texture scaling mode relative to the rect"),
    },
    {NULL}
};
#undef OFFSET

const struct node_class ngli_texturefill_class = {
    .id        = NGL_NODE_TEXTUREFILL,
    .name      = "TextureFill",
    .init      = texturefill_init,
    .update    = ngli_node_update_children,
    .opts_size = sizeof(struct texturefill_opts),
    .priv_size = sizeof(struct texturefill_priv),
    .params    = texturefill_params,
    .file      = __FILE__,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * GradientFill
 * ═══════════════════════════════════════════════════════════════════════════ */

struct gradientfill_priv { struct fill_info fi; };

struct gradientfill_opts {
    float color0[3];
    float color1[3];
    float opacity0;
    float opacity1;
    float pos0[2];
    float pos1[2];
    int gradient_mode;
    int gradient_linear;
};

static const struct param_choices gradientfill_mode_choices = {
    .name = "gradient_mode",
    .consts = {
        {"ramp",   0, .desc=NGLI_DOCSTRING("straight line gradient")},
        {"radial", 1, .desc=NGLI_DOCSTRING("radial gradient between the two points")},
        {NULL}
    }
};

static const char gradientfill_glsl[] =
    "vec4 ngl_color(vec2 uv, vec2 tex_coord) {\n"
    "    vec3 c0 = color0 * opacity0;\n"
    "    vec3 c1 = color1 * opacity1;\n"
    "    float aspect = rect_size.x / rect_size.y;\n"
    "    float t = 0.0;\n"
    "    if (gradient_mode == 0) {\n"
    "        vec2 pa = uv - pos0, ba = pos1 - pos0;\n"
    "        pa.x *= aspect; ba.x *= aspect;\n"
    "        t = dot(pa, ba) / dot(ba, ba);\n"
    "    } else {\n"
    "        vec2 pa = uv - pos0, pb = uv - pos1;\n"
    "        pa.x *= aspect; pb.x *= aspect;\n"
    "        float lpa = length(pa);\n"
    "        t = lpa / (lpa + length(pb));\n"
    "    }\n"
    "    float a = mix(opacity0, opacity1, t);\n"
    "    if (gradient_linear != 0) return vec4(ngli_srgbmix(c0, c1, t), a);\n"
    "    return vec4(mix(c0, c1, t), a);\n"
    "}\n";

static int gradientfill_init(struct ngl_node *node)
{
    struct gradientfill_priv *s = node->priv_data;
    const struct gradientfill_opts *o = node->opts;
    struct fill_info *fi = &s->fi;
    fi->helper_flags = FILL_HELPER_SRGB;
    fi->glsl = gradientfill_glsl;
    fi->opts = o;
    REG_UNIFORM(fi, "color0",          NGPU_TYPE_VEC3, struct gradientfill_opts, color0);
    REG_UNIFORM(fi, "color1",          NGPU_TYPE_VEC3, struct gradientfill_opts, color1);
    REG_UNIFORM(fi, "opacity0",        NGPU_TYPE_F32,  struct gradientfill_opts, opacity0);
    REG_UNIFORM(fi, "opacity1",        NGPU_TYPE_F32,  struct gradientfill_opts, opacity1);
    REG_UNIFORM(fi, "pos0",            NGPU_TYPE_VEC2, struct gradientfill_opts, pos0);
    REG_UNIFORM(fi, "pos1",            NGPU_TYPE_VEC2, struct gradientfill_opts, pos1);
    REG_UNIFORM(fi, "gradient_mode",   NGPU_TYPE_I32,  struct gradientfill_opts, gradient_mode);
    REG_UNIFORM(fi, "gradient_linear", NGPU_TYPE_I32,  struct gradientfill_opts, gradient_linear);
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct gradientfill_priv, fi) == 0,
                   "fill_info must be first in gradientfill_priv");

#define OFFSET(x) offsetof(struct gradientfill_opts, x)
static const struct node_param gradientfill_params[] = {
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
        .choices = &gradientfill_mode_choices,
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

const struct node_class ngli_gradientfill_class = {
    .id        = NGL_NODE_GRADIENTFILL,
    .name      = "GradientFill",
    .init      = gradientfill_init,
    .opts_size = sizeof(struct gradientfill_opts),
    .priv_size = sizeof(struct gradientfill_priv),
    .params    = gradientfill_params,
    .file      = __FILE__,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Gradient4Fill
 * ═══════════════════════════════════════════════════════════════════════════ */

struct gradient4fill_priv { struct fill_info fi; };

struct gradient4fill_opts {
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

static const char gradient4fill_glsl[] =
    "#define _g4(tl,tr,br,bl,u) mix(mix(tl,tr,u.x),mix(bl,br,u.x),u.y)\n"
    "vec4 ngl_color(vec2 uv, vec2 tex_coord) {\n"
    "    vec3 tl = color_tl * opacity_tl;\n"
    "    vec3 tr = color_tr * opacity_tr;\n"
    "    vec3 br = color_br * opacity_br;\n"
    "    vec3 bl = color_bl * opacity_bl;\n"
    "    float a = _g4(opacity_tl, opacity_tr, opacity_br, opacity_bl, uv);\n"
    "    if (gradient_linear != 0)\n"
    "        return vec4(ngli_linear2srgb(_g4(ngli_srgb2linear(tl),\n"
    "                                        ngli_srgb2linear(tr),\n"
    "                                        ngli_srgb2linear(br),\n"
    "                                        ngli_srgb2linear(bl), uv)), a);\n"
    "    return vec4(_g4(tl, tr, br, bl, uv), a);\n"
    "}\n";

static int gradient4fill_init(struct ngl_node *node)
{
    struct gradient4fill_priv *s = node->priv_data;
    const struct gradient4fill_opts *o = node->opts;
    struct fill_info *fi = &s->fi;
    fi->helper_flags = FILL_HELPER_SRGB;
    fi->glsl = gradient4fill_glsl;
    fi->opts = o;
    REG_UNIFORM(fi, "color_tl",        NGPU_TYPE_VEC3, struct gradient4fill_opts, color_tl);
    REG_UNIFORM(fi, "color_tr",        NGPU_TYPE_VEC3, struct gradient4fill_opts, color_tr);
    REG_UNIFORM(fi, "color_br",        NGPU_TYPE_VEC3, struct gradient4fill_opts, color_br);
    REG_UNIFORM(fi, "color_bl",        NGPU_TYPE_VEC3, struct gradient4fill_opts, color_bl);
    REG_UNIFORM(fi, "opacity_tl",      NGPU_TYPE_F32,  struct gradient4fill_opts, opacity_tl);
    REG_UNIFORM(fi, "opacity_tr",      NGPU_TYPE_F32,  struct gradient4fill_opts, opacity_tr);
    REG_UNIFORM(fi, "opacity_br",      NGPU_TYPE_F32,  struct gradient4fill_opts, opacity_br);
    REG_UNIFORM(fi, "opacity_bl",      NGPU_TYPE_F32,  struct gradient4fill_opts, opacity_bl);
    REG_UNIFORM(fi, "gradient_linear", NGPU_TYPE_I32,  struct gradient4fill_opts, gradient_linear);
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct gradient4fill_priv, fi) == 0,
                   "fill_info must be first in gradient4fill_priv");

#define OFFSET(x) offsetof(struct gradient4fill_opts, x)
static const struct node_param gradient4fill_params[] = {
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

const struct node_class ngli_gradient4fill_class = {
    .id        = NGL_NODE_GRADIENT4FILL,
    .name      = "Gradient4Fill",
    .init      = gradient4fill_init,
    .opts_size = sizeof(struct gradient4fill_opts),
    .priv_size = sizeof(struct gradient4fill_priv),
    .params    = gradient4fill_params,
    .file      = __FILE__,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * NoiseFill
 * ═══════════════════════════════════════════════════════════════════════════ */

struct noisefill_priv { struct fill_info fi; };

struct noisefill_opts {
    int noise_type;
    float amplitude;
    uint32_t octaves;
    float lacunarity;
    float gain;
    uint32_t seed;
    float scale[2];
    float evolution;
};

static const struct param_choices noisefill_type_choices = {
    .name = "noise_type",
    .consts = {
        {"blocky", 0, .desc=NGLI_DOCSTRING("blocky noise")},
        {"perlin", 1, .desc=NGLI_DOCSTRING("perlin noise")},
        {NULL}
    }
};

static const char noisefill_glsl[] =
    "vec4 ngl_color(vec2 uv, vec2 tex_coord) {\n"
    "    vec2 st = uv * noise_scale;\n"
    "    float n = fbm(vec3(st, noise_evolution), noise_type, noise_amplitude,\n"
    "                  noise_octaves, noise_lacunarity, noise_gain, noise_seed);\n"
    "    n = (n + 1.0) / 2.0;\n"
    "    return vec4(vec3(n), 1.0);\n"
    "}\n";

static int noisefill_init(struct ngl_node *node)
{
    struct noisefill_priv *s = node->priv_data;
    const struct noisefill_opts *o = node->opts;
    struct fill_info *fi = &s->fi;
    fi->helper_flags = FILL_HELPER_MISC_UTILS | FILL_HELPER_NOISE;
    fi->glsl = noisefill_glsl;
    fi->opts = o;
    REG_UNIFORM(fi, "noise_type",       NGPU_TYPE_I32,  struct noisefill_opts, noise_type);
    REG_UNIFORM(fi, "noise_amplitude",  NGPU_TYPE_F32,  struct noisefill_opts, amplitude);
    REG_UNIFORM(fi, "noise_octaves",    NGPU_TYPE_U32,  struct noisefill_opts, octaves);
    REG_UNIFORM(fi, "noise_lacunarity", NGPU_TYPE_F32,  struct noisefill_opts, lacunarity);
    REG_UNIFORM(fi, "noise_gain",       NGPU_TYPE_F32,  struct noisefill_opts, gain);
    REG_UNIFORM(fi, "noise_seed",       NGPU_TYPE_U32,  struct noisefill_opts, seed);
    REG_UNIFORM(fi, "noise_scale",      NGPU_TYPE_VEC2, struct noisefill_opts, scale);
    REG_UNIFORM(fi, "noise_evolution",  NGPU_TYPE_F32,  struct noisefill_opts, evolution);
    return 0;
}

NGLI_STATIC_ASSERT(offsetof(struct noisefill_priv, fi) == 0,
                   "fill_info must be first in noisefill_priv");

#define OFFSET(x) offsetof(struct noisefill_opts, x)
static const struct node_param noisefill_params[] = {
    {
        .key     = "type",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(noise_type),
        .choices = &noisefill_type_choices,
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

const struct node_class ngli_noisefill_class = {
    .id        = NGL_NODE_NOISEFILL,
    .name      = "NoiseFill",
    .init      = noisefill_init,
    .opts_size = sizeof(struct noisefill_opts),
    .priv_size = sizeof(struct noisefill_priv),
    .params    = noisefill_params,
    .file      = __FILE__,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * CustomFill
 * ═══════════════════════════════════════════════════════════════════════════ */

struct customfill_priv {
    struct fill_info fi;
    char *built_glsl;   /* dynamically allocated, owned */
};

struct customfill_opts {
    char *glsl_header;
    char *color_glsl;
    struct ngl_node **frag_resources;
    size_t nb_frag_resources;
    int scaling;
    int wrap;
    int nb_frag_output;
};

static int node_is_texture(const struct ngl_node *node)
{
    return node->cls->id == NGL_NODE_TEXTURE2D ||
           node->cls->id == NGL_NODE_TEXTURE2DARRAY ||
           node->cls->id == NGL_NODE_TEXTURE3D ||
           node->cls->id == NGL_NODE_TEXTURECUBE ||
           node->cls->id == NGL_NODE_TEXTUREVIEW ||
           node->cls->id == NGL_NODE_CUSTOMTEXTURE;
}

static int customfill_init(struct ngl_node *node)
{
    struct customfill_priv *s = node->priv_data;
    const struct customfill_opts *o = node->opts;

    if (!o->color_glsl || !o->color_glsl[0]) {
        LOG(ERROR, "CustomFill: color_glsl param is required");
        return NGL_ERROR_INVALID_USAGE;
    }

    for (size_t i = 0; i < o->nb_frag_resources; i++) {
        const struct ngl_node *res = o->frag_resources[i];
        if (!res->label || !res->label[0]) {
            LOG(ERROR, "CustomFill: frag_resources[%zu]: node label is required as GLSL name", i);
            return NGL_ERROR_INVALID_USAGE;
        }
    }

    /* Build GLSL: optional header + ngl_color()/ngl_colors() body */
    struct bstr *bstr = ngli_bstr_create();
    if (!bstr)
        return NGL_ERROR_MEMORY;

    if (o->glsl_header && o->glsl_header[0])
        ngli_bstr_printf(bstr, "%s\n", o->glsl_header);
    if (o->nb_frag_output > 0)
        ngli_bstr_printf(bstr, "void ngl_colors(vec2 uv, vec2 tex_coord) {\n%s\n}\n", o->color_glsl);
    else
        ngli_bstr_printf(bstr, "vec4 ngl_color(vec2 uv, vec2 tex_coord) {\n%s\n}\n", o->color_glsl);

    if (ngli_bstr_check(bstr) < 0) {
        ngli_bstr_freep(&bstr);
        return NGL_ERROR_MEMORY;
    }
    s->built_glsl = ngli_bstr_strdup(bstr);
    ngli_bstr_freep(&bstr);
    if (!s->built_glsl)
        return NGL_ERROR_MEMORY;

    struct fill_info *fi = &s->fi;
    fi->glsl           = s->built_glsl;
    fi->opts           = o;
    fi->scaling        = o->scaling;
    fi->wrap           = o->wrap;
    fi->nb_frag_output = (size_t)o->nb_frag_output;

    for (size_t i = 0; i < o->nb_frag_resources; i++) {
        struct ngl_node *res = o->frag_resources[i];
        enum ngpu_type type;

        /* Try as a texture (possibly wrapped in transforms) */
        const struct ngl_node *leaf = ngli_transform_get_leaf_node(res);
        if (leaf && node_is_texture(leaf)) {
            ngli_assert(fi->nb_custom_textures < FILL_MAX_TEXTURES);
            struct fill_custom_texture_def *ct = &fi->custom_textures[fi->nb_custom_textures++];
            snprintf(ct->name, sizeof(ct->name), "%s", res->label);
            ct->texture_node = res;
            continue;
        }

        /* Try as a block */
        if (res->cls->id == NGL_NODE_BLOCK) {
            ngli_assert(fi->nb_custom_blocks < FILL_MAX_TEXTURES);
            struct fill_custom_block_def *cb = &fi->custom_blocks[fi->nb_custom_blocks++];
            snprintf(cb->name, sizeof(cb->name), "%s", res->label);
            cb->node = res;
            continue;
        }

        /* Otherwise treat as a uniform */
        int ret = node_to_ngpu_type(res, &type);
        if (ret < 0)
            return ret;
        ngli_assert(fi->nb_custom_uniforms < FILL_MAX_UNIFORMS);
        struct fill_custom_uniform_def *cu = &fi->custom_uniforms[fi->nb_custom_uniforms++];
        snprintf(cu->name, sizeof(cu->name), "%s", res->label);
        cu->type = type;
        cu->node = res;
    }

    return 0;
}

static void customfill_uninit(struct ngl_node *node)
{
    struct customfill_priv *s = node->priv_data;
    ngli_freep(&s->built_glsl);
}

NGLI_STATIC_ASSERT(offsetof(struct customfill_priv, fi) == 0,
                   "fill_info must be first in customfill_priv");

#define OFFSET(x) offsetof(struct customfill_opts, x)
static const struct node_param customfill_params[] = {
    {
        .key    = "glsl_header",
        .type   = NGLI_PARAM_TYPE_STR,
        .offset = OFFSET(glsl_header),
        .desc   = NGLI_DOCSTRING("GLSL code prepended before ngl_color() (helper functions, etc.)"),
    },
    {
        .key    = "color_glsl",
        .type   = NGLI_PARAM_TYPE_STR,
        .offset = OFFSET(color_glsl),
        .desc   = NGLI_DOCSTRING("GLSL body of vec4 ngl_color(vec2 uv, vec2 tex_coord)"),
    },
    {
        .key        = "frag_resources",
        .type       = NGLI_PARAM_TYPE_NODELIST,
        .offset     = OFFSET(frag_resources),
        .node_types = (const uint32_t[]){
            NGL_NODE_UNIFORMFLOAT,  NGL_NODE_UNIFORMVEC2,  NGL_NODE_UNIFORMVEC3,
            NGL_NODE_UNIFORMVEC4,   NGL_NODE_UNIFORMCOLOR, NGL_NODE_UNIFORMQUAT,
            NGL_NODE_UNIFORMMAT4,   NGL_NODE_UNIFORMINT,   NGL_NODE_UNIFORMIVEC2,
            NGL_NODE_UNIFORMIVEC3,  NGL_NODE_UNIFORMIVEC4, NGL_NODE_UNIFORMUINT,
            NGL_NODE_UNIFORMUIVEC2, NGL_NODE_UNIFORMUIVEC3,NGL_NODE_UNIFORMUIVEC4,
            NGL_NODE_UNIFORMBOOL,
            NGL_NODE_EVALFLOAT,    NGL_NODE_EVALVEC2,
            NGL_NODE_EVALVEC3,     NGL_NODE_EVALVEC4,
            NGL_NODE_ANIMATEDFLOAT,NGL_NODE_ANIMATEDVEC2,
            NGL_NODE_ANIMATEDVEC3, NGL_NODE_ANIMATEDVEC4,
            NGL_NODE_ANIMATEDQUAT, NGL_NODE_ANIMATEDCOLOR,
            NGL_NODE_NOISEFLOAT,   NGL_NODE_NOISEVEC2,
            NGL_NODE_NOISEVEC3,    NGL_NODE_NOISEVEC4,
            NGL_NODE_TIME,
            TRANSFORM_TYPES_ARGS,
            NGL_NODE_TEXTURE2D,    NGL_NODE_TEXTURE2DARRAY,
            NGL_NODE_TEXTURE3D,    NGL_NODE_TEXTURECUBE,
            NGL_NODE_TEXTUREVIEW,  NGL_NODE_CUSTOMTEXTURE,
            NGL_NODE_BLOCK,
            NGLI_NODE_NONE,
        },
        .desc = NGLI_DOCSTRING("uniform and texture nodes available to color_glsl; "
                               "each node's label is used as the GLSL name"),
    },
    {
        .key     = "scaling",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(scaling),
        .choices = &texturefill_scaling_choices,
        .desc    = NGLI_DOCSTRING("texture scaling mode applied to custom fill content"),
    },
    {
        .key     = "wrap",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET(wrap),
        .choices = &texturefill_wrap_choices,
        .desc    = NGLI_DOCSTRING("wrap mode for out-of-bounds coordinates"),
    },
    {
        .key    = "nb_frag_output",
        .type   = NGLI_PARAM_TYPE_I32,
        .offset = OFFSET(nb_frag_output),
        .desc   = NGLI_DOCSTRING("number of fragment outputs for MRT rendering "
                                 "(0 means single output using ngl_color(), "
                                 ">0 uses ngl_colors() writing to ngl_out_color[])"),
    },
    {NULL}
};
#undef OFFSET

const struct node_class ngli_customfill_class = {
    .id        = NGL_NODE_CUSTOMFILL,
    .name      = "CustomFill",
    .init      = customfill_init,
    .update    = ngli_node_update_children,
    .uninit    = customfill_uninit,
    .opts_size = sizeof(struct customfill_opts),
    .priv_size = sizeof(struct customfill_priv),
    .params    = customfill_params,
    .file      = __FILE__,
};
