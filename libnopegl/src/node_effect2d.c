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
#include "image.h"
#include "internal.h"
#include "log.h"
#include "node2d.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "node_effect2d_shader.h"
#include "shader2d.h"
#include "node_uniform.h"
#include "nopegl/nopegl.h"
#include "pipeline.h"
#include "rtt.h"
#include "node_block.h"
#include "node_mask.h"
#include "node_texture.h"
#include "utils/bstr.h"
#include "utils/darray.h"
#include "utils/hmap.h"
#include "utils/memory.h"
#include "utils/string.h"
#include "utils/utils.h"

/* GLSL fragments as string */
#include "effect2d_composite_frag.h"
#include "effect2d_composite_vert.h"


struct uniform_map {
    int32_t index;
    const void *data;
};

NGLI_DECLARE_DARRAY_WITH_NAME(effect2d_texture_darray, struct ngpu_pgcraft_texture);
NGLI_DECLARE_DARRAY_WITH_NAME(effect2d_block_darray, struct ngpu_pgcraft_block);

struct effect2d_vert_block {
    struct ngli_mat4 projection_matrix;
    struct ngli_mat4 modelview_matrix;
    float rect[4];
    float effect_rect[4];
    float quad_tex_scale[2];
    float quad_tex_offset[2];
};

struct effect2d_frag_block {
    float opacity;
    float _pad0;
    float input_scale[2];
    float input_offset[2];
    float rect_size[2];
};

struct effect2d_opts {
    struct ngli_node_darray children;
    int bounds;
    struct ngl_node *rect_node;
    float rect[4];
    float dilation;
    struct ngl_node *mask_node;
    int mask_channel;
    struct ngli_node_darray mask_children;
    struct ngli_node2d_opts node2d;
    struct ngl_node *enabled_node;
    int enabled;
    struct ngli_node_darray shaders;
};

enum {
    EFFECT2D_BOUNDS_CHILDREN,
    EFFECT2D_BOUNDS_CANVAS,
    EFFECT2D_BOUNDS_RECT,
};

static const struct param_choices bounds_choices = {
    .name = "effect2d_bounds",
    .consts = {
        {"children", EFFECT2D_BOUNDS_CHILDREN, .desc=NGLI_DOCSTRING("children bounding box")},
        {"canvas",   EFFECT2D_BOUNDS_CANVAS,   .desc=NGLI_DOCSTRING("box (0, 0, canvas width, canvas height) in "
                                                                    "local 2D space")},
        {"rect",     EFFECT2D_BOUNDS_RECT,     .desc=NGLI_DOCSTRING("box specified by `rect`")},
        {NULL}
    }
};

struct effect2d_program {
    struct hmap *resources;
    const char *source_label;
    char *frag_glsl;
    struct shader2d shader;
    struct ngpu_block_desc user_block_desc;
    size_t user_block_size;
    NGLI_DARRAY(int32_t) user_field_indices;
    NGLI_DARRAY(struct ngl_node *) user_nodes;
    struct effect2d_texture_darray crafter_textures;
    struct effect2d_block_darray crafter_blocks;
    int32_t vert_block_index;
    int32_t frag_block_index;
    int32_t user_block_index;
    struct ngpu_pgcraft *crafter;
    struct ngli_pipeline *pipeline;
};

struct effect2d_priv {
    struct ngli_node2d_info node2d_info;

    struct hmap *mask_resources;
    struct rtt_ctx *mask_rtt;
    struct image_resource *mask_image;

    struct rtt_ctx *rtt;
    struct image_resource *input_image;
    struct ngpu_rendertarget_layout layout;
    float local_effect_margin;
    float rect[4];
    float effect_rect[4];

    /* Built-in uniform blocks shared by every shader program */
    struct ngpu_block_desc vert_block_desc;
    size_t vert_block_size;
    struct ngpu_block_desc frag_block_desc;
    size_t frag_block_size;

    NGLI_DARRAY(struct effect2d_program) programs;
    size_t active_program_index;
    bool drawme;
};

#define OFFSET(x) offsetof(struct effect2d_opts, x)
static const struct node_param effect2d_params[] = {
    {
        .key       = "children",
        .type      = NGLI_PARAM_TYPE_NODELIST,
        .offset    = OFFSET(children),
        .flags      = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .node_types = NGLI_NODE2D_TYPES_LIST,
        .desc       = NGLI_DOCSTRING("2D scenes to render offscreen"),
    }, {
        .key       = "bounds",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(bounds),
        .def_value = {.i32=EFFECT2D_BOUNDS_CHILDREN},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .choices   = &bounds_choices,
        .desc      = NGLI_DOCSTRING("bounds used for offscreen rendering and compositing"),
    }, {
        .key       = "rect",
        .type      = NGLI_PARAM_TYPE_VEC4,
        .offset    = OFFSET(rect_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("custom bounds (x, y, width, height) in local 2D space; used when `bounds` "
                                    "is `rect`"),
    }, {
        .key       = "dilation",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(dilation),
        .desc      = NGLI_DOCSTRING("geometry dilation in pixels to accommodate effects that extend beyond children bounds"),
    }, {
        .key       = "translate",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.translate_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("translation in pixels"),
    }, {
        .key       = "rotation",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(node2d.rotation_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("rotation angle in degrees"),
    }, {
        .key       = "scale",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.scale_node),
        .def_value = {.vec={1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("scale factors"),
    }, {
        .key       = "anchor",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.anchor_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("anchor/pivot point in pixels"),
    }, {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(node2d.opacity_node),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("opacity of the composited result"),
    }, {
        .key       = "visible",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(node2d.visible),
        .def_value = {.i32=1},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("whether the effect and its children are visible"),
    }, {
        .key       = "blend_mode",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(node2d.blend_mode),
        .def_value = {.i32=NGLI_BLEND_MODE_SRC_OVER},
        .choices   = &ngli_blend_mode_choices,
        .desc      = NGLI_DOCSTRING("define how the rendered effect is composited with the current framebuffer"),
    }, {
        .key       = "enabled",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(enabled_node),
        .def_value = {.i32=1},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("whether to apply shader processing before compositing"),
    }, {
        .key        = "shaders",
        .type       = NGLI_PARAM_TYPE_NODELIST,
        .offset     = OFFSET(shaders),
        .node_types = (const uint32_t[]){NGL_NODE_EFFECT2DSHADER, NGLI_NODE_NONE},
        .desc       = NGLI_DOCSTRING("timed shaders in priority order; the first active shader is used, or "
                                     "passthrough if none is active"),
    },
    {NULL}
};

static const struct node_param layer2d_params[] = {
    {
        .key        = "children",
        .type       = NGLI_PARAM_TYPE_NODELIST,
        .offset     = OFFSET(children),
        .flags      = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .node_types = NGLI_NODE2D_TYPES_LIST,
        .desc       = NGLI_DOCSTRING("2D scenes to render as one isolated layer"),
    },
    {
        .key       = "translate",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.translate_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("translation in pixels"),
    }, {
        .key       = "rotation",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(node2d.rotation_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("rotation angle in degrees"),
    }, {
        .key       = "scale",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.scale_node),
        .def_value = {.vec={1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("scale factors"),
    }, {
        .key       = "anchor",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.anchor_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("anchor/pivot point in pixels"),
    }, {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(node2d.opacity_node),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("opacity of the composited layer"),
    }, {
        .key       = "visible",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(node2d.visible),
        .def_value = {.i32=1},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("whether the layer and its children are visible"),
    }, {
        .key       = "blend_mode",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(node2d.blend_mode),
        .def_value = {.i32=NGLI_BLEND_MODE_SRC_OVER},
        .choices   = &ngli_blend_mode_choices,
        .desc      = NGLI_DOCSTRING("define how the composited layer is blended with the current framebuffer"),
    },
    {NULL}
};

static const struct node_param mask2d_params[] = {
    {
        .key        = "children",
        .type       = NGLI_PARAM_TYPE_NODELIST,
        .offset     = OFFSET(children),
        .flags      = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .node_types = NGLI_NODE2D_TYPES_LIST,
        .desc       = NGLI_DOCSTRING("2D scenes to render before applying the mask"),
    }, {
        .key       = "mask_rect",
        .type      = NGLI_PARAM_TYPE_VEC4,
        .offset    = OFFSET(rect_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("rectangle (x, y, width, height) in local 2D space the mask image covers, "
                                    "the children being cut outside of it; when width or height is 0, the mask "
                                    "covers the composited children bounds"),
    }, {
        .key        = "mask",
        .type       = NGLI_PARAM_TYPE_NODE,
        .offset     = OFFSET(mask_node),
        .node_types = (const uint32_t[]){NGL_NODE_TEXTURE2D, NGL_NODE_CUSTOMTEXTURE, NGLI_NODE_NONE},
        .desc       = NGLI_DOCSTRING("Texture2D or CustomTexture sampled as the mask; the image covers "
                                     "`mask_rect`, or the composited children bounds, their anti-aliased edges "
                                     "included; when unset, the mask is `mask_children`"),
    }, {
        .key       = "channel",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(mask_channel),
        .choices   = &ngli_masktexture_channel_choices,
        .desc      = NGLI_DOCSTRING("channel of mask used as coverage; luminance uses the sRGB / BT.709 weights"),
    }, {
        .key        = "mask_children",
        .type       = NGLI_PARAM_TYPE_NODELIST,
        .offset     = OFFSET(mask_children),
        .flags      = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .node_types = NGLI_NODE2D_TYPES_LIST,
        .desc       = NGLI_DOCSTRING("2D scenes drawn as the mask when `mask` is unset: they are rendered in the "
                                     "same local space and at the same resolution as the children, so a mask "
                                     "shape lines up with the content it masks; they do not extend the bounds"),
    },
    {
        .key       = "translate",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.translate_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("translation in pixels"),
    }, {
        .key       = "rotation",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(node2d.rotation_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("rotation angle in degrees"),
    }, {
        .key       = "scale",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.scale_node),
        .def_value = {.vec={1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("scale factors"),
    }, {
        .key       = "anchor",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(node2d.anchor_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("anchor/pivot point in pixels"),
    }, {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(node2d.opacity_node),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("opacity of the masked result"),
    }, {
        .key       = "visible",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(node2d.visible),
        .def_value = {.i32=1},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("whether the masked result and its children are visible"),
    }, {
        .key       = "blend_mode",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(node2d.blend_mode),
        .def_value = {.i32=NGLI_BLEND_MODE_SRC_OVER},
        .choices   = &ngli_blend_mode_choices,
        .desc      = NGLI_DOCSTRING("define how the masked result is blended with the current framebuffer"),
    },
    {NULL}
};

static const char * const mask2d_coverage_glsl[] = {
    [MASK_CHANNEL_ALPHA]     = "mask.a",
    [MASK_CHANNEL_LUMINANCE] = "dot(mask.rgb, vec3(0.2126, 0.7152, 0.0722))", /* sRGB / BT.709 */
    [MASK_CHANNEL_RED]       = "mask.r",
    [MASK_CHANNEL_GREEN]     = "mask.g",
    [MASK_CHANNEL_BLUE]      = "mask.b",
};

static int node_is_texture(const struct ngl_node *node)
{
    return node->cls->id == NGL_NODE_TEXTURE2D ||
           node->cls->id == NGL_NODE_TEXTURE2DARRAY ||
           node->cls->id == NGL_NODE_TEXTURE3D ||
           node->cls->id == NGL_NODE_TEXTURECUBE ||
           node->cls->id == NGL_NODE_CUSTOMTEXTURE;
}

static int register_uniform(const char *name, struct ngl_node *res, struct effect2d_program *program)
{
    const struct variable_info *var = res->priv_data;
    const int field_idx = ngpu_block_desc_add_field(&program->user_block_desc, name, var->data_type, 0);
    if (field_idx < 0)
        return field_idx;
    if (ngli_darray_try_push(&program->user_field_indices, field_idx) < 0)
        return NGL_ERROR_MEMORY;
    if (ngli_darray_try_push(&program->user_nodes, res) < 0)
        return NGL_ERROR_MEMORY;
    return 0;
}

static int register_texture(const char *name, struct ngl_node *res, struct effect2d_texture_darray *textures)
{
    struct texture_info *texture_info = ngli_node_texture_get_texture_info(res);
    struct ngpu_pgcraft_texture tex = {
        .type        = ngli_node_texture_get_pgcraft_texture_type(res),
        .stage       = NGPU_PROGRAM_STAGE_FRAG,
        .format      = texture_info->params.format,
        .clamp_video = texture_info->clamp_video,
        .premult     = texture_info->premult,
    };
    snprintf(tex.name, sizeof(tex.name), "%s", name);
    return ngli_darray_try_push(textures, tex) < 0 ? NGL_ERROR_MEMORY : 0;
}

static int register_block(const char *name, struct ngl_node *res, struct ngpu_ctx *gpu_ctx, struct effect2d_block_darray *blocks)
{
    struct block_info *block_info = res->priv_data;
    struct ngpu_block_desc *block = &block_info->block;
    const size_t block_size = ngpu_block_desc_get_size(block, 0);

    enum ngpu_type btype = NGPU_TYPE_UNIFORM_BUFFER;
    if (block->layout == NGPU_BLOCK_LAYOUT_STD430) {
        btype = NGPU_TYPE_STORAGE_BUFFER;
    } else {
        const struct ngpu_limits *limits = ngpu_ctx_get_limits(gpu_ctx);
        if (block_size > limits->max_uniform_block_size)
            btype = NGPU_TYPE_STORAGE_BUFFER;
    }

    int ret = ngli_node_block_extend_usage_from_type(res, btype);
    if (ret < 0)
        return ret;

    struct ngpu_pgcraft_block crafter_block = {
        .type   = btype,
        .stage  = NGPU_PROGRAM_STAGE_FRAG,
        .block  = block,
    };
    snprintf(crafter_block.name, sizeof(crafter_block.name), "%s", name);
    return ngli_darray_try_push(blocks, crafter_block) < 0 ? NGL_ERROR_MEMORY : 0;
}

static int register_resource(const char *name, struct ngl_node *res, struct ngpu_ctx *gpu_ctx,
                             struct effect2d_program *program)
{
    if (node_is_texture(res))
        return register_texture(name, res, &program->crafter_textures);
    if (res->cls->id == NGL_NODE_BLOCK)
        return register_block(name, res, gpu_ctx, &program->crafter_blocks);
    return register_uniform(name, res, program);
}

static int register_resources(struct hmap *resources, struct ngpu_ctx *gpu_ctx,
                              struct effect2d_program *program)
{
    if (resources) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(resources, entry))) {
            int ret = register_resource(entry->key.str, entry->data, gpu_ctx, program);
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

static void reset_program(struct effect2d_program *program)
{
    ngli_pipeline_freep(&program->pipeline);
    ngpu_pgcraft_freep(&program->crafter);
    ngli_freep(&program->frag_glsl);
    ngli_shader2d_reset(&program->shader);
    ngli_darray_reset(&program->user_field_indices);
    ngli_darray_reset(&program->user_nodes);
    ngli_darray_reset(&program->crafter_textures);
    ngli_darray_reset(&program->crafter_blocks);
    ngpu_block_desc_reset(&program->user_block_desc);
}

static const struct shader2d_role shader_role = {.name = "effect", .uv = "ngli_rect_uv"};

/* The input mapping includes the effect's sampling correction in addition to
 * the image's coordinates. Use the transform already uploaded by Effect2D. */
static void write_input_helpers(struct bstr *out)
{
    ngli_bstr_print(out,
        "vec2 ngl_tex_uv_input(const ngl_FragmentInput frag) { return frag.rect_uv; }\n"
        "vec2 ngl_tex_uv_input(const ngl_FragmentInput frag, vec2 p) { return p; }\n"
        "vec2 ngl_tex_coord_input(const ngl_FragmentInput frag) { return ngli_tex_coord; }\n"
        "vec2 ngl_tex_coord_input(const ngl_FragmentInput frag, vec2 p) {\n"
        "    return p * ngli_input_scale + ngli_input_offset;\n"
        "}\n"
        "vec4 ngl_sample_input(const ngl_FragmentInput frag) { return ngl_texvideo(ngl_input, ngli_tex_coord); }\n"
        "vec4 ngl_sample_input(const ngl_FragmentInput frag, vec2 uv) {\n"
        "    return ngl_texvideo(ngl_input, uv * ngli_input_scale + ngli_input_offset);\n"
        "}\n");
}

static int add_program(struct ngl_node *node, const char *glsl,
                       struct hmap *resources, bool premult, const char *source_label)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    struct effect2d_program program = {
        .resources = resources,
        .source_label = source_label,
        .user_block_index = -1,
    };

    ngli_shader2d_init(&program.shader);
    ngpu_block_desc_init(gpu_ctx, &program.user_block_desc, NGPU_BLOCK_LAYOUT_STD140);

    if (!ngli_str_is_empty(glsl)) {
        struct bstr *bstr = ngli_bstr_create();
        if (!bstr) {
            reset_program(&program);
            return NGL_ERROR_MEMORY;
        }

        int ret = 0;
        if (resources) {
            const struct hmap_entry *entry = NULL;
            while (ret >= 0 && (entry = ngli_hmap_next(resources, entry))) {
                const struct ngl_node *res = entry->data;
                if (!node_is_texture(res))
                    continue;
                const enum ngpu_pgcraft_texture_type type = ngli_node_texture_get_pgcraft_texture_type(res);
                if (type == NGPU_PGCRAFT_TEXTURE_TYPE_2D || type == NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO)
                    ret = ngli_shader2d_add_texture(&program.shader, entry->key.str);
            }
        }
        if (ret < 0) {
            ngli_bstr_freep(&bstr);
            reset_program(&program);
            return ret;
        }
        ngli_shader2d_write_header(&program.shader, bstr);
        write_input_helpers(bstr);
        ret = ngli_shader2d_write_source(bstr, glsl, "ngli_effect", false, 3, source_label);
        if (ret < 0) {
            ngli_bstr_freep(&bstr);
            reset_program(&program);
            return ret;
        }
        ngli_bstr_print(bstr, "void main() {\n"
                             "    ngl_FragmentInput frag;\n"
                             "    frag.rect_uv = ngli_rect_uv;\n"
                             "    frag.rect_size = ngli_effect_size;\n"
                             "    frag.content_uv = ngli_rect_uv;\n"
                             "    frag.canvas_px = ngli_canvas_px;\n"
                             "    frag._uv = ngli_rect_uv;\n"
                             "    frag._lin = vec4(1.0, 0.0, 0.0, 1.0);\n"
                             "    frag._off = vec2(0.0);\n"
                             "    frag._content_lin = frag._lin;\n"
                             "    frag._content_off = vec2(0.0);\n");
        ngli_shader2d_write_fragment_input_textures(&program.shader, bstr, &shader_role);
        ngli_bstr_print(bstr, "    vec4 color = ngli_effect(frag);\n");
        if (premult)
            ngli_bstr_printf(bstr, "    color.rgb *= color.a;\n");
        ngli_bstr_printf(bstr, "    ngl_out_color = color * opacity;\n");
        ngli_bstr_printf(bstr, "}\n");

        program.frag_glsl = ngli_bstr_strdup(bstr);
        ngli_bstr_freep(&bstr);
        if (!program.frag_glsl) {
            reset_program(&program);
            return NGL_ERROR_MEMORY;
        }
    }

    int ret = register_resources(resources, gpu_ctx, &program);
    if (ret < 0) {
        reset_program(&program);
        return ret;
    }

    if (program.user_field_indices.count > 0)
        program.user_block_size = ngpu_block_desc_get_size(&program.user_block_desc, 0);

    if (ngli_darray_try_push(&s->programs, program) < 0) {
        reset_program(&program);
        return NGL_ERROR_MEMORY;
    }
    return 0;
}

static int effect2d_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    int ret;

    s->input_image = ngli_image_resource_create();
    if (!s->input_image)
        return NGL_ERROR_MEMORY;

    s->layout.nb_colors = 1;
    s->layout.colors[0].format = NGPU_FORMAT_R8G8B8A8_UNORM;

    const struct effect2d_opts *o = node->opts;

    /* Build vertex uniform block descriptor */
    ngpu_block_desc_init(gpu_ctx, &s->vert_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    static const struct ngpu_block_field vert_fields[] = {
        {.name = "projection_matrix", .type = NGPU_TYPE_MAT4},
        {.name = "modelview_matrix",  .type = NGPU_TYPE_MAT4},
        {.name = "rect",              .type = NGPU_TYPE_VEC4},
        {.name = "effect_rect",       .type = NGPU_TYPE_VEC4},
        {.name = "ngli_quad_tex_scale",  .type = NGPU_TYPE_VEC2},
        {.name = "ngli_quad_tex_offset", .type = NGPU_TYPE_VEC2},
    };
    ret = ngpu_block_desc_add_fields(&s->vert_block_desc, vert_fields, NGLI_ARRAY_NB(vert_fields));
    if (ret < 0)
        return ret;
    s->vert_block_size = ngpu_block_desc_get_size(&s->vert_block_desc, 0);
    ngli_assert(s->vert_block_size == sizeof(struct effect2d_vert_block));

    /* Build fragment uniform block descriptor */
    ngpu_block_desc_init(gpu_ctx, &s->frag_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    static const struct ngpu_block_field frag_fields[] = {
        {.name = "opacity", .type = NGPU_TYPE_F32},
        {.name = "ngli_input_scale",  .type = NGPU_TYPE_VEC2},
        {.name = "ngli_input_offset", .type = NGPU_TYPE_VEC2},
        {.name = "ngli_effect_size", .type = NGPU_TYPE_VEC2},
    };
    ret = ngpu_block_desc_add_fields(&s->frag_block_desc, frag_fields, NGLI_ARRAY_NB(frag_fields));
    if (ret < 0)
        return ret;
    s->frag_block_size = ngpu_block_desc_get_size(&s->frag_block_desc, 0);
    ngli_assert(s->frag_block_size == sizeof(struct effect2d_frag_block));

    /*
     * Program 0 is always the passthrough used for gaps and the master bypass;
     * for a Mask2D, it is the masking composite.
     */
    if (node->cls->id == NGL_NODE_MASK2D) {
        const char *mask_coord = "ngl_tex_coord_input(frag)";
        if (o->mask_node) {
            if (o->mask_children.count) {
                LOG(ERROR, "mask and mask_children are mutually exclusive");
                return NGL_ERROR_INVALID_USAGE;
            }
            s->mask_resources = ngli_hmap_try_create(NGLI_HMAP_TYPE_STR);
            if (!s->mask_resources)
                return NGL_ERROR_MEMORY;
            ret = ngli_hmap_try_set_str(s->mask_resources, "ngl_mask", o->mask_node);
            if (ret < 0)
                return ret;
            mask_coord = "ngl_tex_coord_ngl_mask(frag)";
        } else {
            s->mask_image = ngli_image_resource_create();
            if (!s->mask_image)
                return NGL_ERROR_MEMORY;
        }
        char glsl[512];
        snprintf(glsl, sizeof(glsl),
                 "vec4 main(const ngl_FragmentInput frag) {\n"
                 "    vec4 mask = ngl_texvideo(ngl_mask, %s);\n"
                 "    return ngl_sample_input(frag) * %s;\n"
                 "}\n",
                 mask_coord, mask2d_coverage_glsl[o->mask_channel]);
        ret = add_program(node, glsl, s->mask_resources, false, "mask");
    } else {
        ret = add_program(node, NULL, NULL, false, "passthrough");
    }
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < o->shaders.count; i++) {
        const struct effect2d_shader_info info = ngli_effect2d_shader_get_info(o->shaders.data[i]);
        ret = add_program(node, info.glsl, info.resources, info.premult, o->shaders.data[i]->label);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static void effect2d_get_rendertarget_layout(const struct ngl_node *node,
                                             struct ngpu_rendertarget_layout *layout)
{
    const struct effect2d_priv *s = node->priv_data;
    *layout = s->layout;
}

static int build_crafter_blocks(const struct ngl_node *node, const struct effect2d_program *program,
                                bool has_user_uniforms, struct effect2d_block_darray *blocks)
{
    struct effect2d_priv *s = node->priv_data;

    for (size_t i = 0; i < program->crafter_blocks.count; i++)
        if (ngli_darray_try_push(blocks, program->crafter_blocks.data[i]) < 0)
            return NGL_ERROR_MEMORY;

    const struct ngpu_pgcraft_block vert_crafter_block = {
        .name          = "vert",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .stage         = NGPU_PROGRAM_STAGE_VERT,
        .block         = &s->vert_block_desc,
    };
    if (ngli_darray_try_push(blocks, vert_crafter_block) < 0)
        return NGL_ERROR_MEMORY;

    const struct ngpu_pgcraft_block frag_crafter_block = {
        .name          = "frag",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .stage         = NGPU_PROGRAM_STAGE_FRAG,
        .block         = &s->frag_block_desc,
    };
    if (ngli_darray_try_push(blocks, frag_crafter_block) < 0)
        return NGL_ERROR_MEMORY;

    if (has_user_uniforms) {
        const struct ngpu_pgcraft_block user_crafter_block = {
            .name          = "user_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &program->user_block_desc,
        };
        if (ngli_darray_try_push(blocks, user_crafter_block) < 0)
            return NGL_ERROR_MEMORY;
    }

    return 0;
}

static int prepare_program(struct ngl_node *node, struct effect2d_program *program,
                           const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    const struct effect2d_opts *o = node->opts;

    const bool has_user_uniforms = program->user_field_indices.count > 0;

    /* Merge the blocks registered at init with the built-in ones */
    struct effect2d_block_darray blocks = {0};
    int ret = build_crafter_blocks(node, program, has_user_uniforms, &blocks);
    if (ret < 0) {
        ngli_darray_reset(&blocks);
        return ret;
    }

    /* Merge built-in texture with user textures */
    struct ngpu_pgcraft_texture src_tex = {
        .name  = "ngl_input",
        .type  = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
        .stage = NGPU_PROGRAM_STAGE_FRAG,
    };
    struct effect2d_texture_darray textures = {0};
    if (ngli_darray_try_push(&textures, src_tex) < 0) {
        ngli_darray_reset(&blocks);
        ngli_darray_reset(&textures);
        return NGL_ERROR_MEMORY;
    }
    const bool has_mask_image = s->mask_image && program == &s->programs.data[0];
    const int32_t nb_builtin_textures = has_mask_image ? 2 : 1;
    if (has_mask_image) {
        const struct ngpu_pgcraft_texture mask_tex = {
            .name  = "ngl_mask",
            .type  = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .stage = NGPU_PROGRAM_STAGE_FRAG,
        };
        if (ngli_darray_try_push(&textures, mask_tex) < 0) {
            ngli_darray_reset(&blocks);
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }
    for (size_t i = 0; i < program->crafter_textures.count; i++) {
        if (ngli_darray_try_push(&textures, program->crafter_textures.data[i]) < 0) {
            ngli_darray_reset(&blocks);
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "ngli_rect_uv",   .type = NGPU_TYPE_VEC2},
        {.name = "ngli_rect_px",   .type = NGPU_TYPE_VEC2},
        {.name = "ngli_tex_coord", .type = NGPU_TYPE_VEC2},
        {.name = "ngli_canvas_px", .type = NGPU_TYPE_VEC2},
    };

    struct bstr *vert = ngli_bstr_create();
    if (!vert) {
        ngli_darray_reset(&textures);
        ngli_darray_reset(&blocks);
        return NGL_ERROR_MEMORY;
    }
    ret = ngli_shader2d_write_vertex(&program->shader, vert, effect2d_composite_vert,
                                     vert_out_vars, NGLI_ARRAY_NB(vert_out_vars), &shader_role, 1);
    if (ret < 0) {
        ngli_bstr_freep(&vert);
        ngli_darray_reset(&textures);
        ngli_darray_reset(&blocks);
        return ret;
    }

    const char *frag_base = program->frag_glsl ? program->frag_glsl : effect2d_composite_frag;

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/effect2d",
        .vert_base        = ngli_bstr_strptr(vert),
        .frag_base        = frag_base,
        .textures         = textures.data,
        .nb_textures      = textures.count,
        .blocks           = blocks.data,
        .nb_blocks        = blocks.count,
        .vert_out_vars    = program->shader.varyings.data,
        .nb_vert_out_vars = program->shader.varyings.count,
    };

    program->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!program->crafter) {
        ngli_bstr_freep(&vert);
        ngli_darray_reset(&blocks);
        ngli_darray_reset(&textures);
        return NGL_ERROR_MEMORY;
    }

    ret = ngpu_pgcraft_craft(program->crafter, &crafter_params);
    ngli_bstr_freep(&vert);
    ngli_darray_reset(&blocks);
    ngli_darray_reset(&textures);
    if (ret < 0) {
        LOG(ERROR, "Effect2DShader %s: failed to compile GLSL source 3", program->source_label);
        return ret;
    }

    program->vert_block_index = ngpu_pgcraft_get_block_index(program->crafter, "vert", NGPU_PROGRAM_STAGE_VERT);
    program->frag_block_index = ngpu_pgcraft_get_block_index(program->crafter, "frag", NGPU_PROGRAM_STAGE_FRAG);
    if (has_user_uniforms)
        program->user_block_index = ngpu_pgcraft_get_block_index(program->crafter, "user_params", NGPU_PROGRAM_STAGE_FRAG);

    /* Apply blending preset */
    struct ngpu_graphics_state state = NGPU_GRAPHICS_STATE_DEFAULTS;
    ret = ngli_blend_mode_apply(&state, o->node2d.blend_mode);
    if (ret < 0)
        return ret;

    /* Create and init pipeline */
    program->pipeline = ngli_pipeline_create(gpu_ctx);
    if (!program->pipeline)
        return NGL_ERROR_MEMORY;

    const struct ngli_pipeline_params params = {
        .type = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .state        = state,
            .rt_layout    = *rendertarget_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(program->crafter),
        },
        .program          = ngpu_pgcraft_get_program(program->crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(program->crafter),
        .texture_infos    = ngpu_pgcraft_get_texture_infos(program->crafter),
    };

    ret = ngli_pipeline_init(program->pipeline, &params);
    if (ret < 0)
        return ret;

    ret = ngli_pipeline_set_image_source(program->pipeline, 0, s->input_image);
    if (ret < 0 && ret != NGL_ERROR_NOT_FOUND)
        return ret;
    if (has_mask_image) {
        ret = ngli_pipeline_set_image_source(program->pipeline, 1, s->mask_image);
        if (ret < 0 && ret != NGL_ERROR_NOT_FOUND)
            return ret;
    }
    for (size_t i = 0; i < program->crafter_textures.count; i++) {
        const char *name = program->crafter_textures.data[i].name;
        const struct ngl_node *res = program->resources ? ngli_hmap_get_str(program->resources, name) : NULL;
        if (!res) {
            LOG(ERROR, "no resource registered for texture '%s'", name);
            return NGL_ERROR_BUG;
        }
        const struct texture_info *info = ngli_node_texture_get_texture_info(res);
        ret = ngli_pipeline_set_image_source(program->pipeline, (int32_t)i + nb_builtin_textures, info->resource);
        if (ret < 0 && ret != NGL_ERROR_NOT_FOUND)
            return ret;
    }

    /* Register block sources */
    if (program->resources) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(program->resources, entry))) {
            const struct ngl_node *res = entry->data;
            if (res->cls->category != NGLI_NODE_CATEGORY_BLOCK)
                continue;
            const struct block_info *info = res->priv_data;
            const int32_t index = ngpu_pgcraft_get_block_index(program->crafter, entry->key.str, NGPU_PROGRAM_STAGE_FRAG);
            ret = ngli_pipeline_set_buffer_source(program->pipeline, index, info->resource, 0, NGPU_BUFFER_WHOLE_SIZE);
            if (ret < 0 && ret != NGL_ERROR_NOT_FOUND)
                return ret;
        }
    }

    return 0;
}

static int effect2d_prepare(struct ngl_node *node,
                            const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct effect2d_priv *s = node->priv_data;
    for (size_t i = 0; i < s->programs.count; i++) {
        int ret = prepare_program(node, &s->programs.data[i], rendertarget_layout);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int resize_rtt(struct rtt_ctx **rttp, struct image_resource *image, struct ngl_ctx *ctx,
                      uint32_t width, uint32_t height)
{
    if (*rttp) {
        uint32_t current_width, current_height;
        ngli_rtt_get_dimensions(*rttp, &current_width, &current_height);
        if (current_width == width && current_height == height)
            return 0;
    }

    struct rtt_ctx *rtt = ngli_rtt_create(ctx);
    if (!rtt)
        return NGL_ERROR_MEMORY;

    const struct ngpu_texture_params tex_params = {
        .type    = NGPU_TEXTURE_TYPE_2D,
        .format  = NGPU_FORMAT_R8G8B8A8_UNORM,
        .width   = width,
        .height  = height,
        .usage   = NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
                   NGPU_TEXTURE_USAGE_SAMPLED_BIT,
        .min_filter = NGPU_FILTER_LINEAR,
        .mag_filter = NGPU_FILTER_LINEAR,
        .wrap_s  = NGPU_WRAP_CLAMP_TO_EDGE,
        .wrap_t  = NGPU_WRAP_CLAMP_TO_EDGE,
    };
    int ret = ngli_rtt_from_texture_params(rtt, &tex_params);
    if (ret < 0)
        goto fail;

    struct ngli_mat4 coordinates;
    ngpu_ctx_get_rendertarget_uvcoord_matrix(ctx->gpu_ctx, coordinates.m);
    ngli_image_set_coordinates_matrix(ngli_rtt_get_image(rtt, 0), &coordinates);
    ngli_image_resource_set(image, NULL);
    ngli_rtt_freep(rttp);
    *rttp = rtt;

    return 0;

fail:
    ngli_rtt_freep(&rtt);
    return ret;
}

static void compute_bounds(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct effect2d_priv *s = node->priv_data;

    struct ngli_mat4 trs_matrix;
    ngli_node2d_compute_trs(node, trs_matrix.m);

    struct ngli_mat4 *modelview_matrix = &s->node2d_info.transform_matrix;
    ngli_mat4_mul(modelview_matrix->m, ctx->transform_2d_matrix.m, trs_matrix.m);

    s->node2d_info.screen_aabb = ngli_aabb_apply_transform(&s->node2d_info.aabb, modelview_matrix->m);
    s->node2d_info.effect_margin = ngli_node2d_scale_effect_margin(modelview_matrix, s->local_effect_margin);
}

static void effect2d_pre_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    const struct effect2d_opts *o = node->opts;

    static const struct ngli_mat4 id_matrix = {.m = NGLI_MAT4_IDENTITY};

    s->drawme = false;
    s->node2d_info.aabb = NGLI_AABB_EMPTY;
    s->node2d_info.transform_matrix = id_matrix;
    s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;

    if (!o->node2d.visible)
        return;

    /* Forward the pre-draw callback to the resources consumed by the effect2d composite pipeline */
    const int enabled = *(const int *)ngli_node_get_data_ptr(o->enabled_node, &o->enabled);
    const struct effect2d_program *active_program = &s->programs.data[enabled ? s->active_program_index : 0];
    if (active_program->resources) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(active_program->resources, entry)))
            ngli_node_pre_draw(entry->data);
    }

    /* Isolate transforms and opacity to measure children in the RTT local space */
    float children_effect_margin = 0.f;
    const struct ngli_mat4 prev_transform_2d = ctx->transform_2d_matrix;
    const float prev_opacity_2d = ctx->opacity_2d;
    ngli_node2d_apply_default_transform(ctx);

    for (size_t i = 0; i < o->children.count; i++)
        ngli_node_pre_draw(o->children.data[i]);
    for (size_t i = 0; i < o->mask_children.count; i++)
        ngli_node_pre_draw(o->mask_children.data[i]);

    /*
     * Compute or apply the bounding box and position of the composite quad.
     *
     * For Mask2D, map the mask to mask_rect and clip any child content outside
     * that rectangle.
     */
    int bounds = o->bounds;
    if (node->cls->id == NGL_NODE_MASK2D) {
        const float *mask_rect = ngli_node_get_data_ptr(o->rect_node, o->rect);
        bounds = mask_rect[2] > 0.f && mask_rect[3] > 0.f ? EFFECT2D_BOUNDS_RECT : EFFECT2D_BOUNDS_CHILDREN;
    }
    struct aabb children_bbox;
    if (bounds == EFFECT2D_BOUNDS_CANVAS) {
        const float width = NGLI_MAX(ctx->canvas_2d_width, 0.f);
        const float height = NGLI_MAX(ctx->canvas_2d_height, 0.f);
        children_bbox = (struct aabb) {
            .center = {width / 2.f, height / 2.f, 0.f, 1.f},
            .extent = {width / 2.f, height / 2.f},
        };
    } else if (bounds == EFFECT2D_BOUNDS_RECT) {
        const float *rect = ngli_node_get_data_ptr(o->rect_node, o->rect);
        const float half_w = NGLI_MAX(rect[2], 0.f) / 2.f;
        const float half_h = NGLI_MAX(rect[3], 0.f) / 2.f;
        const float center_x = rect[0] + half_w;
        const float center_y = rect[1] + half_h;
        if (!isfinite(rect[2]) || !isfinite(rect[3]) ||
            !isfinite(center_x) || !isfinite(center_y)) {
            children_bbox = NGLI_AABB_EMPTY;
        } else {
            children_bbox = (struct aabb) {
                .center = {center_x, center_y, 0.f, 1.f},
                .extent = {half_w, half_h},
            };
        }
    } else {
        children_bbox = ngli_node_compute_children_bounding_box(o->children.data, o->children.count);
        children_effect_margin = ngli_node_compute_children_effect_margin(o->children.data, o->children.count);
    }

    ctx->transform_2d_matrix = prev_transform_2d;
    ctx->opacity_2d = prev_opacity_2d;

    /* Skip rendering if children have no bounding box */
    if (children_bbox.extent[0] < 0.f || children_bbox.extent[1] < 0.f)
        return;

    NGLI_ALIGNED_VEC(bbox_min);
    NGLI_ALIGNED_VEC(bbox_max);
    ngli_aabb_get_min_max(&children_bbox, bbox_min, bbox_max);

    /* Sanitize the dilation value */
    if (!isfinite(o->dilation))
        return;
    const float d = NGLI_MAX(o->dilation, 0.f);

    s->local_effect_margin = children_effect_margin + d;
    float x0 = bbox_min[0] - d;
    float y0 = bbox_min[1] - d;
    float x1 = bbox_max[0] + d;
    float y1 = bbox_max[1] + d;

    /* Shader rect coordinates cover the full effect, even when cropped. */
    const float effect_rect[] = {x0, y0, x1 - x0, y1 - y0};
    memcpy(s->effect_rect, effect_rect, sizeof(s->effect_rect));

    /*
     * Crop the composite quad to the canvas plus the effect margin,
     * preserving a 1:1 texel mapping.
     */
    if (ctx->canvas_2d_width > 0.f && ctx->canvas_2d_height > 0.f) {
        const struct aabb canvas_aabb = {
            .center = {ctx->canvas_2d_width / 2.f, ctx->canvas_2d_height / 2.f, 0.f, 1.f},
            .extent = {ctx->canvas_2d_width / 2.f, ctx->canvas_2d_height / 2.f},
        };
        struct ngli_mat4 canvas_to_local;
        ngli_mat4_inverse(canvas_to_local.m, prev_transform_2d.m);
        struct aabb visible = ngli_aabb_apply_transform(&canvas_aabb, canvas_to_local.m);
        visible.extent[0] += s->local_effect_margin;
        visible.extent[1] += s->local_effect_margin;
        NGLI_ALIGNED_VEC(visible_min);
        NGLI_ALIGNED_VEC(visible_max);
        ngli_aabb_get_min_max(&visible, visible_min, visible_max);
        x0 = fmaxf(x0, visible_min[0]);
        y0 = fmaxf(y0, visible_min[1]);
        x1 = fminf(x1, visible_max[0]);
        y1 = fminf(y1, visible_max[1]);
    }

    if (x1 <= x0 || y1 <= y0)
        return;

    const float qw = x1 - x0;
    const float qh = y1 - y0;
    const float rect[] = {x0, y0, qw, qh};
    memcpy(s->rect, rect, sizeof(s->rect));

    /* The public bounds describe the complete composite quad. */
    s->node2d_info.aabb = children_bbox;
    s->node2d_info.aabb.extent[0] += d;
    s->node2d_info.aabb.extent[1] += d;

    /* Size internal rendertarget to the bbox scaled to viewport resolution */
    const float canvas_w = ctx->canvas_2d_width;
    const float canvas_h = ctx->canvas_2d_height;
    const float rt_w = (float)ctx->viewport.width;
    const float rt_h = (float)ctx->viewport.height;
    const float scale_x = canvas_w > 0.f ? rt_w / canvas_w : 1.f;
    const float scale_y = canvas_h > 0.f ? rt_h / canvas_h : 1.f;

    /* Compute and clamp final dimension to the device's max 2D texture dimension. */
    const struct ngpu_limits *limits = ngpu_ctx_get_limits(gpu_ctx);
    const uint32_t max_dim = limits->max_texture_dimension_2d;
    const double scaled_w = ceil((double)qw * (double)scale_x);
    const double scaled_h = ceil((double)qh * (double)scale_y);
    if (!isfinite(scaled_w) || !isfinite(scaled_h) || scaled_w <= 0.0 || scaled_h <= 0.0)
        return;

    const uint32_t w = scaled_w >= max_dim ? max_dim : (uint32_t)scaled_w;
    const uint32_t h = scaled_h >= max_dim ? max_dim : (uint32_t)scaled_h;

    int ret = resize_rtt(&s->rtt, s->input_image, ctx, w, h);
    if (ret < 0)
        return;
    if (s->mask_image) {
        ret = resize_rtt(&s->mask_rtt, s->mask_image, ctx, w, h);
        if (ret < 0)
            return;
    }

    /* Render the children into the RTT in the local 2D coordinates */
    ngli_node2d_apply_default_transform(ctx);

    const struct ngli_mat4 prev_projection_2d = ctx->projection_2d_matrix;

    ngli_rtt_begin(s->rtt);

    struct ngli_mat4 fbo_base_projection;
    ngpu_ctx_get_projection_matrix(gpu_ctx, fbo_base_projection.m);
    ngli_mat4_orthographic(ctx->projection_2d_matrix.m, x0, x1, y1, y0, -1.f, 1.f);
    ngli_mat4_mul(ctx->projection_2d_matrix.m, fbo_base_projection.m, ctx->projection_2d_matrix.m);

    for (size_t i = 0; i < o->children.count; i++) {
        ngli_node_draw(o->children.data[i]);
    }

    ngli_rtt_end(s->rtt);

    /* Render the mask children into the mask RTT in local 2D coordinates */
    if (s->mask_rtt) {
        ngli_node2d_apply_default_transform(ctx);
        ngli_rtt_begin(s->mask_rtt);
        for (size_t i = 0; i < o->mask_children.count; i++)
            ngli_node_draw(o->mask_children.data[i]);
        ngli_rtt_end(s->mask_rtt);
    }

    s->drawme = true;

    ctx->transform_2d_matrix = prev_transform_2d;
    ctx->opacity_2d = prev_opacity_2d;
    ctx->projection_2d_matrix = prev_projection_2d;

    compute_bounds(node);
}

static int effect2d_update(struct ngl_node *node, double t)
{
    struct effect2d_priv *s = node->priv_data;
    const struct effect2d_opts *o = node->opts;

    int ret = ngli_node_update_children(node, t);
    if (ret < 0)
        return ret;

    s->active_program_index = 0;
    for (size_t i = 0; i < o->shaders.count; i++) {
        const struct effect2d_shader_info info = ngli_effect2d_shader_get_info(o->shaders.data[i]);
        if (t >= info.start && (info.end < 0.0 || t < info.end)) {
            s->active_program_index = i + 1;
            break;
        }
    }
    return 0;
}

static void effect2d_draw(struct ngl_node *node)
{
    const struct pipeline_execution execution = {.staging = node->ctx->current_staging_buffer};

    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    const struct effect2d_opts *o = node->opts;

    if (!o->node2d.visible || !s->drawme) {
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    compute_bounds(node);

    const int enabled = *(const int *)ngli_node_get_data_ptr(o->enabled_node, &o->enabled);
    const size_t program_index = enabled ? s->active_program_index : 0;
    struct effect2d_program *program = &s->programs.data[program_index];
    struct ngli_pipeline *pl = program->pipeline;

    ngli_image_resource_set(s->input_image, s->rtt ? ngli_rtt_get_image(s->rtt, 0) : NULL);
    if (s->mask_image)
        ngli_image_resource_set(s->mask_image, s->mask_rtt ? ngli_rtt_get_image(s->mask_rtt, 0) : NULL);

    float tex_scale[2], tex_offset[2];
    const struct ngli_image *input = s->rtt ? ngli_rtt_get_image(s->rtt, 0) : NULL;
    ngli_image_get_coordinates_scale_offset(input, tex_scale, tex_offset);

    /* Compose full-effect UV -> cropped-input UV -> memory once for both stages. */
    for (size_t i = 0; i < 2; i++) {
        const float size = s->rect[2 + i];
        const float scale = size > 0.f ? s->effect_rect[2 + i] / size : 1.f;
        const float offset = size > 0.f ? (s->effect_rect[i] - s->rect[i]) / size : 0.f;
        tex_offset[i] += offset * tex_scale[i];
        tex_scale[i] *= scale;
    }

    /* Fill and push vertex block to staging buffer */
    {
        struct effect2d_vert_block vert_data = {0};
        vert_data.projection_matrix = ctx->projection_2d_matrix;
        vert_data.modelview_matrix = s->node2d_info.transform_matrix;
        memcpy(vert_data.rect, s->rect, sizeof(vert_data.rect));
        memcpy(vert_data.effect_rect, s->effect_rect, sizeof(vert_data.effect_rect));
        memcpy(vert_data.quad_tex_scale, tex_scale, sizeof(vert_data.quad_tex_scale));
        memcpy(vert_data.quad_tex_offset, tex_offset, sizeof(vert_data.quad_tex_offset));

        const size_t vert_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &vert_data, s->vert_block_size);
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_update_buffer(pl, program->vert_block_index, staging_buf, vert_offset, s->vert_block_size);
    }

    /* Fill and push fragment block to staging buffer */
    {
        const float group_opacity = ctx->opacity_2d;
        const float local_opacity = *(const float *)ngli_node_get_data_ptr(o->node2d.opacity_node, &o->node2d.opacity);

        struct effect2d_frag_block frag_data = {0};
        frag_data.opacity = local_opacity * group_opacity;
        memcpy(frag_data.rect_size, s->effect_rect + 2, sizeof(frag_data.rect_size));
        memcpy(frag_data.input_scale, tex_scale, sizeof(tex_scale));
        memcpy(frag_data.input_offset, tex_offset, sizeof(tex_offset));

        const size_t frag_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &frag_data, sizeof(frag_data));
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_update_buffer(pl, program->frag_block_index, staging_buf, frag_offset, sizeof(frag_data));
    }

    /* Fill and push user uniform block to staging buffer (if any) */
    if (program->user_block_index >= 0) {
        size_t offset = 0;
        uint8_t *data = ngpu_staging_buffer_reserve(ctx->current_staging_buffer, program->user_block_size, &offset);
        const struct ngpu_block_field *fields = program->user_block_desc.fields;

        const int32_t *field_indices = program->user_field_indices.data;
        for (size_t i = 0; i < program->user_field_indices.count; i++) {
            const struct variable_info *var = program->user_nodes.data[i]->priv_data;
            ngpu_block_field_copy(&fields[field_indices[i]], data + fields[field_indices[i]].offset, var->data);
        }

        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_update_buffer(pl, program->user_block_index, staging_buf, offset, program->user_block_size);
    }

    if (!ngpu_ctx_is_render_pass_active(gpu_ctx))
        ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);

    ngpu_ctx_set_viewport(gpu_ctx, &ctx->viewport);
    ngpu_ctx_set_scissor(gpu_ctx, &ctx->scissor);

    ngli_pipeline_draw(pl, &execution, 4, 1, 0);
}

static void effect2d_release(struct ngl_node *node)
{
    struct effect2d_priv *s = node->priv_data;
    for (size_t i = 0; i < s->programs.count; i++)
        ngli_pipeline_discard_resources(s->programs.data[i].pipeline);

    ngli_image_resource_set(s->input_image, NULL);
    ngli_rtt_freep(&s->rtt);
    if (s->mask_image)
        ngli_image_resource_set(s->mask_image, NULL);
    ngli_rtt_freep(&s->mask_rtt);
}

static void effect2d_unprepare(struct ngl_node *node)
{
    struct effect2d_priv *s = node->priv_data;

    for (size_t i = 0; i < s->programs.count; i++) {
        struct effect2d_program *program = &s->programs.data[i];
        ngli_pipeline_freep(&program->pipeline);
        ngpu_pgcraft_freep(&program->crafter);
    }
}

static void effect2d_uninit(struct ngl_node *node)
{
    struct effect2d_priv *s = node->priv_data;

    for (size_t i = 0; i < s->programs.count; i++)
        reset_program(&s->programs.data[i]);
    ngli_darray_reset(&s->programs);
    ngli_image_resource_unrefp(&s->input_image);
    ngli_image_resource_unrefp(&s->mask_image);

    ngpu_block_desc_reset(&s->vert_block_desc);
    ngpu_block_desc_reset(&s->frag_block_desc);
    ngli_hmap_freep(&s->mask_resources);
}

const struct node_class ngli_effect2d_class = {
    .id        = NGL_NODE_EFFECT2D,
    .name      = "Effect2D",
    .priv_size = sizeof(struct effect2d_priv),
    .init      = effect2d_init,
    .get_rendertarget_layout = effect2d_get_rendertarget_layout,
    .prepare   = effect2d_prepare,
    .unprepare = effect2d_unprepare,
    .update    = effect2d_update,
    .pre_draw  = effect2d_pre_draw,
    .draw      = effect2d_draw,
    .release   = effect2d_release,
    .uninit    = effect2d_uninit,
    .opts_size = sizeof(struct effect2d_opts),
    .node2d_offset = offsetof(struct effect2d_opts, node2d),
    .params    = effect2d_params,
    .flags     = NGLI_NODE_FLAG_2D,
    .file      = __FILE__,
};

const struct node_class ngli_layer2d_class = {
    .id        = NGL_NODE_LAYER2D,
    .name      = "Layer2D",
    .priv_size = sizeof(struct effect2d_priv),
    .init      = effect2d_init,
    .get_rendertarget_layout = effect2d_get_rendertarget_layout,
    .prepare   = effect2d_prepare,
    .unprepare = effect2d_unprepare,
    .update    = effect2d_update,
    .pre_draw  = effect2d_pre_draw,
    .draw      = effect2d_draw,
    .release   = effect2d_release,
    .uninit    = effect2d_uninit,
    .opts_size = sizeof(struct effect2d_opts),
    .node2d_offset = offsetof(struct effect2d_opts, node2d),
    .params    = layer2d_params,
    .flags     = NGLI_NODE_FLAG_2D,
    .file      = __FILE__,
};

const struct node_class ngli_mask2d_class = {
    .id        = NGL_NODE_MASK2D,
    .name      = "Mask2D",
    .priv_size = sizeof(struct effect2d_priv),
    .init      = effect2d_init,
    .get_rendertarget_layout = effect2d_get_rendertarget_layout,
    .prepare   = effect2d_prepare,
    .unprepare = effect2d_unprepare,
    .update    = effect2d_update,
    .pre_draw  = effect2d_pre_draw,
    .draw      = effect2d_draw,
    .release   = effect2d_release,
    .uninit    = effect2d_uninit,
    .opts_size = sizeof(struct effect2d_opts),
    .node2d_offset = offsetof(struct effect2d_opts, node2d),
    .params    = mask2d_params,
    .flags     = NGLI_NODE_FLAG_2D,
    .file      = __FILE__,
};
