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
#include "image.h"
#include "internal.h"
#include "node2d.h"
#include "log.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "node_uniform.h"
#include "nopegl/nopegl.h"
#include "pipeline_compat.h"
#include "rtt.h"
#include "node_block.h"
#include "node_texture.h"
#include "utils/bstr.h"
#include "utils/darray.h"
#include "utils/memory.h"
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

struct effect2d_vert_block {
    NGLI_ALIGNED_MAT(projection_matrix);
    NGLI_ALIGNED_MAT(modelview_matrix);
};

struct effect2d_frag_block {
    float opacity;
    float _pad[3];
};

struct effect2d_opts {
    struct ngl_node **children;
    size_t nb_children;
    const char *glsl_header;
    const char *glsl_color;
    struct ngl_node **resources;
    size_t nb_resources;
    float dilation;
    struct ngli_node2d_opts node2d;
};

struct effect2d_priv {
    struct ngli_node2d_info node2d_info;

    struct rtt_ctx *rtt;
    struct ngpu_rendertarget_layout layout;
    uint32_t width;
    uint32_t height;
    struct aabb children_bbox;

    /* User resources */
    char *frag_glsl;

    /* Built-in uniform blocks */
    struct ngpu_block_desc vert_block_desc;
    size_t vert_block_size;
    struct ngpu_block_desc frag_block_desc;
    size_t frag_block_size;

    /* User uniform block */
    struct ngpu_block_desc user_block_desc;
    size_t user_block_size;
    struct darray user_field_indices; // array of int32_t
    struct darray user_nodes;         // array of struct ngl_node *

    /* Crafter inputs */
    struct darray crafter_textures; // array of struct ngpu_pgcraft_texture
    struct darray crafter_blocks;   // array of struct ngpu_pgcraft_block
    struct ngpu_pgcraft_attribute position_attr;
    struct ngpu_pgcraft_attribute uvcoord_attr;

    /* Pipeline state */
    int32_t vert_block_index;
    int32_t frag_block_index;
    int32_t user_block_index;
    struct darray textures_map;   // array of struct texture_map
    struct darray blocks_map;     // array of struct block_map
    struct geometry *geometry;
    struct ngpu_pgcraft *crafter;
    struct pipeline_compat *pipeline;
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
        .key       = "glsl_header",
        .type      = NGLI_PARAM_TYPE_STR,
        .offset    = OFFSET(glsl_header),
        .desc      = NGLI_DOCSTRING("optional GLSL helper functions prepended before the fragment body"),
    }, {
        .key       = "glsl_color",
        .type      = NGLI_PARAM_TYPE_STR,
        .offset    = OFFSET(glsl_color),
        .desc      = NGLI_DOCSTRING("custom fragment shader body; receives uv (vec2), must return vec4"),
    }, {
        .key        = "resources",
        .type       = NGLI_PARAM_TYPE_NODELIST,
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
            NGL_NODE_TEXTUREVIEW,
            NGL_NODE_CUSTOMTEXTURE,
            NGL_NODE_BLOCK,
            NGLI_NODE_NONE,
        },
        .desc = NGLI_DOCSTRING("uniform and texture nodes available to fragment; "
                               "each node's label is used as the GLSL name"),
    }, {
        .key       = "dilation",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(dilation),
        .desc      = NGLI_DOCSTRING("geometry dilation in pixels to accommodate effects that bleed beyond children bounds"),
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
        .key       = "blending",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(node2d.blending),
        .def_value = {.i32=NGLI_BLENDING_SRC_OVER},
        .choices   = &ngli_blending_choices,
        .desc      = NGLI_DOCSTRING("blending mode for the final composite"),
    },
    {NULL}
};

static const float quad_vertices[] = {
   -1.f,-1.f, 0.f,
    1.f,-1.f, 0.f,
   -1.f, 1.f, 0.f,
    1.f, 1.f, 0.f,
};

static const float quad_uvcoords[] = {
    0.f, 1.f,
    1.f, 1.f,
    0.f, 0.f,
    1.f, 0.f,
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

static int register_uniform(struct ngl_node *res, struct effect2d_priv *s)
{
    const struct variable_info *var = res->priv_data;
    const int field_idx = ngpu_block_desc_add_field(&s->user_block_desc, res->label, var->data_type, 0);
    if (field_idx < 0)
        return field_idx;
    if (!ngli_darray_push(&s->user_field_indices, &field_idx))
        return NGL_ERROR_MEMORY;
    if (!ngli_darray_push(&s->user_nodes, &res))
        return NGL_ERROR_MEMORY;
    return 0;
}

static int register_texture(struct ngl_node *res, struct darray *textures)
{
    struct texture_info *texture_info = res->priv_data;
    struct ngpu_pgcraft_texture tex = {
        .type        = ngli_node_texture_get_pgcraft_texture_type(res),
        .stage       = NGPU_PROGRAM_STAGE_FRAG,
        .image       = &texture_info->image,
        .format      = texture_info->params.format,
        .clamp_video = texture_info->clamp_video,
        .premult     = texture_info->premult,
    };
    snprintf(tex.name, sizeof(tex.name), "%s", res->label);
    return ngli_darray_push(textures, &tex) ? 0 : NGL_ERROR_MEMORY;
}

static int register_block(struct ngl_node *res, struct ngpu_ctx *gpu_ctx, struct darray *blocks)
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
    snprintf(crafter_block.name, sizeof(crafter_block.name), "%s", res->label);
    return ngli_darray_push(blocks, &crafter_block) ? 0 : NGL_ERROR_MEMORY;
}

static int register_resource(struct ngl_node *res, struct ngpu_ctx *gpu_ctx,
                             struct effect2d_priv *s, struct darray *textures, struct darray *blocks)
{
    if (node_is_texture(res))
        return register_texture(res, textures);
    if (res->cls->id == NGL_NODE_BLOCK)
        return register_block(res, gpu_ctx, blocks);
    return register_uniform(res, s);
}

static int register_resources(struct ngl_node *node, struct ngpu_ctx *gpu_ctx,
                              struct effect2d_priv *s, struct darray *textures, struct darray *blocks)
{
    const struct effect2d_opts *o = node->opts;
    for (size_t i = 0; i < o->nb_resources; i++) {
        int ret = register_resource(o->resources[i], gpu_ctx, s, textures, blocks);
        if (ret < 0)
            return ret;
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

    ngli_darray_init(&s->user_field_indices, sizeof(int32_t), 0);
    ngli_darray_init(&s->user_nodes, sizeof(struct ngl_node *), 0);
    s->user_block_index = -1;

    /* Build the composite fragment shader */
    if (o->glsl_color && o->glsl_color[0]) {
        /* Validate resources */
        for (size_t i = 0; i < o->nb_resources; i++) {
            const struct ngl_node *res = o->resources[i];
            if (!res->label || !res->label[0]) {
                LOG(ERROR, "resources[%zu]: node label is required as GLSL name", i);
                return NGL_ERROR_INVALID_USAGE;
            }
        }

        struct bstr *bstr = ngli_bstr_create();
        if (!bstr)
            return NGL_ERROR_MEMORY;

        if (o->glsl_header && o->glsl_header[0])
            ngli_bstr_printf(bstr, "%s\n", o->glsl_header);
        ngli_bstr_printf(bstr, "vec4 ngl_effect(vec2 uv) {\n%s\n}\n", o->glsl_color);
        ngli_bstr_printf(bstr, "void main() {\n");
        ngli_bstr_printf(bstr, "    ngl_out_color = ngl_effect(tex_coord) * opacity;\n");
        ngli_bstr_printf(bstr, "}\n");

        s->frag_glsl = ngli_bstr_strdup(bstr);
        ngli_bstr_freep(&bstr);
        if (!s->frag_glsl)
            return NGL_ERROR_MEMORY;
    }

    /* Build vertex uniform block descriptor */
    ngpu_block_desc_init(gpu_ctx, &s->vert_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    static const struct ngpu_block_field vert_fields[] = {
        {.name = "projection_matrix", .type = NGPU_TYPE_MAT4},
        {.name = "modelview_matrix",  .type = NGPU_TYPE_MAT4},
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

    /* Build user uniform block descriptor */
    ngpu_block_desc_init(gpu_ctx, &s->user_block_desc, NGPU_BLOCK_LAYOUT_STD140);

    /* Parse user resources into user uniforms (block_desc fields), textures, and blocks */
    ngli_darray_init(&s->crafter_textures, sizeof(struct ngpu_pgcraft_texture), 0);
    ngli_darray_init(&s->crafter_blocks, sizeof(struct ngpu_pgcraft_block), 0);

    ret = register_resources(node, gpu_ctx, s, &s->crafter_textures, &s->crafter_blocks);
    if (ret < 0)
        return ret;

    if (ngli_darray_count(&s->user_field_indices) > 0)
        s->user_block_size = ngpu_block_desc_get_size(&s->user_block_desc, 0);

    /* Create composite quad geometry */
    s->geometry = ngli_geometry_create(gpu_ctx);
    if (!s->geometry)
        return NGL_ERROR_MEMORY;

    if ((ret = ngli_geometry_set_vertices(s->geometry, 4, quad_vertices)) < 0 ||
        (ret = ngli_geometry_set_uvcoords(s->geometry, 4, quad_uvcoords)) < 0 ||
        (ret = ngli_geometry_init(s->geometry, NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)) < 0)
        return ret;

    /* Store attributes for prepare */
    snprintf(s->position_attr.name, sizeof(s->position_attr.name), "position");
    s->position_attr.type   = NGPU_TYPE_VEC3;
    s->position_attr.format = NGPU_FORMAT_R32G32B32_SFLOAT;
    s->position_attr.stride = s->geometry->vertices_layout.stride;
    s->position_attr.offset = s->geometry->vertices_layout.offset;
    s->position_attr.buffer = s->geometry->vertices_buffer;

    snprintf(s->uvcoord_attr.name, sizeof(s->uvcoord_attr.name), "uvcoord");
    s->uvcoord_attr.type   = NGPU_TYPE_VEC2;
    s->uvcoord_attr.format = NGPU_FORMAT_R32G32_SFLOAT;
    s->uvcoord_attr.stride = s->geometry->uvcoords_layout.stride;
    s->uvcoord_attr.offset = s->geometry->uvcoords_layout.offset;
    s->uvcoord_attr.buffer = s->geometry->uvcoords_buffer;

    return 0;
}

static void effect2d_get_child_render_state(const struct ngl_node *node,
                                             const struct ngpu_graphics_state *parent_gs,
                                             const struct ngpu_rendertarget_layout *parent_rtl,
                                             struct ngpu_graphics_state *child_gs,
                                             struct ngpu_rendertarget_layout *child_rtl)
{
    const struct effect2d_priv *s = node->priv_data;
    *child_gs = *parent_gs;
    *child_rtl = s->layout;
}

static int effect2d_prepare(struct ngl_node *node,
                            const struct ngpu_graphics_state *graphics_state,
                            const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    const struct effect2d_opts *o = node->opts;

    ngli_darray_init(&s->textures_map, sizeof(struct texture_map), 0);
    ngli_darray_init(&s->blocks_map, sizeof(struct block_map), 0);

    const bool has_user_uniforms = ngli_darray_count(&s->user_field_indices) > 0;

    /* Register built-in vert/frag/user blocks for pgcraft */
    const struct ngpu_pgcraft_block vert_crafter_block = {
        .name          = "vert",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER,
        .stage         = NGPU_PROGRAM_STAGE_VERT,
        .block         = &s->vert_block_desc,
    };
    if (!ngli_darray_push(&s->crafter_blocks, &vert_crafter_block))
        return NGL_ERROR_MEMORY;

    const struct ngpu_pgcraft_block frag_crafter_block = {
        .name          = "frag",
        .instance_name = "",
        .type          = NGPU_TYPE_UNIFORM_BUFFER,
        .stage         = NGPU_PROGRAM_STAGE_FRAG,
        .block         = &s->frag_block_desc,
    };
    if (!ngli_darray_push(&s->crafter_blocks, &frag_crafter_block))
        return NGL_ERROR_MEMORY;

    if (has_user_uniforms) {
        const struct ngpu_pgcraft_block user_crafter_block = {
            .name          = "user_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &s->user_block_desc,
        };
        if (!ngli_darray_push(&s->crafter_blocks, &user_crafter_block))
            return NGL_ERROR_MEMORY;
    }

    /* Merge built-in texture with user textures */
    struct ngpu_pgcraft_texture src_tex = {
        .name  = "tex",
        .type  = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
        .stage = NGPU_PROGRAM_STAGE_FRAG,
    };
    struct darray textures;
    ngli_darray_init(&textures, sizeof(struct ngpu_pgcraft_texture), 0);
    if (!ngli_darray_push(&textures, &src_tex)) {
        ngli_darray_reset(&textures);
        return NGL_ERROR_MEMORY;
    }
    const struct ngpu_pgcraft_texture *user_t = ngli_darray_data(&s->crafter_textures);
    for (size_t i = 0; i < ngli_darray_count(&s->crafter_textures); i++) {
        if (!ngli_darray_push(&textures, &user_t[i])) {
            ngli_darray_reset(&textures);
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

    const char *frag_base = s->frag_glsl ? s->frag_glsl : effect2d_composite_frag;

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/effect2d",
        .vert_base        = effect2d_composite_vert,
        .frag_base        = frag_base,
        .textures         = ngli_darray_data(&textures),
        .nb_textures      = ngli_darray_count(&textures),
        .blocks           = ngli_darray_data(&s->crafter_blocks),
        .nb_blocks        = ngli_darray_count(&s->crafter_blocks),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter) {
        ngli_darray_reset(&textures);
        return NGL_ERROR_MEMORY;
    }

    int ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    ngli_darray_reset(&textures);
    if (ret < 0)
        return ret;

    s->vert_block_index = ngpu_pgcraft_get_block_index(s->crafter, "vert", NGPU_PROGRAM_STAGE_VERT);
    s->frag_block_index = ngpu_pgcraft_get_block_index(s->crafter, "frag", NGPU_PROGRAM_STAGE_FRAG);
    if (has_user_uniforms)
        s->user_block_index = ngpu_pgcraft_get_block_index(s->crafter, "user_params", NGPU_PROGRAM_STAGE_FRAG);

    /* Apply blending preset */
    struct ngpu_graphics_state state = *graphics_state;
    ret = ngli_blending_apply_preset(&state, o->node2d.blending);
    if (ret < 0)
        return ret;

    /* Create and init pipeline */
    s->pipeline = ngli_pipeline_compat_create(gpu_ctx);
    if (!s->pipeline)
        return NGL_ERROR_MEMORY;

    const struct pipeline_compat_params params = {
        .type = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
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

    ret = ngli_pipeline_compat_init(s->pipeline, &params);
    if (ret < 0)
        return ret;

    /* Build texture map */
    const struct ngpu_pgcraft_texture_infos texture_infos = ngpu_pgcraft_get_texture_infos(s->crafter);
    for (size_t i = 0; i < texture_infos.nb_infos; i++) {
        const struct texture_map tm = {.image = texture_infos.infos[i].image};
        if (!ngli_darray_push(&s->textures_map, &tm))
            return NGL_ERROR_MEMORY;
    }

    /* Build block map */
    for (size_t i = 0; i < o->nb_resources; i++) {
        const struct ngl_node *res = o->resources[i];
        if (res->cls->category != NGLI_NODE_CATEGORY_BLOCK)
            continue;
        const struct block_info *info = res->priv_data;
        const struct block_map bm = {
            .index      = ngpu_pgcraft_get_block_index(s->crafter, res->label, NGPU_PROGRAM_STAGE_FRAG),
            .info       = info,
            .buffer_rev = SIZE_MAX,
        };
        if (!ngli_darray_push(&s->blocks_map, &bm))
            return NGL_ERROR_MEMORY;
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
    ngpu_ctx_get_rendertarget_uvcoord_matrix(ctx->gpu_ctx, image->coordinates_matrix);

    s->width = width;
    s->height = height;

    return 0;
}

static void effect2d_pre_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    const struct effect2d_opts *o = node->opts;

    if (!o->node2d.visible)
        return;

    /* Pre-draw children (computes children bounding boxes) */
    for (size_t i = 0; i < o->nb_children; i++)
        ngli_node_pre_draw(o->children[i]);

    /* Compute bounding box and position of the composite quad */
    s->children_bbox = ngli_node_compute_children_bounding_box(o->children, o->nb_children);
    s->node2d_info.aabb = s->children_bbox;

    /* Skip rendering if children have no bounding box */
    if (s->children_bbox.extent[0] < 0.f)
        return;

    NGLI_ALIGNED_VEC(bbox_min);
    NGLI_ALIGNED_VEC(bbox_max);
    ngli_aabb_get_min_max(&s->children_bbox, bbox_min, bbox_max);

    const float d = o->dilation;
    const float qx = bbox_min[0] - d;
    const float qy = bbox_min[1] - d;
    const float qw = bbox_max[0] - bbox_min[0] + 2.f * d;
    const float qh = bbox_max[1] - bbox_min[1] + 2.f * d;

    /* Size internal rendertarget to the bbox scaled to viewport resolution */
    const float canvas_w = ctx->canvas_2d_width;
    const float canvas_h = ctx->canvas_2d_height;
    const float rt_w = (float)ngpu_rendertarget_get_width(ctx->current_rendertarget);
    const float rt_h = (float)ngpu_rendertarget_get_height(ctx->current_rendertarget);
    const float scale_x = canvas_w > 0.f ? rt_w / canvas_w : 1.f;
    const float scale_y = canvas_h > 0.f ? rt_h / canvas_h : 1.f;
    const uint32_t w = (uint32_t)ceilf(qw * scale_x);
    const uint32_t h = (uint32_t)ceilf(qh * scale_y);
    if (w == 0 || h == 0)
        return;
    int ret = resize_rtt(s, ctx, w, h);
    if (ret < 0)
        return;

    struct texture_map *textures_map = ngli_darray_data(&s->textures_map);
    if (ngli_darray_count(&s->textures_map) > 0) {
        textures_map[0].image = ngli_rtt_get_image(s->rtt, 0);
    }

    const float vertices[] = {
        qx,      qy,      0.f,
        qx + qw, qy,      0.f,
        qx,      qy + qh, 0.f,
        qx + qw, qy + qh, 0.f,
    };
    ngpu_buffer_upload(s->geometry->vertices_buffer, vertices, 0, sizeof(vertices));

    static const float uvcoords[] = {
        0.f, 1.f,
        1.f, 1.f,
        0.f, 0.f,
        1.f, 0.f,
    };
    ngpu_buffer_upload(s->geometry->uvcoords_buffer, uvcoords, 0, sizeof(uvcoords));

    /* Manage transform stack and render children */
    NGLI_ALIGNED_MAT(prev_projection_2d);
    memcpy(prev_projection_2d, ctx->projection_2d_matrix, sizeof(prev_projection_2d));
    struct darray prev_transform_2d_stack = ctx->transform_2d_stack;
    struct darray prev_opacity_2d_stack = ctx->opacity_2d_stack;

    ngli_darray_init(&ctx->transform_2d_stack, 4 * 4 * sizeof(float), NGLI_DARRAY_FLAG_ALIGNED);
    ngli_darray_init(&ctx->opacity_2d_stack, sizeof(float), 0);

    static const NGLI_ALIGNED_MAT(id_matrix) = NGLI_MAT4_IDENTITY;
    const float default_opacity = 1.f;
    if (!ngli_darray_push(&ctx->transform_2d_stack, id_matrix) ||
        !ngli_darray_push(&ctx->opacity_2d_stack, &default_opacity))
        goto restore_2d_state;

    ngli_rtt_begin(s->rtt);

    NGLI_ALIGNED_MAT(fbo_base_projection);
    ngpu_ctx_get_projection_matrix(gpu_ctx, fbo_base_projection);
    ngli_mat4_orthographic(ctx->projection_2d_matrix, qx - 0.5f, qx + qw - 0.5f, qy + qh - 0.5f, qy - 0.5f, -1.f, 1.f);
    ngli_mat4_mul(ctx->projection_2d_matrix, fbo_base_projection, ctx->projection_2d_matrix);

    for (size_t i = 0; i < o->nb_children; i++) {
        ngli_node_draw(o->children[i]);
    }

    ngli_rtt_end(s->rtt);

restore_2d_state:
    ngli_darray_reset(&ctx->transform_2d_stack);
    ngli_darray_reset(&ctx->opacity_2d_stack);
    ctx->transform_2d_stack = prev_transform_2d_stack;
    ctx->opacity_2d_stack = prev_opacity_2d_stack;
    memcpy(ctx->projection_2d_matrix, prev_projection_2d, sizeof(prev_projection_2d));

}

static void effect2d_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    const struct effect2d_opts *o = node->opts;

    if (!o->node2d.visible) {
        s->node2d_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    NGLI_ALIGNED_MAT(trs_matrix);
    ngli_node2d_compute_trs(node, trs_matrix);

    NGLI_ALIGNED_MAT(modelview_matrix);
    const float *prev_matrix = ngli_darray_tail(&ctx->transform_2d_stack);
    ngli_mat4_mul(modelview_matrix, prev_matrix, trs_matrix);

    struct pipeline_compat *pl = s->pipeline;

    /* Update textures */
    struct texture_map *textures_map = ngli_darray_data(&s->textures_map);
    for (size_t i = 0; i < ngli_darray_count(&s->textures_map); i++)
        ngli_pipeline_compat_update_image(pl, (int32_t)i, textures_map[i].image, ctx->current_staging_buffer);

    /* Fill and push vertex block to staging buffer */
    {
        struct effect2d_vert_block vert_data = {0};
        memcpy(vert_data.projection_matrix, ctx->projection_2d_matrix, sizeof(vert_data.projection_matrix));
        memcpy(vert_data.modelview_matrix, modelview_matrix, sizeof(vert_data.modelview_matrix));

        const size_t vert_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &vert_data, s->vert_block_size);
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl, s->vert_block_index, staging_buf, vert_offset, s->vert_block_size);
    }

    /* Fill and push fragment block to staging buffer */
    {
        const float *group_opacity = ngli_darray_tail(&ctx->opacity_2d_stack);
        const float local_opacity = *(const float *)ngli_node_get_data_ptr(o->node2d.opacity_node, &o->node2d.opacity);

        struct effect2d_frag_block frag_data = {0};
        frag_data.opacity = local_opacity * *group_opacity;

        const size_t frag_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &frag_data, sizeof(frag_data));
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl, s->frag_block_index, staging_buf, frag_offset, sizeof(frag_data));
    }

    /* Fill and push user uniform block to staging buffer (if any) */
    if (s->user_block_index >= 0) {
        size_t offset = 0;
        uint8_t *data = ngpu_staging_buffer_reserve(ctx->current_staging_buffer, s->user_block_size, &offset);
        const struct ngpu_block_field *fields = s->user_block_desc.fields;

        const int32_t *field_indices = ngli_darray_data(&s->user_field_indices);
        const struct ngl_node **user_nodes = ngli_darray_data(&s->user_nodes);
        for (size_t i = 0; i < ngli_darray_count(&s->user_field_indices); i++) {
            const struct variable_info *var = user_nodes[i]->priv_data;
            ngpu_block_field_copy(&fields[field_indices[i]], data + fields[field_indices[i]].offset, var->data);
        }

        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl, s->user_block_index, staging_buf, offset, s->user_block_size);
    }

    /* Update blocks */
    struct block_map *block_maps = ngli_darray_data(&s->blocks_map);
    for (size_t i = 0; i < ngli_darray_count(&s->blocks_map); i++) {
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

    /* Set AABB for parent nodes */
    struct ngli_node2d_info *node2d_info = &s->node2d_info;
    memcpy(node2d_info->transform_matrix, modelview_matrix, sizeof(node2d_info->transform_matrix));
    node2d_info->screen_aabb = s->children_bbox;
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

    ngli_pipeline_compat_freep(&s->pipeline);
    ngpu_pgcraft_freep(&s->crafter);
    ngli_geometry_freep(&s->geometry);

    ngli_freep(&s->frag_glsl);
    ngli_darray_reset(&s->user_field_indices);
    ngli_darray_reset(&s->user_nodes);
    ngli_darray_reset(&s->crafter_textures);
    ngli_darray_reset(&s->crafter_blocks);
    ngpu_block_desc_reset(&s->vert_block_desc);
    ngpu_block_desc_reset(&s->frag_block_desc);
    ngpu_block_desc_reset(&s->user_block_desc);
    ngli_darray_reset(&s->textures_map);
    ngli_darray_reset(&s->blocks_map);
}

const struct node_class ngli_effect2d_class = {
    .id        = NGL_NODE_EFFECT2D,
    .name      = "Effect2D",
    .priv_size = sizeof(struct effect2d_priv),
    .init      = effect2d_init,
    .get_child_render_state = effect2d_get_child_render_state,
    .prepare   = effect2d_prepare,
    .update    = ngli_node_update_children,
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
