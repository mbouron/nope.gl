/*
 * Copyright 2026 Nope Forge
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
#include "node_mask.h"
#include "nopegl/nopegl.h"
#include "params.h"

/* ─────────────────────────── MaskBlur ─────────────────────────────────── */

/*
 * All blur GLSL variants use `mask_sigma` (a uniform) and fall back to `aa`
 * when sigma is very small so the result is always smooth.
 *
 * `d`    : SDF distance (negative inside, positive outside, zero at boundary)
 * `edge` : stroke inner/outer edge offset from the shape boundary
 * `sd`   : signed distance from the edge (sd = d - edge)
 */

/* Normal: symmetric blur on both sides of the edge */
static const char maskblur_normal_glsl[] =
    "float ngl_mask_alpha(vec2 uv, float d, float edge, float aa) {\n"
    "    float sd = d - edge;\n"
    "    float s = max(mask_sigma, aa);\n"
    "    return 1.0 - smoothstep(-s, s, sd);\n"
    "}\n";

/* Solid: inside fully opaque, blurred outer edge */
static const char maskblur_solid_glsl[] =
    "float ngl_mask_alpha(vec2 uv, float d, float edge, float aa) {\n"
    "    float sd = d - edge;\n"
    "    float s = max(mask_sigma, aa);\n"
    "    return 1.0 - smoothstep(0.0, s, sd);\n"
    "}\n";

/* Outer: outer glow only, interior is transparent */
static const char maskblur_outer_glsl[] =
    "float ngl_mask_alpha(vec2 uv, float d, float edge, float aa) {\n"
    "    float sd = d - edge;\n"
    "    float s = max(mask_sigma, aa);\n"
    "    return (1.0 - smoothstep(0.0, s, sd)) * step(0.0, sd);\n"
    "}\n";

/* Inner: inner fade only, exterior is transparent */
static const char maskblur_inner_glsl[] =
    "float ngl_mask_alpha(vec2 uv, float d, float edge, float aa) {\n"
    "    float sd = d - edge;\n"
    "    float s = max(mask_sigma, aa);\n"
    "    return (1.0 - smoothstep(-s, 0.0, sd)) * step(sd, 0.0);\n"
    "}\n";

static const char * const maskblur_glsl_table[] = {
    [MASK_BLUR_NORMAL] = maskblur_normal_glsl,
    [MASK_BLUR_SOLID]  = maskblur_solid_glsl,
    [MASK_BLUR_OUTER]  = maskblur_outer_glsl,
    [MASK_BLUR_INNER]  = maskblur_inner_glsl,
};

const struct param_choices ngli_maskblur_style_choices = {
    .name = "mask_blur_style",
    .consts = {
        {"normal", MASK_BLUR_NORMAL, .desc=NGLI_DOCSTRING("blur both sides of the edge symmetrically")},
        {"solid",  MASK_BLUR_SOLID,  .desc=NGLI_DOCSTRING("keep inside fully opaque, blur the outer edge")},
        {"outer",  MASK_BLUR_OUTER,  .desc=NGLI_DOCSTRING("outer glow only, interior is transparent")},
        {"inner",  MASK_BLUR_INNER,  .desc=NGLI_DOCSTRING("inner fade only, exterior is transparent")},
        {NULL}
    }
};

struct maskblur_opts {
    float sigma;
    int style;
};

struct maskblur_priv {
    struct mask_info info;
};

#define OFFSET_BLUR(x) offsetof(struct maskblur_opts, x)

static int maskblur_init(struct ngl_node *node)
{
    struct maskblur_priv *s = node->priv_data;
    const struct maskblur_opts *o = node->opts;

    s->info.glsl      = maskblur_glsl_table[o->style];
    s->info.dilation  = o->sigma * 3.0f;
    s->info.opts      = o;

    snprintf(s->info.uniforms[0].name, sizeof(s->info.uniforms[0].name), "mask_sigma");
    s->info.uniforms[0].type        = NGPU_TYPE_F32;
    s->info.uniforms[0].opts_offset = OFFSET_BLUR(sigma);
    s->info.nb_uniforms = 1;

    return 0;
}

static const struct node_param maskblur_params[] = {
    {
        .key       = "sigma",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET_BLUR(sigma),
        .def_value = {.f32 = 5.0f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("blur radius in pixels"),
    },
    {
        .key     = "style",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET_BLUR(style),
        .choices = &ngli_maskblur_style_choices,
        .desc    = NGLI_DOCSTRING("blur style controlling how alpha is distributed around the edge"),
    },
    {NULL}
};

const struct node_class ngli_maskblur_class = {
    .id        = NGL_NODE_MASKBLUR,
    .name      = "MaskBlur",
    .init      = maskblur_init,
    .opts_size = sizeof(struct maskblur_opts),
    .priv_size = sizeof(struct maskblur_priv),
    .params    = maskblur_params,
    .file      = __FILE__,
};

/* ─────────────────────────── MaskTexture ───────────────────────────────── */

/*
 * Each variant multiplies the standard SDF-based alpha by the selected channel
 * of the mask texture sampled at the raw UV coordinate.
 */

static const char masktexture_alpha_glsl[] =
    "float ngl_mask_alpha(vec2 uv, float d, float edge, float aa) {\n"
    "    float base = 1.0 - smoothstep(edge - aa, edge + aa, d);\n"
    "    return base * ngl_texvideo(mask_tex, uv).a;\n"
    "}\n";

static const char masktexture_luminance_glsl[] =
    "float ngl_mask_alpha(vec2 uv, float d, float edge, float aa) {\n"
    "    float base = 1.0 - smoothstep(edge - aa, edge + aa, d);\n"
    "    vec4 tc = ngl_texvideo(mask_tex, uv);\n"
    "    return base * dot(tc.rgb, vec3(0.299, 0.587, 0.114));\n"
    "}\n";

static const char masktexture_red_glsl[] =
    "float ngl_mask_alpha(vec2 uv, float d, float edge, float aa) {\n"
    "    float base = 1.0 - smoothstep(edge - aa, edge + aa, d);\n"
    "    return base * ngl_texvideo(mask_tex, uv).r;\n"
    "}\n";

static const char masktexture_green_glsl[] =
    "float ngl_mask_alpha(vec2 uv, float d, float edge, float aa) {\n"
    "    float base = 1.0 - smoothstep(edge - aa, edge + aa, d);\n"
    "    return base * ngl_texvideo(mask_tex, uv).g;\n"
    "}\n";

static const char masktexture_blue_glsl[] =
    "float ngl_mask_alpha(vec2 uv, float d, float edge, float aa) {\n"
    "    float base = 1.0 - smoothstep(edge - aa, edge + aa, d);\n"
    "    return base * ngl_texvideo(mask_tex, uv).b;\n"
    "}\n";

static const char * const masktexture_glsl_table[] = {
    [MASK_CHANNEL_ALPHA]     = masktexture_alpha_glsl,
    [MASK_CHANNEL_LUMINANCE] = masktexture_luminance_glsl,
    [MASK_CHANNEL_RED]       = masktexture_red_glsl,
    [MASK_CHANNEL_GREEN]     = masktexture_green_glsl,
    [MASK_CHANNEL_BLUE]      = masktexture_blue_glsl,
};

const struct param_choices ngli_masktexture_channel_choices = {
    .name = "mask_texture_channel",
    .consts = {
        {"alpha",     MASK_CHANNEL_ALPHA,     .desc=NGLI_DOCSTRING("alpha channel")},
        {"luminance", MASK_CHANNEL_LUMINANCE, .desc=NGLI_DOCSTRING("luminance (weighted RGB)")},
        {"red",       MASK_CHANNEL_RED,       .desc=NGLI_DOCSTRING("red channel")},
        {"green",     MASK_CHANNEL_GREEN,     .desc=NGLI_DOCSTRING("green channel")},
        {"blue",      MASK_CHANNEL_BLUE,      .desc=NGLI_DOCSTRING("blue channel")},
        {NULL}
    }
};

struct masktexture_opts {
    struct ngl_node *texture_node;
    int channel;
};

struct masktexture_priv {
    struct mask_info info;
};

#define OFFSET_TEX(x) offsetof(struct masktexture_opts, x)

static int masktexture_init(struct ngl_node *node)
{
    struct masktexture_priv *s = node->priv_data;
    const struct masktexture_opts *o = node->opts;

    s->info.glsl         = masktexture_glsl_table[o->channel];
    s->info.texture_node = o->texture_node;
    s->info.dilation     = 0.0f;
    s->info.opts         = o;

    return 0;
}

static const struct node_param masktexture_params[] = {
    {
        .key        = "texture",
        .type       = NGLI_PARAM_TYPE_NODE,
        .offset     = OFFSET_TEX(texture_node),
        .node_types = (const uint32_t[]){NGL_NODE_TEXTURE2D, NGLI_NODE_NONE},
        .flags      = NGLI_PARAM_FLAG_NON_NULL,
        .desc       = NGLI_DOCSTRING("texture used as an alpha mask"),
    },
    {
        .key     = "channel",
        .type    = NGLI_PARAM_TYPE_SELECT,
        .offset  = OFFSET_TEX(channel),
        .choices = &ngli_masktexture_channel_choices,
        .desc    = NGLI_DOCSTRING("texture channel used for the mask value"),
    },
    {NULL}
};

const struct node_class ngli_masktexture_class = {
    .id        = NGL_NODE_MASKTEXTURE,
    .name      = "MaskTexture",
    .init      = masktexture_init,
    .opts_size = sizeof(struct masktexture_opts),
    .priv_size = sizeof(struct masktexture_priv),
    .params    = masktexture_params,
    .file      = __FILE__,
};
