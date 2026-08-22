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
#include "node2d.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "node_effect2d_shader.h"
#include "node_uniform.h"
#include "nopegl/nopegl.h"
#include "pipeline_compat.h"
#include "rtt.h"
#include "node_block.h"
#include "node_texture.h"
#include "utils/bstr.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/string.h"
#include "utils/utils.h"

/* GLSL fragments as string */
#include "effect2d_composite_frag.h"
#include "effect2d_composite_vert.h"

#include "effect2d_vert.h"


struct uniform_map {
    int32_t index;
    const void *data;
};

struct texture_map {
    const struct image *image;
};

struct block_map {
    int32_t index;
    const struct block_info *info;
    size_t buffer_rev;
};

NGLI_DECLARE_DARRAY_WITH_NAME(effect2d_texture_darray, struct ngpu_pgcraft_texture);
NGLI_DECLARE_DARRAY_WITH_NAME(effect2d_block_darray, struct ngpu_pgcraft_block);

struct effect2d_vert_block {
    struct ngli_mat4 projection_matrix;
    struct ngli_mat4 modelview_matrix;
    float rect[4];
};

struct effect2d_frag_block {
    float opacity;
    float _pad[3];
};

struct effect2d_opts {
    struct ngl_node **children;
    size_t nb_children;
    int bounds;
    struct ngl_node *rect_node;
    float rect[4];
    float dilation;
    struct ngli_node2d_opts node2d;
    struct ngl_node *enabled_node;
    int enabled;
    struct ngl_node **shaders;
    size_t nb_shaders;
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
    char *frag_glsl;
    struct ngpu_block_desc user_block_desc;
    size_t user_block_size;
    NGLI_DARRAY(int32_t) user_field_indices;
    NGLI_DARRAY(struct ngl_node *) user_nodes;
    struct effect2d_texture_darray crafter_textures;
    struct effect2d_block_darray crafter_blocks;
    int32_t vert_block_index;
    int32_t frag_block_index;
    int32_t user_block_index;
    NGLI_DARRAY(struct texture_map) textures_map;
    NGLI_DARRAY(struct block_map) blocks_map;
    struct ngpu_pgcraft *crafter;
    struct pipeline_compat *pipeline;
};

struct effect2d_priv {
    struct ngli_node2d_info node2d_info;

    struct rtt_ctx *rtt;
    struct ngpu_rendertarget_layout layout;
    uint32_t width;
    uint32_t height;
    float local_effect_margin;
    float rect[4];

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
        .image       = &texture_info->image,
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

    if (btype == NGPU_TYPE_UNIFORM_BUFFER)
        ngli_node_block_extend_usage(res, NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    else
        ngli_node_block_extend_usage(res, NGPU_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    const struct ngpu_buffer *buffer = block_info->buffer;
    const size_t buffer_size = buffer ? ngpu_buffer_get_size(buffer) : 0;
    struct ngpu_pgcraft_block crafter_block = {
        .type   = btype,
        .stage  = NGPU_PROGRAM_STAGE_FRAG,
        .block  = block,
        .buffer = {.buffer = buffer, .size = buffer_size},
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
    ngli_pipeline_compat_freep(&program->pipeline);
    ngpu_pgcraft_freep(&program->crafter);
    ngli_freep(&program->frag_glsl);
    ngli_darray_reset(&program->user_field_indices);
    ngli_darray_reset(&program->user_nodes);
    ngli_darray_reset(&program->crafter_textures);
    ngli_darray_reset(&program->crafter_blocks);
    ngpu_block_desc_reset(&program->user_block_desc);
    ngli_darray_reset(&program->textures_map);
    ngli_darray_reset(&program->blocks_map);
}

static int add_program(struct ngl_node *node, const char *glsl_header, const char *glsl_color,
                       struct hmap *resources, bool premult)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    struct effect2d_program program = {
        .resources = resources,
        .user_block_index = -1,
    };

    ngpu_block_desc_init(gpu_ctx, &program.user_block_desc, NGPU_BLOCK_LAYOUT_STD140);

    if (!ngli_str_is_empty(glsl_color)) {
        struct bstr *bstr = ngli_bstr_create();
        if (!bstr) {
            reset_program(&program);
            return NGL_ERROR_MEMORY;
        }

        if (!ngli_str_is_empty(glsl_header))
            ngli_bstr_printf(bstr, "%s\n", glsl_header);
        ngli_bstr_printf(bstr, "vec4 ngl_effect(vec2 uv, vec2 tex_coord) {\n%s\n}\n", glsl_color);
        ngli_bstr_printf(bstr, "void main() {\n");
        ngli_bstr_printf(bstr, "    vec4 color = ngl_effect(uv, tex_coord);\n");
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

    s->layout.nb_colors = 1;
    s->layout.colors[0].format = NGPU_FORMAT_R8G8B8A8_UNORM;

    const struct effect2d_opts *o = node->opts;

    /* Build vertex uniform block descriptor */
    ngpu_block_desc_init(gpu_ctx, &s->vert_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    static const struct ngpu_block_field vert_fields[] = {
        {.name = "projection_matrix", .type = NGPU_TYPE_MAT4},
        {.name = "modelview_matrix",  .type = NGPU_TYPE_MAT4},
        {.name = "rect",              .type = NGPU_TYPE_VEC4},
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
    };
    ret = ngpu_block_desc_add_fields(&s->frag_block_desc, frag_fields, NGLI_ARRAY_NB(frag_fields));
    if (ret < 0)
        return ret;
    s->frag_block_size = ngpu_block_desc_get_size(&s->frag_block_desc, 0);
    ngli_assert(s->frag_block_size == sizeof(struct effect2d_frag_block));

    /* Program 0 is always the passthrough used for gaps and the master bypass. */
    ret = add_program(node, NULL, NULL, NULL, false);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < o->nb_shaders; i++) {
        const struct effect2d_shader_info info = ngli_effect2d_shader_get_info(o->shaders[i]);
        ret = add_program(node, info.glsl_header, info.glsl_color, info.resources, info.premult);
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

static int prepare_program(struct ngl_node *node, struct effect2d_program *program,
                           const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    const struct effect2d_opts *o = node->opts;

    const bool has_user_uniforms = program->user_field_indices.count > 0;

    /* Register built-in vert/frag/user blocks for pgcraft */
    const struct ngpu_pgcraft_block vert_crafter_block = {
        .name          = "vert",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER,
        .stage         = NGPU_PROGRAM_STAGE_VERT,
        .block         = &s->vert_block_desc,
    };
    if (ngli_darray_try_push(&program->crafter_blocks, vert_crafter_block) < 0)
        return NGL_ERROR_MEMORY;

    const struct ngpu_pgcraft_block frag_crafter_block = {
        .name          = "frag",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER,
        .stage         = NGPU_PROGRAM_STAGE_FRAG,
        .block         = &s->frag_block_desc,
    };
    if (ngli_darray_try_push(&program->crafter_blocks, frag_crafter_block) < 0)
        return NGL_ERROR_MEMORY;

    if (has_user_uniforms) {
        const struct ngpu_pgcraft_block user_crafter_block = {
            .name          = "user_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &program->user_block_desc,
        };
        if (ngli_darray_try_push(&program->crafter_blocks, user_crafter_block) < 0)
            return NGL_ERROR_MEMORY;
    }

    /* Merge built-in texture with user textures */
    struct ngpu_pgcraft_texture src_tex = {
        .name  = "tex",
        .type  = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
        .stage = NGPU_PROGRAM_STAGE_FRAG,
    };
    struct effect2d_texture_darray textures = {0};
    if (ngli_darray_try_push(&textures, src_tex) < 0) {
        ngli_darray_reset(&textures);
        return NGL_ERROR_MEMORY;
    }
    for (size_t i = 0; i < program->crafter_textures.count; i++) {
        if (ngli_darray_try_push(&textures, program->crafter_textures.data[i]) < 0) {
            ngli_darray_reset(&textures);
            return NGL_ERROR_MEMORY;
        }
    }

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "uv",        .type = NGPU_TYPE_VEC2},
        {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
    };


    const char *frag_base = program->frag_glsl ? program->frag_glsl : effect2d_composite_frag;

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/effect2d",
        .vert_base        = effect2d_composite_vert,
        .frag_base        = frag_base,
        .textures         = textures.data,
        .nb_textures      = textures.count,
        .blocks           = program->crafter_blocks.data,
        .nb_blocks        = program->crafter_blocks.count,
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    program->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!program->crafter) {
        ngli_darray_reset(&textures);
        return NGL_ERROR_MEMORY;
    }

    int ret = ngpu_pgcraft_craft(program->crafter, &crafter_params);
    ngli_darray_reset(&textures);
    if (ret < 0)
        return ret;

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
    program->pipeline = ngli_pipeline_compat_create(gpu_ctx);
    if (!program->pipeline)
        return NGL_ERROR_MEMORY;

    const struct pipeline_compat_params params = {
        .type = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .state        = state,
            .rt_layout    = *rendertarget_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(program->crafter),
        },
        .program          = ngpu_pgcraft_get_program(program->crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(program->crafter),
        .resources        = ngpu_pgcraft_get_bindgroup_resources(program->crafter),
        .vertex_resources = ngpu_pgcraft_get_vertex_resources(program->crafter),
        .texture_infos    = ngpu_pgcraft_get_texture_infos(program->crafter),
    };

    ret = ngli_pipeline_compat_init(program->pipeline, &params);
    if (ret < 0)
        return ret;

    /* Build texture map */
    const struct ngpu_pgcraft_texture_infos texture_infos = ngpu_pgcraft_get_texture_infos(program->crafter);
    for (size_t i = 0; i < texture_infos.nb_infos; i++) {
        const struct texture_map tm = {.image = texture_infos.infos[i].image};
        if (ngli_darray_try_push(&program->textures_map, tm) < 0)
            return NGL_ERROR_MEMORY;
    }

    /* Build block map */
    if (program->resources) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(program->resources, entry))) {
            const struct ngl_node *res = entry->data;
            if (res->cls->category != NGLI_NODE_CATEGORY_BLOCK)
                continue;
            const struct block_info *info = res->priv_data;
            const struct block_map bm = {
                .index      = ngpu_pgcraft_get_block_index(program->crafter, entry->key.str, NGPU_PROGRAM_STAGE_FRAG),
                .info       = info,
                .buffer_rev = SIZE_MAX,
            };
            if (ngli_darray_try_push(&program->blocks_map, bm) < 0)
                return NGL_ERROR_MEMORY;
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

static int resize_rtt(struct effect2d_priv *s, struct ngl_ctx *ctx, uint32_t width, uint32_t height)
{
    if (s->width == width && s->height == height && s->rtt)
        return 0;

    ngli_rtt_freep(&s->rtt);

    s->rtt = ngli_rtt_create(ctx);
    if (!s->rtt)
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
    int ret = ngli_rtt_from_texture_params(s->rtt, &tex_params);
    if (ret < 0)
        return ret;

    struct image *image = ngli_rtt_get_image(s->rtt, 0);
    ngpu_ctx_get_rendertarget_uvcoord_matrix(ctx->gpu_ctx, image->coordinates_matrix.m);

    s->width = width;
    s->height = height;

    return 0;
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

    for (size_t i = 0; i < o->nb_children; i++)
        ngli_node_pre_draw(o->children[i]);

    /* Compute or apply the bounding box and position of the composite quad */
    struct aabb children_bbox;
    if (o->bounds == EFFECT2D_BOUNDS_CANVAS) {
        const float width = NGLI_MAX(ctx->canvas_2d_width, 0.f);
        const float height = NGLI_MAX(ctx->canvas_2d_height, 0.f);
        children_bbox = (struct aabb) {
            .center = {width / 2.f, height / 2.f, 0.f, 1.f},
            .extent = {width / 2.f, height / 2.f},
        };
    } else if (o->bounds == EFFECT2D_BOUNDS_RECT) {
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
        children_bbox = ngli_node_compute_children_bounding_box(o->children, o->nb_children);
        children_effect_margin = ngli_node_compute_children_effect_margin(o->children, o->nb_children);
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
    const float qx = bbox_min[0] - d;
    const float qy = bbox_min[1] - d;
    const float qw = bbox_max[0] - bbox_min[0] + 2.f * d;
    const float qh = bbox_max[1] - bbox_min[1] + 2.f * d;

    const float rect[] = {qx, qy, qw, qh};
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

    /*
     * Cap the RTT size to the visible canvas region extended by the dilation
     * margin plus the children effect margin. The ortho projection and quad
     * geometry still use the full bbox so children keep their correct
     * positions; only the texture resolution shrinks when the bbox exceeds the
     * canvas.
     */
    float rtt_qw = qw;
    float rtt_qh = qh;
    if (canvas_w > 0.f && canvas_h > 0.f) {
        rtt_qw = NGLI_MIN(qw, canvas_w + 2.f * s->local_effect_margin);
        rtt_qh = NGLI_MIN(qh, canvas_h + 2.f * s->local_effect_margin);
    }

    /* Compute and clamp final dimension to the device's max 2D texture dimension. */
    const struct ngpu_limits *limits = ngpu_ctx_get_limits(gpu_ctx);
    const uint32_t max_dim = limits->max_texture_dimension_2d;
    const double scaled_w = ceil((double)rtt_qw * (double)scale_x);
    const double scaled_h = ceil((double)rtt_qh * (double)scale_y);
    if (!isfinite(scaled_w) || !isfinite(scaled_h) || scaled_w <= 0.0 || scaled_h <= 0.0)
        return;

    const uint32_t w = scaled_w >= max_dim ? max_dim : (uint32_t)scaled_w;
    const uint32_t h = scaled_h >= max_dim ? max_dim : (uint32_t)scaled_h;

    int ret = resize_rtt(s, ctx, w, h);
    if (ret < 0)
        return;

    /* Render children into the RTT in the same isolated 2D space */
    ngli_node2d_apply_default_transform(ctx);

    const struct ngli_mat4 prev_projection_2d = ctx->projection_2d_matrix;

    ngli_rtt_begin(s->rtt);

    struct ngli_mat4 fbo_base_projection;
    ngpu_ctx_get_projection_matrix(gpu_ctx, fbo_base_projection.m);
    ngli_mat4_orthographic(ctx->projection_2d_matrix.m, qx - 0.5f, qx + qw - 0.5f, qy + qh - 0.5f, qy - 0.5f, -1.f, 1.f);
    ngli_mat4_mul(ctx->projection_2d_matrix.m, fbo_base_projection.m, ctx->projection_2d_matrix.m);

    for (size_t i = 0; i < o->nb_children; i++) {
        ngli_node_draw(o->children[i]);
    }

    ngli_rtt_end(s->rtt);

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
    for (size_t i = 0; i < o->nb_shaders; i++) {
        const struct effect2d_shader_info info = ngli_effect2d_shader_get_info(o->shaders[i]);
        if (t >= info.start && (info.end < 0.0 || t < info.end)) {
            s->active_program_index = i + 1;
            break;
        }
    }
    return 0;
}

static void effect2d_draw(struct ngl_node *node)
{
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
    struct pipeline_compat *pl = program->pipeline;

    if (program->textures_map.count > 0 && s->rtt)
        program->textures_map.data[0].image = ngli_rtt_get_image(s->rtt, 0);

    /* Update textures */
    for (size_t i = 0; i < program->textures_map.count; i++)
        ngli_pipeline_compat_update_image(pl, (int32_t)i, program->textures_map.data[i].image, ctx->current_staging_buffer);

    /* Fill and push vertex block to staging buffer */
    {
        struct effect2d_vert_block vert_data = {0};
        vert_data.projection_matrix = ctx->projection_2d_matrix;
        vert_data.modelview_matrix = s->node2d_info.transform_matrix;
        memcpy(vert_data.rect, s->rect, sizeof(vert_data.rect));

        const size_t vert_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &vert_data, s->vert_block_size);
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl, program->vert_block_index, staging_buf, vert_offset, s->vert_block_size);
    }

    /* Fill and push fragment block to staging buffer */
    {
        const float group_opacity = ctx->opacity_2d;
        const float local_opacity = *(const float *)ngli_node_get_data_ptr(o->node2d.opacity_node, &o->node2d.opacity);

        struct effect2d_frag_block frag_data = {0};
        frag_data.opacity = local_opacity * group_opacity;

        const size_t frag_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &frag_data, sizeof(frag_data));
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl, program->frag_block_index, staging_buf, frag_offset, sizeof(frag_data));
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
        ngli_pipeline_compat_update_buffer(pl, program->user_block_index, staging_buf, offset, program->user_block_size);
    }

    /* Update blocks */
    struct block_map *block_maps = program->blocks_map.data;
    for (size_t i = 0; i < program->blocks_map.count; i++) {
        const struct block_info *info = block_maps[i].info;
        if (block_maps[i].buffer_rev != info->buffer_rev) {
            ngli_pipeline_compat_update_buffer(pl, block_maps[i].index, info->buffer, 0, 0);
            block_maps[i].buffer_rev = info->buffer_rev;
        }
    }

    if (!ngpu_ctx_is_render_pass_active(gpu_ctx))
        ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);

    ngpu_ctx_set_viewport(gpu_ctx, &ctx->viewport);
    ngpu_ctx_set_scissor(gpu_ctx, &ctx->scissor);

    ngli_pipeline_compat_draw(pl, 4, 1, 0);
}

static void effect2d_release(struct ngl_node *node)
{
    struct effect2d_priv *s = node->priv_data;

    ngli_rtt_freep(&s->rtt);
    s->width = 0;
    s->height = 0;
}

static void effect2d_uninit(struct ngl_node *node)
{
    struct effect2d_priv *s = node->priv_data;

    for (size_t i = 0; i < s->programs.count; i++)
        reset_program(&s->programs.data[i]);
    ngli_darray_reset(&s->programs);

    ngpu_block_desc_reset(&s->vert_block_desc);
    ngpu_block_desc_reset(&s->frag_block_desc);
}

const struct node_class ngli_effect2d_class = {
    .id        = NGL_NODE_EFFECT2D,
    .name      = "Effect2D",
    .priv_size = sizeof(struct effect2d_priv),
    .init      = effect2d_init,
    .get_rendertarget_layout = effect2d_get_rendertarget_layout,
    .prepare   = effect2d_prepare,
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
