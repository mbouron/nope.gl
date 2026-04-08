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
#include "internal.h"
#include "log.h"
#include <ngpu/ngpu.h>
#include "node_block.h"
#include "node_texture.h"
#include "pipeline_compat.h"
#include "transforms.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

#include "source_mask_frag.h"
#include "source_mask_vert.h"

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

struct uniform_map {
    int32_t index;
    const void *data;
};

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
    struct darray blocks_map; // struct resource_map
    struct darray textures_map; // struct texture_map
    struct darray reframing_nodes; // struct ngl_node *
};

struct drawmask_opts {
    struct ngl_node *content;
    struct ngl_node *mask;
    int inverted;
    enum ngli_blending blending;
    struct ngl_node *geometry;
    struct ngl_node **filters;
    size_t nb_filters;
};

struct drawmask_priv {
    struct filterschain *filterschain;
    char *combined_fragment;
    struct ngpu_pgcraft_attribute position_attr;
    struct ngpu_pgcraft_attribute uvcoord_attr;
    uint32_t nb_vertices;
    enum ngpu_primitive_topology topology;
    struct geometry *geometry;
    int own_geometry;
    struct pipeline_desc pipeline_desc;
    struct darray uniforms; // struct ngpu_pgcraft_uniform
    struct ngpu_pgcraft *crafter;
    int32_t modelview_matrix_index;
    int32_t projection_matrix_index;
    int32_t aspect_index;
    struct darray uniforms_map; // struct uniform_map
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

#define OFFSET(x) offsetof(struct drawmask_opts, x)
static const struct node_param drawmask_params[] = {
    {"content",   NGLI_PARAM_TYPE_NODE, OFFSET(content),
                 .flags=NGLI_PARAM_FLAG_NON_NULL,
                 .node_types=(const uint32_t[]){TRANSFORM_TYPES_ARGS, NGL_NODE_TEXTURE2D, NGLI_NODE_NONE},
                 .desc=NGLI_DOCSTRING("content texture being masked")},
    {"mask",      NGLI_PARAM_TYPE_NODE, OFFSET(mask),
                 .flags=NGLI_PARAM_FLAG_NON_NULL,
                 .node_types=(const uint32_t[]){TRANSFORM_TYPES_ARGS, NGL_NODE_TEXTURE2D, NGLI_NODE_NONE},
                 .desc=NGLI_DOCSTRING("texture serving as mask (only the red channel is used)")},
    {"inverted",  NGLI_PARAM_TYPE_BOOL, OFFSET(inverted),
                 .desc=NGLI_DOCSTRING("whether to dig into or keep")},
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

static int build_uniforms_map(struct drawmask_priv *s)
{
    ngli_darray_init(&s->uniforms_map, sizeof(struct uniform_map), 0);

    const struct ngpu_pgcraft_uniform *uniforms = ngli_darray_data(&s->uniforms);
    for (size_t i = 0; i < ngli_darray_count(&s->uniforms); i++) {
        const struct ngpu_pgcraft_uniform *uniform = &uniforms[i];
        const int32_t index = ngpu_pgcraft_get_uniform_index(s->crafter, uniform->name, uniform->stage);

        /* The following can happen if the driver makes optimisation (MESA is
         * typically able to optimize several passes of the same filter) */
        if (index < 0)
            continue;

        /* This skips unwanted uniforms such as modelview and projection which
         * are handled separately */
        if (!uniform->data)
            continue;

        const struct uniform_map map = {.index=index, .data=uniform->data};
        if (!ngli_darray_push(&s->uniforms_map, &map))
            return NGL_ERROR_MEMORY;
    }

    return 0;
}

static int drawmask_init(struct ngl_node *node)
{
    struct drawmask_priv *s = node->priv_data;
    const struct drawmask_opts *o = node->opts;
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    const struct ngl_node *content = ngli_transform_get_leaf_node(o->content);
    if (!content) {
        LOG(ERROR, "no content texture found at the end of the transform chain");
        return NGL_ERROR_INVALID_USAGE;
    }

    const struct ngl_node *mask = ngli_transform_get_leaf_node(o->mask);
    if (!mask) {
        LOG(ERROR, "no mask texture found at the end of the transform chain");
        return NGL_ERROR_INVALID_USAGE;
    }

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

    int ret = ngli_filterschain_init(s->filterschain, "source_mask", source_mask_frag, 0);
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

    /* Register uniforms: common + source + filters */
    ngli_darray_init(&s->uniforms, sizeof(struct ngpu_pgcraft_uniform), 0);

    const struct ngpu_pgcraft_uniform common_uniforms[] = {
        {.name="modelview_matrix",  .type=NGPU_TYPE_MAT4,  .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="projection_matrix", .type=NGPU_TYPE_MAT4,  .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="aspect",            .type=NGPU_TYPE_F32,   .stage=NGPU_PROGRAM_STAGE_FRAG},
    };
    for (size_t i = 0; i < NGLI_ARRAY_NB(common_uniforms); i++)
        if (!ngli_darray_push(&s->uniforms, &common_uniforms[i]))
            return NGL_ERROR_MEMORY;

    const struct ngpu_pgcraft_uniform source_uniforms[] = {
        {.name="inverted", .type=NGPU_TYPE_BOOL, .stage=NGPU_PROGRAM_STAGE_FRAG, .data=&o->inverted},
    };
    for (size_t i = 0; i < NGLI_ARRAY_NB(source_uniforms); i++)
        if (!ngli_darray_push(&s->uniforms, &source_uniforms[i]))
            return NGL_ERROR_MEMORY;

    const struct darray *comb_uniforms_array = ngli_filterschain_get_resources(s->filterschain);
    const struct ngpu_pgcraft_uniform *comb_uniforms = ngli_darray_data(comb_uniforms_array);
    for (size_t i = 0; i < ngli_darray_count(comb_uniforms_array); i++)
        if (!ngli_darray_push(&s->uniforms, &comb_uniforms[i]))
            return NGL_ERROR_MEMORY;

    /* Setup textures */
    struct texture_info *content_info = o->content->priv_data;
    struct texture_info *mask_info = o->mask->priv_data;
    struct ngpu_pgcraft_texture textures[] = {
        {
            .name        = "content",
            .type        = ngli_node_texture_get_pgcraft_texture_type(o->content),
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .image       = &content_info->image,
            .format      = content_info->params.format,
            .clamp_video = content_info->clamp_video,
            .premult     = content_info->premult,
        }, {
            .name        = "mask",
            .type        = ngli_node_texture_get_pgcraft_texture_type(o->mask),
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .image       = &mask_info->image,
            .format      = mask_info->params.format,
            .clamp_video = mask_info->clamp_video,
            .premult     = mask_info->premult,
        },
    };

    /* Craft the program */
    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "uv",            .type = NGPU_TYPE_VEC2},
        {.name = "content_coord", .type = NGPU_TYPE_VEC2},
        {.name = "mask_coord",    .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_attribute attributes[] = {
        s->position_attr,
        s->uvcoord_attr,
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/drawmask",
        .vert_base        = source_mask_vert,
        .frag_base        = s->combined_fragment,
        .uniforms         = ngli_darray_data(&s->uniforms),
        .nb_uniforms      = ngli_darray_count(&s->uniforms),
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter)
        return NGL_ERROR_MEMORY;

    ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    if (ret < 0)
        return ret;

    s->modelview_matrix_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "modelview_matrix", NGPU_PROGRAM_STAGE_VERT);
    s->projection_matrix_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "projection_matrix", NGPU_PROGRAM_STAGE_VERT);
    s->aspect_index = ngpu_pgcraft_get_uniform_index(s->crafter, "aspect", NGPU_PROGRAM_STAGE_FRAG);

    ret = build_uniforms_map(s);
    if (ret < 0)
        return ret;

    return 0;
}

static int drawmask_prepare(struct ngl_node *node,
                            const struct ngpu_graphics_state *graphics_state,
                            const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct drawmask_priv *s = node->priv_data;
    const struct drawmask_opts *o = node->opts;

    struct pipeline_desc *desc = &s->pipeline_desc;

    /* Init pipeline desc fields */
    ngli_darray_init(&desc->blocks_map, sizeof(struct resource_map), 0);
    ngli_darray_init(&desc->textures_map, sizeof(struct texture_map), 0);
    ngli_darray_init(&desc->reframing_nodes, sizeof(struct ngl_node *), 0);

    /* Apply blending preset */
    struct ngpu_graphics_state state = *graphics_state;
    int ret = ngli_blending_apply_preset(&state, o->blending);
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
        .compat_info      = ngpu_pgcraft_get_compat_info(s->crafter),
    };

    ret = ngli_pipeline_compat_init(desc->pipeline_compat, &params);
    if (ret < 0)
        return ret;

    /* Build texture map */
    const struct ngpu_pgcraft_compat_info *info = ngpu_pgcraft_get_compat_info(s->crafter);
    for (size_t i = 0; i < info->nb_texture_infos; i++) {
        const struct texture_map map = {.image = info->images[i], .image_rev = SIZE_MAX};
        if (!ngli_darray_push(&desc->textures_map, &map))
            return NGL_ERROR_MEMORY;
    }

    /* Push reframing nodes for content and mask textures */
    if (!ngli_darray_push(&desc->reframing_nodes, &o->content) ||
        !ngli_darray_push(&desc->reframing_nodes, &o->mask))
        return NGL_ERROR_MEMORY;

    return 0;
}

static void drawmask_draw(struct ngl_node *node)
{
    struct drawmask_priv *s = node->priv_data;

    ngli_node_draw_children(node);

    struct ngl_ctx *ctx = node->ctx;
    struct pipeline_desc *desc = &s->pipeline_desc;
    struct pipeline_compat *pl_compat = desc->pipeline_compat;

    const float *modelview_matrix  = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const float *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    ngli_pipeline_compat_update_uniform(pl_compat, s->modelview_matrix_index, modelview_matrix);
    ngli_pipeline_compat_update_uniform(pl_compat, s->projection_matrix_index, projection_matrix);

    if (s->aspect_index >= 0) {
        const float aspect = (float)ctx->viewport.width / (float)ctx->viewport.height;
        ngli_pipeline_compat_update_uniform(pl_compat, s->aspect_index, &aspect);
    }

    const struct uniform_map *uniform_map = ngli_darray_data(&s->uniforms_map);
    for (size_t i = 0; i < ngli_darray_count(&s->uniforms_map); i++)
        ngli_pipeline_compat_update_uniform(pl_compat, uniform_map[i].index, uniform_map[i].data);

    struct texture_map *texture_map = ngli_darray_data(&desc->textures_map);
    const struct ngl_node **reframing_nodes = ngli_darray_data(&desc->reframing_nodes);
    for (size_t i = 0; i < ngli_darray_count(&desc->textures_map); i++) {
        if (texture_map[i].image_rev != texture_map[i].image->rev) {
            ngli_pipeline_compat_update_image(pl_compat, (int32_t)i, texture_map[i].image);
            texture_map[i].image_rev = texture_map[i].image->rev;
        }

        NGLI_ALIGNED_MAT(reframing_matrix);
        ngli_transform_chain_compute(reframing_nodes[i], reframing_matrix);
        ngli_pipeline_compat_apply_reframing_matrix(pl_compat, (int32_t)i, texture_map[i].image, reframing_matrix);
    }

    struct resource_map *resource_map = ngli_darray_data(&desc->blocks_map);
    for (size_t i = 0; i < ngli_darray_count(&desc->blocks_map); i++) {
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

static void drawmask_uninit(struct ngl_node *node)
{
    struct drawmask_priv *s = node->priv_data;
    struct pipeline_desc *desc = &s->pipeline_desc;

    /* Free pipeline desc resources */
    ngli_pipeline_compat_freep(&desc->pipeline_compat);
    ngli_darray_reset(&desc->blocks_map);
    ngli_darray_reset(&desc->textures_map);
    ngli_darray_reset(&desc->reframing_nodes);

    /* Free crafter and uniforms */
    ngpu_pgcraft_freep(&s->crafter);
    ngli_darray_reset(&s->uniforms);
    ngli_darray_reset(&s->uniforms_map);

    /* Free filter chain */
    ngli_freep(&s->combined_fragment);
    ngli_filterschain_freep(&s->filterschain);

    /* Free owned geometry */
    if (s->own_geometry)
        ngli_geometry_freep(&s->geometry);
}

const struct node_class ngli_drawmask_class = {
    .id        = NGL_NODE_DRAWMASK,
    .category  = NGLI_NODE_CATEGORY_DRAW,
    .name      = "DrawMask",
    .init      = drawmask_init,
    .prepare   = drawmask_prepare,
    .update    = ngli_node_update_children,
    .draw      = drawmask_draw,
    .uninit    = drawmask_uninit,
    .opts_size = sizeof(struct drawmask_opts),
    .priv_size = sizeof(struct drawmask_priv),
    .params    = drawmask_params,
    .file      = __FILE__,
};
