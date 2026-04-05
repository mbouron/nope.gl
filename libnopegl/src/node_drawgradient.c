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
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

#include "source_gradient_frag.h"
#include "source_gradient_vert.h"
#include "source_gradient4_frag.h"
#include "source_gradient4_vert.h"

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
    struct darray blocks_map; // struct resource_map
    struct darray textures_map; // struct texture_map
    struct darray reframing_nodes; // struct ngl_node *
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

struct drawgradient_vert_block {
    NGLI_ALIGNED_MAT(modelview_matrix);
    NGLI_ALIGNED_MAT(projection_matrix);
};

struct drawgradient4_vert_block {
    NGLI_ALIGNED_MAT(modelview_matrix);
    NGLI_ALIGNED_MAT(projection_matrix);
};

struct drawgradient_frag_block {
    float aspect;
    float _pad0[3];
    float color0[3];
    float _pad1;
    float color1[3];
    float opacity0;
    float opacity1;
    float _pad2;
    float pos0[2];
    float pos1[2];
    int32_t mode;
    int32_t linear;
};

struct drawgradient4_frag_block {
    float aspect;
    float _pad0[3];
    float color_tl[3];
    float _pad1;
    float color_tr[3];
    float _pad2;
    float color_br[3];
    float _pad3;
    float color_bl[3];
    float opacity_tl;
    float opacity_tr;
    float opacity_br;
    float opacity_bl;
    int32_t linear;
};

/*
 * DrawGradient
 */

struct drawgradient_opts {
    struct ngl_node *color0_node;
    float color0[3];
    struct ngl_node *color1_node;
    float color1[3];
    struct ngl_node *opacity0_node;
    float opacity0;
    struct ngl_node *opacity1_node;
    float opacity1;
    struct ngl_node *pos0_node;
    float pos0[2];
    struct ngl_node *pos1_node;
    float pos1[2];
    int mode;
    struct ngl_node *linear_node;
    int linear;
    enum ngli_blending blending;
    struct ngl_node *geometry;
    struct ngl_node **filters;
    size_t nb_filters;
};

struct drawgradient_priv {
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

#define GRADIENT_MODE_RAMP   0
#define GRADIENT_MODE_RADIAL 1

static const struct param_choices gradient_mode_choices = {
    .name = "gradient_mode",
    .consts = {
        {"ramp",   GRADIENT_MODE_RAMP,   .desc=NGLI_DOCSTRING("straight line gradient, uniform perpendicularly to the line between the points")},
        {"radial", GRADIENT_MODE_RADIAL, .desc=NGLI_DOCSTRING("distance between the points spread circularly")},
        {NULL}
    }
};

#define OFFSET(x) offsetof(struct drawgradient_opts, x)
static const struct node_param drawgradient_params[] = {
    {"color0",   NGLI_PARAM_TYPE_VEC3, OFFSET(color0_node), {.vec={0.f, 0.f, 0.f}},
                 .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                 .desc=NGLI_DOCSTRING("color of the first point")},
    {"color1",   NGLI_PARAM_TYPE_VEC3, OFFSET(color1_node), {.vec={1.f, 1.f, 1.f}},
                 .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                 .desc=NGLI_DOCSTRING("color of the second point")},
    {"opacity0", NGLI_PARAM_TYPE_F32, OFFSET(opacity0_node), {.f32=1.f},
                 .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                 .desc=NGLI_DOCSTRING("opacity of the first color")},
    {"opacity1", NGLI_PARAM_TYPE_F32, OFFSET(opacity1_node), {.f32=1.f},
                 .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                 .desc=NGLI_DOCSTRING("opacity of the second color")},
    {"pos0",     NGLI_PARAM_TYPE_VEC2, OFFSET(pos0_node), {.vec={0.f, 0.5f}},
                 .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                 .desc=NGLI_DOCSTRING("position of the first point (in UV coordinates)")},
    {"pos1",     NGLI_PARAM_TYPE_VEC2, OFFSET(pos1_node), {.vec={1.f, 0.5f}},
                 .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                 .desc=NGLI_DOCSTRING("position of the second point (in UV coordinates)")},
    {"mode",     NGLI_PARAM_TYPE_SELECT, OFFSET(mode), {.i32=GRADIENT_MODE_RAMP},
                 .choices=&gradient_mode_choices,
                 .desc=NGLI_DOCSTRING("mode of interpolation between the two points")},
    {"linear",   NGLI_PARAM_TYPE_BOOL, OFFSET(linear_node), {.i32=1},
                 .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                 .desc=NGLI_DOCSTRING("interpolate colors linearly")},
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

static int drawgradient_init(struct ngl_node *node)
{
    struct drawgradient_priv *s = node->priv_data;
    const struct drawgradient_opts *o = node->opts;
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;


    /* Setup geometry */
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

    int ret = ngli_filterschain_init(s->filterschain, "source_gradient", source_gradient_frag, NGLI_FILTER_HELPER_SRGB);
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
    ngpu_block_desc_add_field(&s->frag_block_desc, "color0", NGPU_TYPE_VEC3, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "color1", NGPU_TYPE_VEC3, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "opacity0", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "opacity1", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "pos0", NGPU_TYPE_VEC2, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "pos1", NGPU_TYPE_VEC2, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "mode", NGPU_TYPE_I32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "linear", NGPU_TYPE_BOOL, 0);
    s->first_filter_field = s->frag_block_desc.nb_fields;

    const struct darray *comb_uniforms_array = ngli_filterschain_get_resources(s->filterschain);
    const struct ngli_filter_resource *comb_uniforms = ngli_darray_data(comb_uniforms_array);
    for (size_t i = 0; i < ngli_darray_count(comb_uniforms_array); i++)
        ngpu_block_desc_add_field(&s->frag_block_desc, comb_uniforms[i].name, comb_uniforms[i].type, 0);

    return 0;
}

static int drawgradient_prepare(struct ngl_node *node,
                                const struct ngpu_graphics_state *graphics_state,
                                const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct drawgradient_priv *s = node->priv_data;
    const struct drawgradient_opts *o = node->opts;

    struct pipeline_desc *desc = &s->pipeline_desc;

    /* Init pipeline desc fields */
    ngli_darray_init(&desc->blocks_map, sizeof(struct resource_map), 0);
    ngli_darray_init(&desc->textures_map, sizeof(struct texture_map), 0);
    ngli_darray_init(&desc->reframing_nodes, sizeof(struct ngl_node *), 0);

    const size_t vert_size = ngpu_block_desc_get_size(&s->vert_block_desc, 0);
    const size_t frag_size = ngpu_block_desc_get_size(&s->frag_block_desc, 0);
    ngli_assert(vert_size == sizeof(struct drawgradient_vert_block));
    ngli_assert(frag_size >= sizeof(struct drawgradient_frag_block));

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
        .program_label    = "nopegl/drawgradient",
        .vert_base        = source_gradient_vert,
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

static void drawgradient_draw(struct ngl_node *node)
{
    struct drawgradient_priv *s = node->priv_data;
    const struct drawgradient_opts *o = node->opts;

    ngli_node_draw_children(node);

    struct ngl_ctx *ctx = node->ctx;
    struct pipeline_desc *desc = &s->pipeline_desc;
    struct pipeline_compat *pl_compat = desc->pipeline_compat;

    const float *modelview_matrix  = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const float *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    /* Fill and push vertex block to staging buffer */
    struct drawgradient_vert_block vert_data;
    memcpy(vert_data.modelview_matrix, modelview_matrix, sizeof(vert_data.modelview_matrix));
    memcpy(vert_data.projection_matrix, projection_matrix, sizeof(vert_data.projection_matrix));

    if (s->vert_block_index >= 0) {
        const size_t vert_offset = ngli_staging_buffer_push(ctx->current_staging_buffer, &vert_data, sizeof(vert_data));
        struct ngpu_buffer *staging_buf = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->vert_block_index,
                                           staging_buf, vert_offset, sizeof(vert_data));
    }

    /* Fill and push fragment block to staging buffer */
    if (s->frag_block_index >= 0) {
        const struct ngpu_block_desc *block = &s->frag_block_desc;
        const size_t frag_size = ngpu_block_desc_get_size(block, 0);
        size_t frag_offset = 0;
        uint8_t *data = ngli_staging_buffer_reserve(ctx->current_staging_buffer, frag_size, &frag_offset);

        const float *color0_ptr   = ngli_node_get_data_ptr(o->color0_node, o->color0);
        const float *color1_ptr   = ngli_node_get_data_ptr(o->color1_node, o->color1);
        const float *opacity0_ptr = ngli_node_get_data_ptr(o->opacity0_node, &o->opacity0);
        const float *opacity1_ptr = ngli_node_get_data_ptr(o->opacity1_node, &o->opacity1);
        const float *pos0_ptr     = ngli_node_get_data_ptr(o->pos0_node, o->pos0);
        const float *pos1_ptr     = ngli_node_get_data_ptr(o->pos1_node, o->pos1);
        const int   *linear_ptr   = ngli_node_get_data_ptr(o->linear_node, &o->linear);

        const struct drawgradient_frag_block frag_data = {
            .aspect   = (float)ctx->viewport.width / (float)ctx->viewport.height,
            .color0   = {color0_ptr[0], color0_ptr[1], color0_ptr[2]},
            .color1   = {color1_ptr[0], color1_ptr[1], color1_ptr[2]},
            .opacity0 = *opacity0_ptr,
            .opacity1 = *opacity1_ptr,
            .pos0     = {pos0_ptr[0], pos0_ptr[1]},
            .pos1     = {pos1_ptr[0], pos1_ptr[1]},
            .mode     = o->mode,
            .linear   = *linear_ptr,
        };
        memcpy(data, &frag_data, sizeof(frag_data));

        /* Write filter uniforms */
        const struct darray *comb_uniforms_array = ngli_filterschain_get_resources(s->filterschain);
        const struct ngli_filter_resource *comb_uniforms = ngli_darray_data(comb_uniforms_array);
        for (size_t i = 0; i < ngli_darray_count(comb_uniforms_array); i++) {
            const size_t fi = s->first_filter_field + i;
            if (comb_uniforms[i].data)
                ngpu_block_field_copy(&block->fields[fi], data + block->fields[fi].offset, comb_uniforms[i].data);
        }

        struct ngpu_buffer *staging_buf = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->frag_block_index,
                                           staging_buf, frag_offset, frag_size);
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

static void drawgradient_uninit(struct ngl_node *node)
{
    struct drawgradient_priv *s = node->priv_data;
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

const struct node_class ngli_drawgradient_class = {
    .id        = NGL_NODE_DRAWGRADIENT,
    .category  = NGLI_NODE_CATEGORY_DRAW,
    .name      = "DrawGradient",
    .init      = drawgradient_init,
    .prepare   = drawgradient_prepare,
    .update    = ngli_node_update_children,
    .draw      = drawgradient_draw,
    .uninit    = drawgradient_uninit,
    .opts_size = sizeof(struct drawgradient_opts),
    .priv_size = sizeof(struct drawgradient_priv),
    .params    = drawgradient_params,
    .file      = __FILE__,
};

/*
 * DrawGradient4
 */

struct drawgradient4_opts {
    struct ngl_node *color_tl_node;
    float color_tl[3];
    struct ngl_node *color_tr_node;
    float color_tr[3];
    struct ngl_node *color_br_node;
    float color_br[3];
    struct ngl_node *color_bl_node;
    float color_bl[3];
    struct ngl_node *opacity_tl_node;
    float opacity_tl;
    struct ngl_node *opacity_tr_node;
    float opacity_tr;
    struct ngl_node *opacity_br_node;
    float opacity_br;
    struct ngl_node *opacity_bl_node;
    float opacity_bl;
    struct ngl_node *linear_node;
    int linear;
    enum ngli_blending blending;
    struct ngl_node *geometry;
    struct ngl_node **filters;
    size_t nb_filters;
};

struct drawgradient4_priv {
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

#define OFFSET(x) offsetof(struct drawgradient4_opts, x)
static const struct node_param drawgradient4_params[] = {
    {"color_tl",   NGLI_PARAM_TYPE_VEC3, OFFSET(color_tl_node), {.vec={1.f, .5f, 0.f}}, /* orange */
                   .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                   .desc=NGLI_DOCSTRING("top-left color")},
    {"color_tr",   NGLI_PARAM_TYPE_VEC3, OFFSET(color_tr_node), {.vec={0.f, 1.f, 0.f}}, /* green */
                   .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                   .desc=NGLI_DOCSTRING("top-right color")},
    {"color_br",   NGLI_PARAM_TYPE_VEC3, OFFSET(color_br_node), {.vec={0.f, .5f, 1.f}}, /* azure */
                   .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                   .desc=NGLI_DOCSTRING("bottom-right color")},
    {"color_bl",   NGLI_PARAM_TYPE_VEC3, OFFSET(color_bl_node), {.vec={1.f, .0f, 1.f}}, /* magenta */
                   .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                   .desc=NGLI_DOCSTRING("bottom-left color")},
    {"opacity_tl", NGLI_PARAM_TYPE_F32, OFFSET(opacity_tl_node), {.f32=1.f},
                   .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                   .desc=NGLI_DOCSTRING("opacity of the top-left color")},
    {"opacity_tr", NGLI_PARAM_TYPE_F32, OFFSET(opacity_tr_node), {.f32=1.f},
                   .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                   .desc=NGLI_DOCSTRING("opacity of the top-right color")},
    {"opacity_br", NGLI_PARAM_TYPE_F32, OFFSET(opacity_br_node), {.f32=1.f},
                   .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                   .desc=NGLI_DOCSTRING("opacity of the bottom-right color")},
    {"opacity_bl", NGLI_PARAM_TYPE_F32, OFFSET(opacity_bl_node), {.f32=1.f},
                   .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                   .desc=NGLI_DOCSTRING("opacity of the bottol-left color")},
    {"linear",     NGLI_PARAM_TYPE_BOOL, OFFSET(linear_node), {.i32=1},
                   .flags=NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
                   .desc=NGLI_DOCSTRING("interpolate colors linearly")},
    {"blending",   NGLI_PARAM_TYPE_SELECT, OFFSET(blending),
                   .choices=&ngli_blending_choices,
                   .desc=NGLI_DOCSTRING("define how this node and the current frame buffer are blending together")},
    {"geometry",   NGLI_PARAM_TYPE_NODE, OFFSET(geometry),
                   .node_types=GEOMETRY_TYPES_LIST,
                   .desc=NGLI_DOCSTRING("geometry to be rasterized")},
    {"filters",    NGLI_PARAM_TYPE_NODELIST, OFFSET(filters),
                   .node_types=FILTERS_TYPES_LIST,
                   .desc=NGLI_DOCSTRING("filter chain to apply on top of this source")},
    {NULL}
};
#undef OFFSET

static int drawgradient4_init(struct ngl_node *node)
{
    struct drawgradient4_priv *s = node->priv_data;
    const struct drawgradient4_opts *o = node->opts;
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

    int ret = ngli_filterschain_init(s->filterschain, "source_gradient4", source_gradient4_frag, NGLI_FILTER_HELPER_SRGB);
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
    ngpu_block_desc_add_field(&s->frag_block_desc, "color_tl", NGPU_TYPE_VEC3, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "color_tr", NGPU_TYPE_VEC3, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "color_br", NGPU_TYPE_VEC3, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "color_bl", NGPU_TYPE_VEC3, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "opacity_tl", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "opacity_tr", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "opacity_br", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "opacity_bl", NGPU_TYPE_F32, 0);
    ngpu_block_desc_add_field(&s->frag_block_desc, "linear", NGPU_TYPE_BOOL, 0);
    s->first_filter_field = s->frag_block_desc.nb_fields;

    const struct darray *comb_uniforms_array = ngli_filterschain_get_resources(s->filterschain);
    const struct ngli_filter_resource *comb_uniforms = ngli_darray_data(comb_uniforms_array);
    for (size_t i = 0; i < ngli_darray_count(comb_uniforms_array); i++)
        ngpu_block_desc_add_field(&s->frag_block_desc, comb_uniforms[i].name, comb_uniforms[i].type, 0);

    return 0;
}

static int drawgradient4_prepare(struct ngl_node *node,
                                 const struct ngpu_graphics_state *graphics_state,
                                 const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct drawgradient4_priv *s = node->priv_data;
    const struct drawgradient4_opts *o = node->opts;

    struct pipeline_desc *desc = &s->pipeline_desc;

    /* Init pipeline desc fields */
    ngli_darray_init(&desc->blocks_map, sizeof(struct resource_map), 0);
    ngli_darray_init(&desc->textures_map, sizeof(struct texture_map), 0);
    ngli_darray_init(&desc->reframing_nodes, sizeof(struct ngl_node *), 0);

    struct ngpu_buffer *staging_buf = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);

    const size_t vert_size = ngpu_block_desc_get_size(&s->vert_block_desc, 0);
    const size_t frag_size = ngpu_block_desc_get_size(&s->frag_block_desc, 0);
    ngli_assert(vert_size == sizeof(struct drawgradient4_vert_block));
    ngli_assert(frag_size >= sizeof(struct drawgradient4_frag_block));

    const struct ngpu_pgcraft_block blocks[] = {
        {
            .name          = "vert_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_VERT,
            .block         = &s->vert_block_desc,
            .buffer        = {.buffer = staging_buf, .size = vert_size},
        },
        {
            .name          = "frag_params",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &s->frag_block_desc,
            .buffer        = {.buffer = staging_buf, .size = frag_size},
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
        .program_label    = "nopegl/drawgradient4",
        .vert_base        = source_gradient4_vert,
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

static void drawgradient4_draw(struct ngl_node *node)
{
    struct drawgradient4_priv *s = node->priv_data;
    const struct drawgradient4_opts *o = node->opts;

    ngli_node_draw_children(node);

    struct ngl_ctx *ctx = node->ctx;
    struct pipeline_desc *desc = &s->pipeline_desc;
    struct pipeline_compat *pl_compat = desc->pipeline_compat;

    const float *modelview_matrix  = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const float *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    /* Fill and push vertex block to staging buffer */
    struct drawgradient4_vert_block vert_data;
    memcpy(vert_data.modelview_matrix, modelview_matrix, sizeof(vert_data.modelview_matrix));
    memcpy(vert_data.projection_matrix, projection_matrix, sizeof(vert_data.projection_matrix));

    if (s->vert_block_index >= 0) {
        const size_t vert_offset = ngli_staging_buffer_push(ctx->current_staging_buffer, &vert_data, sizeof(vert_data));
        struct ngpu_buffer *staging_buf = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->vert_block_index,
                                           staging_buf, vert_offset, sizeof(vert_data));
    }

    /* Fill and push fragment block to staging buffer */
    if (s->frag_block_index >= 0) {
        const struct ngpu_block_desc *block = &s->frag_block_desc;
        const size_t frag_size = ngpu_block_desc_get_size(block, 0);
        size_t frag_offset = 0;
        uint8_t *data = ngli_staging_buffer_reserve(ctx->current_staging_buffer, frag_size, &frag_offset);

        const float *color_tl_ptr   = ngli_node_get_data_ptr(o->color_tl_node, o->color_tl);
        const float *color_tr_ptr   = ngli_node_get_data_ptr(o->color_tr_node, o->color_tr);
        const float *color_br_ptr   = ngli_node_get_data_ptr(o->color_br_node, o->color_br);
        const float *color_bl_ptr   = ngli_node_get_data_ptr(o->color_bl_node, o->color_bl);
        const float *opacity_tl_ptr = ngli_node_get_data_ptr(o->opacity_tl_node, &o->opacity_tl);
        const float *opacity_tr_ptr = ngli_node_get_data_ptr(o->opacity_tr_node, &o->opacity_tr);
        const float *opacity_br_ptr = ngli_node_get_data_ptr(o->opacity_br_node, &o->opacity_br);
        const float *opacity_bl_ptr = ngli_node_get_data_ptr(o->opacity_bl_node, &o->opacity_bl);
        const int   *linear_ptr     = ngli_node_get_data_ptr(o->linear_node, &o->linear);

        const struct drawgradient4_frag_block frag_data = {
            .aspect     = (float)ctx->viewport.width / (float)ctx->viewport.height,
            .color_tl   = {color_tl_ptr[0], color_tl_ptr[1], color_tl_ptr[2]},
            .color_tr   = {color_tr_ptr[0], color_tr_ptr[1], color_tr_ptr[2]},
            .color_br   = {color_br_ptr[0], color_br_ptr[1], color_br_ptr[2]},
            .color_bl   = {color_bl_ptr[0], color_bl_ptr[1], color_bl_ptr[2]},
            .opacity_tl = *opacity_tl_ptr,
            .opacity_tr = *opacity_tr_ptr,
            .opacity_br = *opacity_br_ptr,
            .opacity_bl = *opacity_bl_ptr,
            .linear     = *linear_ptr,
        };
        memcpy(data, &frag_data, sizeof(frag_data));

        /* Write filter uniforms */
        const struct darray *comb_uniforms_array = ngli_filterschain_get_resources(s->filterschain);
        const struct ngli_filter_resource *comb_uniforms = ngli_darray_data(comb_uniforms_array);
        for (size_t i = 0; i < ngli_darray_count(comb_uniforms_array); i++) {
            const size_t fi = s->first_filter_field + i;
            if (comb_uniforms[i].data)
                ngpu_block_field_copy(&block->fields[fi], data + block->fields[fi].offset, comb_uniforms[i].data);
        }

        struct ngpu_buffer *staging_buf = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pl_compat, s->frag_block_index,
                                           staging_buf, frag_offset, frag_size);
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

static void drawgradient4_uninit(struct ngl_node *node)
{
    struct drawgradient4_priv *s = node->priv_data;
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

const struct node_class ngli_drawgradient4_class = {
    .id        = NGL_NODE_DRAWGRADIENT4,
    .category  = NGLI_NODE_CATEGORY_DRAW,
    .name      = "DrawGradient4",
    .init      = drawgradient4_init,
    .prepare   = drawgradient4_prepare,
    .update    = ngli_node_update_children,
    .draw      = drawgradient4_draw,
    .uninit    = drawgradient4_uninit,
    .opts_size = sizeof(struct drawgradient4_opts),
    .priv_size = sizeof(struct drawgradient4_priv),
    .params    = drawgradient4_params,
    .file      = __FILE__,
};
