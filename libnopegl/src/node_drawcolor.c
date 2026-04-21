/*
 * Copyright 2023 Nope Forge
 * Copyright 2021-2022 GoPro Inc.
 * Copyright 2021-2024 Clément Bœsch <u pkh.me>
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

#include "blending.h"
#include "filterschain.h"
#include "geometry.h"
#include "image.h"
#include "internal.h"
#include "log.h"
#include <ngpu/ngpu.h>
#include "node_block.h"
#include "node_uniform.h"
#include "pipeline_compat.h"
#include "transforms.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

#include "source_color_frag.h"
#include "source_color_vert.h"

#define VERTEX_USAGE_FLAGS (NGLI_BUFFER_USAGE_TRANSFER_DST_BIT | \
                            NGLI_BUFFER_USAGE_VERTEX_BUFFER_BIT) \

#define GEOMETRY_TYPES_LIST (const uint32_t[]){NGL_NODE_CIRCLE,          \
                                               NGL_NODE_GEOMETRY,        \
                                               NGL_NODE_QUAD,            \
                                               NGL_NODE_TRIANGLE,        \
                                               NGLI_NODE_NONE}

#define FILTERS_TYPES_LIST (const uint32_t[]){NGL_NODE_FILTERALPHA,          \
                                              NGL_NODE_FILTERCOLORMAP,       \
                                              NGL_NODE_FILTERCONTRAST,       \
                                              NGL_NODE_FILTEREXPOSURE,       \
                                              NGL_NODE_FILTERINVERSEALPHA,   \
                                              NGL_NODE_FILTERLINEAR2SRGB,    \
                                              NGL_NODE_FILTEROPACITY,        \
                                              NGL_NODE_FILTERPREMULT,        \
                                              NGL_NODE_FILTERSATURATION,     \
                                              NGL_NODE_FILTERSELECTOR,       \
                                              NGL_NODE_FILTERSRGB2LINEAR,    \
                                              NGLI_NODE_NONE}

struct resource_map {
    int32_t index;
    const struct block_info *info;
    size_t buffer_rev;
};

struct texture_map {
    const struct image *image;
    size_t image_rev;
};

struct pipeline_desc {
    struct pipeline_compat *pipeline_compat;
    NGLI_DARRAY(struct resource_map) blocks_map;
    NGLI_DARRAY(struct texture_map) textures_map;
    struct ngli_node_darray reframing_nodes;
};

struct drawcolor_opts {
    struct ngl_node *color_node;
    float color[3];
    struct ngl_node *opacity_node;
    float opacity;
    enum ngli_blending blending;
    struct ngl_node *geometry;
    struct ngl_node **filters;
    size_t nb_filters;
};

struct drawcolor_vert_block {
    struct ngli_mat4 modelview_matrix;
    struct ngli_mat4 projection_matrix;
};

struct drawcolor_frag_block {
    float aspect;
    float _pad0[3];
    float color[3];
    float opacity;
};

struct drawcolor_priv {
    size_t first_filter_field;
    struct filterschain *filterschain;
    char *combined_fragment;
    struct ngpu_pgcraft_attribute position_attr;
    struct ngpu_pgcraft_attribute uvcoord_attr;
    uint32_t nb_vertices;
    enum ngpu_primitive_topology topology;
    struct geometry *geometry;
    int own_geometry;
    struct pipeline_desc pipeline_desc;
    struct ngpu_pgcraft *crafter;
    struct ngpu_block_desc vert_block_desc;
    struct ngpu_block_desc frag_block_desc;
    int32_t vert_block_index;
    int32_t frag_block_index;
};

static const float default_vertices[] = {
   -1.f,-1.f, 0.f,
    1.f,-1.f, 0.f,
   -1.f, 1.f, 0.f,
    1.f, 1.f, 0.f,
};

static const float default_uvcoords[] = {
    0.f, 1.f,
    1.f, 1.f,
    0.f, 0.f,
    1.f, 0.f,
};

#define OFFSET(x) offsetof(struct drawcolor_opts, x)
static const struct node_param drawcolor_params[] = {
    {"color",    NGLI_PARAM_TYPE_VEC3, OFFSET(color_node), {.vec={1.f, 1.f, 1.f}},
                 .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                 .desc=NGLI_DOCSTRING("color of the shape")},
    {"opacity",  NGLI_PARAM_TYPE_F32, OFFSET(opacity_node), {.f32=1.f},
                 .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                 .desc=NGLI_DOCSTRING("opacity of the color")},
    {"blending", NGLI_PARAM_TYPE_SELECT, OFFSET(blending),
                 .choices=&ngli_blending_choices,
                 .desc=NGLI_DOCSTRING("define how this node and the current frame buffer are blending together")},
    {"geometry", NGLI_PARAM_TYPE_NODE, OFFSET(geometry),
                 .node_types=GEOMETRY_TYPES_LIST,
                 .desc=NGLI_DOCSTRING("geometry to be rasterized")},
    {"filters",  NGLI_PARAM_TYPE_NODELIST, OFFSET(filters),
                 .node_types=FILTERS_TYPES_LIST,
                 .desc=NGLI_DOCSTRING("filter chain to apply on top of this source")},
    {NULL}
};
#undef OFFSET


static int drawcolor_init(struct ngl_node *node)
{
    struct drawcolor_priv *s = node->priv_data;
    const struct drawcolor_opts *o = node->opts;
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    if (!o->geometry) {
        s->own_geometry = 1;

        s->geometry = ngli_geometry_create(gpu_ctx);
        if (!s->geometry)
            return NGL_ERROR_MEMORY;

        int ret;
        if ((ret = ngli_geometry_set_vertices(s->geometry, 4, default_vertices)) < 0 ||
            (ret = ngli_geometry_set_uvcoords(s->geometry, 4, default_uvcoords)) < 0 ||
            (ret = ngli_geometry_init(s->geometry, NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) < 0))
            return ret;
    } else {
        s->geometry = *(struct geometry **)o->geometry->priv_data;
    }

    struct ngpu_buffer *vertices = s->geometry->vertices_buffer;
    struct ngpu_buffer *uvcoords = s->geometry->uvcoords_buffer;
    struct buffer_layout vertices_layout = s->geometry->vertices_layout;
    struct buffer_layout uvcoords_layout = s->geometry->uvcoords_layout;

    if (!uvcoords) {
        LOG(ERROR, "the specified geometry is missing UV coordinates");
        return NGL_ERROR_INVALID_USAGE;
    }

    if (vertices_layout.type != NGPU_TYPE_VEC3) {
        LOG(ERROR, "only geometry with vec3 vertices are supported");
        return NGL_ERROR_UNSUPPORTED;
    }

    if (uvcoords && uvcoords_layout.type != NGPU_TYPE_VEC2) {
        LOG(ERROR, "only geometry with vec2 uvcoords are supported");
        return NGL_ERROR_UNSUPPORTED;
    }

    snprintf(s->position_attr.name, sizeof(s->position_attr.name), "position");
    s->position_attr.type   = NGPU_TYPE_VEC3;
    s->position_attr.format = NGPU_FORMAT_R32G32B32_SFLOAT;
    s->position_attr.stride = vertices_layout.stride;
    s->position_attr.offset = vertices_layout.offset;
    s->position_attr.buffer = vertices;

    snprintf(s->uvcoord_attr.name, sizeof(s->uvcoord_attr.name), "uvcoord");
    s->uvcoord_attr.type   = NGPU_TYPE_VEC2;
    s->uvcoord_attr.format = NGPU_FORMAT_R32G32_SFLOAT;
    s->uvcoord_attr.stride = uvcoords_layout.stride;
    s->uvcoord_attr.offset = uvcoords_layout.offset;
    s->uvcoord_attr.buffer = uvcoords;

    s->nb_vertices = (uint32_t)vertices_layout.count;
    s->topology = s->geometry->topology;

    /* Build filter chain */
    s->filterschain = ngli_filterschain_create();
    if (!s->filterschain)
        return NGL_ERROR_MEMORY;

    int ret = ngli_filterschain_init(s->filterschain, "source_color", source_color_frag, 0);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < o->nb_filters; i++) {
        const struct ngl_node *filter_node = o->filters[i];
        const struct filter *filter = filter_node->priv_data;
        ret = ngli_filterschain_add_filter(s->filterschain, filter);
        if (ret < 0)
            return ret;
    }

    s->combined_fragment = ngli_filterschain_get_combination(s->filterschain);
    if (!s->combined_fragment)
        return NGL_ERROR_MEMORY;

    /* Initialize vertex block descriptor */
    ngpu_block_desc_init(gpu_ctx, &s->vert_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_add_field(&s->vert_block_desc, "modelview_matrix", NGPU_TYPE_MAT4, 0);
    ngpu_block_desc_add_field(&s->vert_block_desc, "projection_matrix", NGPU_TYPE_MAT4, 0);

    /* Initialize fragment block descriptor */
    ngpu_block_desc_init(gpu_ctx, &s->frag_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_add_field(&s->frag_block_desc, "aspect", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "color", NGPU_TYPE_VEC3, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "opacity", NGPU_TYPE_F32, 0);
    s->first_filter_field = s->frag_block_desc.nb_fields;

    const struct ngli_filter_resource_darray *comb_uniforms_array = ngli_filterschain_get_resources(s->filterschain);
    const struct ngli_filter_resource *comb_uniforms = comb_uniforms_array->data;
    for (size_t i = 0; i < comb_uniforms_array->count; i++)
        ngpu_block_desc_add_field(&s->frag_block_desc, comb_uniforms[i].name, comb_uniforms[i].type, 0);

    return 0;
}

static int drawcolor_prepare(struct ngl_node *node,
                             const struct ngpu_graphics_state *graphics_state,
                             const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct drawcolor_priv *s = node->priv_data;
    const struct drawcolor_opts *o = node->opts;

    struct pipeline_desc *desc = &s->pipeline_desc;

    const size_t vert_size = ngpu_block_desc_get_size(&s->vert_block_desc, 0);
    const size_t frag_size = ngpu_block_desc_get_size(&s->frag_block_desc, 0);
    ngli_assert(vert_size == sizeof(struct drawcolor_vert_block));
    ngli_assert(frag_size >= sizeof(struct drawcolor_frag_block));

    const struct ngpu_pgcraft_block blocks[] = {
        {
            .name          = "vert_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_VERT,
            .block         = &s->vert_block_desc,
        },
        {
            .name          = "frag_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &s->frag_block_desc,
        },
    };

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "uv", .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_attribute attributes[] = {
        s->position_attr,
        s->uvcoord_attr,
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/drawcolor",
        .vert_base        = source_color_vert,
        .frag_base        = s->combined_fragment,
        .blocks           = blocks,
        .nb_blocks        = NGLI_ARRAY_NB(blocks),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter)
        return NGL_ERROR_MEMORY;

    int ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    if (ret < 0)
        return ret;

    s->vert_block_index = ngpu_pgcraft_get_block_index(s->crafter, "vert_params", NGPU_PROGRAM_STAGE_VERT);
    s->frag_block_index = ngpu_pgcraft_get_block_index(s->crafter, "frag_params", NGPU_PROGRAM_STAGE_FRAG);

    /* Apply blending preset */
    struct ngpu_graphics_state state = *graphics_state;
    ret = ngli_blending_apply_preset(&state, o->blending);
    if (ret < 0)
        return ret;

    /* Create and init pipeline */
    desc->pipeline_compat = ngli_pipeline_compat_create(gpu_ctx);
    if (!desc->pipeline_compat)
        return NGL_ERROR_MEMORY;

    const struct pipeline_compat_params params = {
        .type = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology     = s->topology,
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

    ret = ngli_pipeline_compat_init(desc->pipeline_compat, &params);
    if (ret < 0)
        return ret;

    return 0;
}

static void drawcolor_draw(struct ngl_node *node)
{
    struct drawcolor_priv *s = node->priv_data;
    const struct drawcolor_opts *o = node->opts;

    ngli_node_draw_children(node);

    struct ngl_ctx *ctx = node->ctx;
    struct pipeline_desc *desc = &s->pipeline_desc;
    struct pipeline_compat *pl_compat = desc->pipeline_compat;

    const struct ngli_mat4 *modelview_matrix  = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const struct ngli_mat4 *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    /* Fill and push vertex block to staging buffer */
    struct drawcolor_vert_block vert_data;
    vert_data.modelview_matrix = *modelview_matrix;
    vert_data.projection_matrix = *projection_matrix;

    if (s->vert_block_index >= 0) {
        const size_t vert_offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &vert_data, sizeof(vert_data));
        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->vert_block_index,
                                           staging_buf, vert_offset, sizeof(vert_data));
    }

    /* Fill and push fragment block to staging buffer */
    if (s->frag_block_index >= 0) {
        const struct ngpu_block_desc *block = &s->frag_block_desc;
        const size_t frag_size = ngpu_block_desc_get_size(block, 0);
        size_t frag_offset;
        uint8_t *data = ngpu_staging_buffer_reserve(ctx->current_staging_buffer, frag_size, &frag_offset);

        const float *color_ptr   = ngli_node_get_data_ptr(o->color_node, o->color);
        const float *opacity_ptr = ngli_node_get_data_ptr(o->opacity_node, &o->opacity);
        const struct drawcolor_frag_block frag_data = {
            .aspect  = (float)ctx->viewport.width / (float)ctx->viewport.height,
            .color   = {color_ptr[0], color_ptr[1], color_ptr[2]},
            .opacity = *opacity_ptr,
        };
        memcpy(data, &frag_data, sizeof(frag_data));

        /* Write filter uniforms */
        const struct ngli_filter_resource_darray *comb_uniforms_array = ngli_filterschain_get_resources(s->filterschain);
        const struct ngli_filter_resource *comb_uniforms = comb_uniforms_array->data;
        for (size_t i = 0; i < comb_uniforms_array->count; i++) {
            const size_t fi = s->first_filter_field + i;
            if (comb_uniforms[i].data)
                ngpu_block_field_copy(&block->fields[fi], data + block->fields[fi].offset, comb_uniforms[i].data);
        }

        struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->frag_block_index,
                                           staging_buf, frag_offset, frag_size);
    }

    struct texture_map *texture_map = desc->textures_map.data;
    for (size_t i = 0; i < desc->textures_map.count; i++) {
        if (texture_map[i].image_rev != texture_map[i].image->rev) {
            ngli_pipeline_compat_update_image(pl_compat, (int32_t)i, texture_map[i].image, ctx->current_staging_buffer);
            texture_map[i].image_rev = texture_map[i].image->rev;
        }

        struct ngli_mat4 reframing_matrix = {0};
        ngli_transform_chain_compute(desc->reframing_nodes.data[i], reframing_matrix.m);
        ngli_pipeline_compat_apply_reframing_matrix(pl_compat, (int32_t)i, texture_map[i].image, reframing_matrix.m, ctx->current_staging_buffer);
    }

    struct resource_map *resource_map = desc->blocks_map.data;
    for (size_t i = 0; i < desc->blocks_map.count; i++) {
        const struct block_info *info = resource_map[i].info;
        if (resource_map[i].buffer_rev != info->buffer_rev) {
            ngli_pipeline_compat_update_buffer(pl_compat, resource_map[i].index, info->buffer, 0, 0);
            resource_map[i].buffer_rev = info->buffer_rev;
        }
    }

    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    if (!ngpu_ctx_is_render_pass_active(gpu_ctx)) {
        ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);
    }

    ngpu_ctx_set_viewport(gpu_ctx, &ctx->viewport);
    ngpu_ctx_set_scissor(gpu_ctx, &ctx->scissor);

    if (s->geometry->indices_buffer) {
        const struct ngpu_buffer *indices = s->geometry->indices_buffer;
        const struct buffer_layout *layout = &s->geometry->indices_layout;
        ngli_pipeline_compat_draw_indexed(pl_compat, indices, layout->format, (uint32_t)layout->count, 1);
    } else {
        ngli_pipeline_compat_draw(pl_compat, s->nb_vertices, 1, 0);
    }
}

static void drawcolor_uninit(struct ngl_node *node)
{
    struct drawcolor_priv *s = node->priv_data;
    struct pipeline_desc *desc = &s->pipeline_desc;

    /* Free pipeline desc resources */
    ngli_pipeline_compat_freep(&desc->pipeline_compat);
    ngli_darray_reset(&desc->blocks_map);
    ngli_darray_reset(&desc->textures_map);
    ngli_darray_reset(&desc->reframing_nodes);

    /* Free crafter and block descriptors */
    ngpu_pgcraft_freep(&s->crafter);
    ngpu_block_desc_reset(&s->vert_block_desc);
    ngpu_block_desc_reset(&s->frag_block_desc);

    /* Free filter chain */
    ngli_freep(&s->combined_fragment);
    ngli_filterschain_freep(&s->filterschain);

    /* Free owned geometry */
    if (s->own_geometry)
        ngli_geometry_freep(&s->geometry);
}

const struct node_class ngli_drawcolor_class = {
    .id        = NGL_NODE_DRAWCOLOR,
    .category  = NGLI_NODE_CATEGORY_DRAW,
    .name      = "DrawColor",
    .init      = drawcolor_init,
    .prepare   = drawcolor_prepare,
    .update    = ngli_node_update_children,
    .draw      = drawcolor_draw,
    .uninit    = drawcolor_uninit,
    .opts_size = sizeof(struct drawcolor_opts),
    .priv_size = sizeof(struct drawcolor_priv),
    .params    = drawcolor_params,
    .file      = __FILE__,
};
