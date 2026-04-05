/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2016-2022 GoPro Inc.
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
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "blending.h"
#include "geometry.h"
#include "image.h"
#include "internal.h"
#include "log.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "node_block.h"
#include "node_buffer.h"
#include "node_resourceprops.h"
#include "node_texture.h"
#include "node_uniform.h"
#include "nopegl/nopegl.h"
#include "pass.h"
#include "pipeline_compat.h"
#include <ngpu/ngpu.h>
#include "utils/darray.h"
#include "utils/hmap.h"
#include "utils/utils.h"

struct resource_map {
    int32_t index;
    const struct block_info *info;
    size_t buffer_rev;
};

struct texture_map {
    const struct image *image;
    size_t image_rev;
};

static int register_uniform(struct pass *s, const char *name, struct ngl_node *uniform, enum ngpu_program_stage stage)
{
    struct ngpu_block_desc *block;
    if (stage == NGPU_PROGRAM_STAGE_VERT)
        block = &s->user_vert_block;
    else if (stage == NGPU_PROGRAM_STAGE_FRAG)
        block = &s->user_frag_block;
    else
        block = &s->user_comp_block;

    enum ngpu_type type;
    const void *data;
    size_t count = 0;
    if (uniform->cls->category == NGLI_NODE_CATEGORY_BUFFER) {
        struct buffer_info *buffer_info = uniform->priv_data;
        type  = buffer_info->layout.type;
        count = buffer_info->layout.count;
        data  = buffer_info->data;
    } else if (uniform->cls->category == NGLI_NODE_CATEGORY_VARIABLE) {
        struct variable_info *variable_info = uniform->priv_data;
        type = variable_info->data_type;
        data = variable_info->data;
    } else {
        ngli_assert(0);
    }

    const int field_idx = ngpu_block_desc_add_field(block, name, type, count);
    if (field_idx < 0)
        return field_idx;

    const struct pass_params *params = &s->params;
    if (params->properties) {
        struct ngl_node *resprops_node = ngli_hmap_get_str(params->properties, name);
        if (resprops_node) {
            const struct resourceprops_opts *resprops = resprops_node->opts;
            block->fields[field_idx].precision = resprops->precision;
        }
    }

    const struct user_uniform_entry entry = {
        .stage = stage,
        .data = data,
        .field_index = (size_t)field_idx,
    };
    if (!ngli_darray_push(&s->user_data_ptrs, &entry))
        return NGL_ERROR_MEMORY;

    return 0;
}

static int init_builtin_blocks(struct pass *s)
{

    static const struct ngpu_block_field vert_fields[] = {
        [PASS_BUILTIN_VERT_MODELVIEW_MATRIX]  = {.name = "ngl_modelview_matrix",  .type = NGPU_TYPE_MAT4},
        [PASS_BUILTIN_VERT_PROJECTION_MATRIX] = {.name = "ngl_projection_matrix", .type = NGPU_TYPE_MAT4},
        [PASS_BUILTIN_VERT_NORMAL_MATRIX]     = {.name = "ngl_normal_matrix",     .type = NGPU_TYPE_MAT3},
    };

    /*
     * Add builtin uniforms as the first fields of the user blocks so that
     * builtin and user uniforms share a single UBO push per stage at draw time.
     */
    if ((ngpu_block_desc_add_fields(&s->user_vert_block, vert_fields, NGLI_ARRAY_NB(vert_fields))) < 0)
        return NGL_ERROR_MEMORY;

    s->resolution_field_index = ngpu_block_desc_add_field(&s->user_frag_block, "ngl_resolution", NGPU_TYPE_VEC2, 0);
    if (s->resolution_field_index < 0)
        return s->resolution_field_index;

    return 0;
}

static int register_texture(struct pass *s, const char *name, struct ngl_node *texture, enum ngpu_program_stage stage)
{
    struct texture_info *texture_info = texture->priv_data;

    const struct pass_params *params = &s->params;

    enum ngpu_pgcraft_texture_type type = ngli_node_texture_get_pgcraft_texture_type(texture);
    enum ngpu_precision precision = 0;
    int writable = 0;
    int as_image = 0;
    if (params->properties) {
        const struct ngl_node *resprops_node = ngli_hmap_get_str(params->properties, name);
        if (resprops_node) {
            const struct resourceprops_opts *resprops = resprops_node->opts;
            as_image = resprops->as_image;
            if (as_image) {
                if (texture->cls->id == NGL_NODE_CUSTOMTEXTURE) {
                    LOG(ERROR, "custom textures do not support being used as images");
                    return NGL_ERROR_UNSUPPORTED;
                }
                /* Disable direct rendering when using image load/store */
                texture_info->supported_image_layouts = NGLI_IMAGE_LAYOUT_DEFAULT_BIT;
                texture_info->params.usage |= NGPU_TEXTURE_USAGE_STORAGE_BIT;
                type = ngli_node_texture_get_pgcraft_image_type(texture);
            }
            precision = resprops->precision;
            writable  = resprops->writable;
        }
    }

    struct ngpu_pgcraft_texture crafter_texture = {
        .type        = type,
        .stage       = stage,
        .precision   = precision,
        .writable    = writable,
        .format      = texture_info->params.format,
        .clamp_video = texture_info->clamp_video,
        .premult     = texture_info->premult,
        .image       = &texture_info->image,
    };
    snprintf(crafter_texture.name, sizeof(crafter_texture.name), "%s", name);

    if (!ngli_darray_push(&s->crafter_textures, &crafter_texture))
        return NGL_ERROR_MEMORY;

    return 0;
}

static int register_block(struct pass *s, const char *name, struct ngl_node *block_node, enum ngpu_program_stage stage)
{
    struct ngl_ctx *ctx = s->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    const struct ngpu_limits *limits = ngpu_ctx_get_limits(gpu_ctx);

    struct block_info *block_info = block_node->priv_data;
    struct ngpu_block_desc *block = &block_info->block;
    const size_t block_size = ngpu_block_desc_get_size(block, 0);

    /*
     * Select buffer type. We prefer UBO over SSBO, but in the following
     * situations, UBO is not possible.
     */
    enum ngpu_type type = NGPU_TYPE_UNIFORM_BUFFER;
    if (block->layout == NGPU_BLOCK_LAYOUT_STD430) {
        LOG(DEBUG, "block %s has a std430 layout, declaring it as SSBO", name);
        type = NGPU_TYPE_STORAGE_BUFFER;
    } else if (block_size > limits->max_uniform_block_size) {
        LOG(DEBUG, "block %s is larger than the max UBO size (%zu > %u), declaring it as SSBO",
            name, block_size, limits->max_uniform_block_size);
        if (block_size > limits->max_storage_block_size) {
            LOG(ERROR, "block %s is larger than the max SSBO size (%zu > %u)",
                name, block_size, limits->max_storage_block_size);
            return NGL_ERROR_GRAPHICS_LIMIT_EXCEEDED;
        }
        type = NGPU_TYPE_STORAGE_BUFFER;
    }

    int writable = 0;
    const struct pass_params *params = &s->params;
    if (params->properties) {
        const struct ngl_node *resprops_node = ngli_hmap_get_str(params->properties, name);
        if (resprops_node) {
            const struct resourceprops_opts *resprops = resprops_node->opts;
            if (resprops->writable)
                type = NGPU_TYPE_STORAGE_BUFFER;
            writable = resprops->writable;
        }
    }

    if (type == NGPU_TYPE_UNIFORM_BUFFER)
        ngli_node_block_extend_usage(block_node, NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    else if (type == NGPU_TYPE_STORAGE_BUFFER)
        ngli_node_block_extend_usage(block_node, NGPU_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    else
        ngli_assert(0);

    const struct ngpu_buffer *buffer = block_info->buffer;
    const size_t buffer_size = buffer ? ngpu_buffer_get_size(buffer) : 0;
    struct ngpu_pgcraft_block crafter_block = {
        .type     = type,
        .stage    = stage,
        .writable = writable,
        .block    = block,
        .buffer   = {
            .buffer = buffer,
            .size   = buffer_size,
        },
    };
    snprintf(crafter_block.name, sizeof(crafter_block.name), "%s", name);

    if (!ngli_darray_push(&s->crafter_blocks, &crafter_block))
        return NGL_ERROR_MEMORY;

    return 0;
}

static int register_attribute_from_buffer(struct pass *s, const char *name,
                                          struct ngpu_buffer *buffer, const struct buffer_layout *layout)
{
    if (!buffer)
        return 0;

    struct ngpu_pgcraft_attribute crafter_attribute = {
        .type   = layout->type,
        .format = layout->format,
        .stride = layout->stride,
        .offset = layout->offset,
        .buffer = buffer,
    };
    snprintf(crafter_attribute.name, sizeof(crafter_attribute.name), "%s", name);

    const struct pass_params *params = &s->params;
    if (params->properties) {
        const struct ngl_node *resprops_node = ngli_hmap_get_str(params->properties, name);
        if (resprops_node) {
            const struct resourceprops_opts *resprops = resprops_node->opts;
            crafter_attribute.precision = resprops->precision;
        }
    }

    if (!ngli_darray_push(&s->crafter_attributes, &crafter_attribute))
        return NGL_ERROR_MEMORY;

    return 0;
}

static int register_attribute(struct pass *s, const char *name, struct ngl_node *attribute, uint32_t rate)
{
    if (!attribute)
        return 0;

    ngli_node_buffer_extend_usage(attribute, NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    struct buffer_info *attribute_priv = attribute->priv_data;
    struct ngpu_pgcraft_attribute crafter_attribute = {
        .type   = attribute_priv->layout.type,
        .format = attribute_priv->layout.format,
        .stride = attribute_priv->layout.stride,
        .offset = attribute_priv->layout.offset,
        .rate   = rate,
        .buffer = attribute_priv->buffer,
    };
    snprintf(crafter_attribute.name, sizeof(crafter_attribute.name), "%s", name);

    attribute_priv->flags |= NGLI_BUFFER_INFO_FLAG_GPU_UPLOAD;

    const struct pass_params *params = &s->params;
    if (params->properties) {
        const struct ngl_node *resprops_node = ngli_hmap_get_str(params->properties, name);
        if (resprops_node) {
            const struct resourceprops_opts *resprops = resprops_node->opts;
            crafter_attribute.precision = resprops->precision;
        }
    }

    if (!ngli_darray_push(&s->crafter_attributes, &crafter_attribute))
        return NGL_ERROR_MEMORY;

    return 0;
}

static int register_resource(struct pass *s, const char *name, struct ngl_node *node, enum ngpu_program_stage stage)
{
    switch (node->cls->category) {
    case NGLI_NODE_CATEGORY_VARIABLE:
    case NGLI_NODE_CATEGORY_BUFFER:  return register_uniform(s, name, node, stage);
    case NGLI_NODE_CATEGORY_TEXTURE: return register_texture(s, name, node, stage);
    case NGLI_NODE_CATEGORY_BLOCK:   return register_block(s, name, node, stage);
    default:
        ngli_assert(0);
    }
}

static int register_resources(struct pass *s, const struct hmap *resources, enum ngpu_program_stage stage)
{
    if (!resources)
        return 0;

    const struct hmap_entry *entry = NULL;
    while ((entry = ngli_hmap_next(resources, entry))) {
        int ret = register_resource(s, entry->key.str, entry->data, stage);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int pass_graphics_init(struct pass *s)
{
    const struct pass_params *params = &s->params;
    const struct geometry *geometry = params->geometry;

    s->pipeline_type = NGPU_PIPELINE_TYPE_GRAPHICS;
    s->topology = geometry->topology;

    if (geometry->indices_buffer) {
        s->indices = geometry->indices_buffer;
        s->indices_layout = &geometry->indices_layout;
    } else {
        s->nb_vertices = (uint32_t)geometry->vertices_layout.count;
    }
    s->nb_instances = s->params.nb_instances;

    int ret;

    if ((ret = register_resources(s, params->vert_resources, NGPU_PROGRAM_STAGE_VERT)) < 0 ||
        (ret = register_resources(s, params->frag_resources, NGPU_PROGRAM_STAGE_FRAG)) < 0)
        return ret;

    if ((ret = register_attribute_from_buffer(s, "ngl_position", geometry->vertices_buffer, &geometry->vertices_layout)) < 0 ||
        (ret = register_attribute_from_buffer(s, "ngl_uvcoord",  geometry->uvcoords_buffer, &geometry->uvcoords_layout)) < 0 ||
        (ret = register_attribute_from_buffer(s, "ngl_normal",   geometry->normals_buffer,  &geometry->normals_layout)) < 0)
        return ret;

    if (params->attributes) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(params->attributes, entry))) {
            ret = register_attribute(s, entry->key.str, entry->data, 0);
            if (ret < 0)
                return ret;
        }
    }

    if (params->instance_attributes) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(params->instance_attributes, entry))) {
            ret = register_attribute(s, entry->key.str, entry->data, 1);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static int pass_compute_init(struct pass *s)
{
    const struct pass_params *params = &s->params;

    int ret = register_resources(s, params->compute_resources, NGPU_PROGRAM_STAGE_COMP);
    if (ret < 0)
        return ret;

    s->pipeline_type = NGPU_PIPELINE_TYPE_COMPUTE;

    return 0;
}

static enum ngpu_program_stage get_program_shader_stage(uint32_t stage_flags)
{
    if (stage_flags == NGPU_PROGRAM_STAGE_VERTEX_BIT)
        return NGPU_PROGRAM_STAGE_VERT;
    if (stage_flags == NGPU_PROGRAM_STAGE_FRAGMENT_BIT)
        return NGPU_PROGRAM_STAGE_FRAG;
    if (stage_flags == NGPU_PROGRAM_STAGE_COMPUTE_BIT)
        return NGPU_PROGRAM_STAGE_COMP;
    ngli_assert(0);
}

static int build_blocks_map(struct pass *s, struct pipeline_desc *desc)
{
    ngli_darray_init(&desc->blocks_map, sizeof(struct resource_map), 0);

    struct ngpu_bindgroup_layout_desc layout_desc = ngpu_pgcraft_get_bindgroup_layout_desc(s->crafter);

    for (size_t i = 0; i < layout_desc.nb_buffers; i++) {
        const struct ngpu_bindgroup_layout_entry *entry = &layout_desc.buffers[i];
        const struct hmap *resources = NULL;
        if (entry->stage_flags == NGPU_PROGRAM_STAGE_VERTEX_BIT)
            resources = s->params.vert_resources;
        else if (entry->stage_flags == NGPU_PROGRAM_STAGE_FRAGMENT_BIT)
            resources = s->params.frag_resources;
        else if (entry->stage_flags == NGPU_PROGRAM_STAGE_COMPUTE_BIT)
            resources = s->params.compute_resources;
        else
            continue; /* skip multi-stage entries (per-texture blocks) */

        if (!resources)
            continue;

        const enum ngpu_program_stage stage = get_program_shader_stage(entry->stage_flags);
        const char *name = ngpu_pgcraft_get_symbol_name(s->crafter, entry->id);
        const int32_t index = ngpu_pgcraft_get_block_index(s->crafter, name, stage);

        const struct ngl_node *node = ngli_hmap_get_str(resources, name);
        if (!node)
            continue;

        if (node->cls->category != NGLI_NODE_CATEGORY_BLOCK)
            continue;

        const struct block_info *info = node->priv_data;
        const struct resource_map map = {.index = index, .info = info, .buffer_rev = SIZE_MAX};
        if (!ngli_darray_push(&desc->blocks_map, &map))
            return NGL_ERROR_MEMORY;
    }

    return 0;
}

int ngli_pass_prepare(struct pass *s,
                      const struct ngpu_graphics_state *graphics_state,
                      const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct ngl_ctx *ctx = s->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter)
        return NGL_ERROR_MEMORY;

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label     = s->params.program_label,
        .vert_base         = s->params.vert_base,
        .frag_base         = s->params.frag_base,
        .comp_base         = s->params.comp_base,
        .textures          = ngli_darray_data(&s->crafter_textures),
        .nb_textures       = ngli_darray_count(&s->crafter_textures),
        .attributes        = ngli_darray_data(&s->crafter_attributes),
        .nb_attributes     = ngli_darray_count(&s->crafter_attributes),
        .blocks            = ngli_darray_data(&s->crafter_blocks),
        .nb_blocks         = ngli_darray_count(&s->crafter_blocks),
        .vert_out_vars     = s->params.vert_out_vars,
        .nb_vert_out_vars  = s->params.nb_vert_out_vars,
        .nb_frag_output    = s->params.nb_frag_output,
        .workgroup_size    = {NGLI_ARG_VEC3(s->params.workgroup_size)},
    };

    int ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    if (ret < 0)
        return ret;

    /* block indices for user blocks (which now include builtin fields) are looked up below */

    s->user_vert_block_index = ngpu_pgcraft_get_block_index(s->crafter, "ngl_vert", NGPU_PROGRAM_STAGE_VERT);
    s->user_frag_block_index = ngpu_pgcraft_get_block_index(s->crafter, "ngl_frag", NGPU_PROGRAM_STAGE_FRAG);
    s->user_comp_block_index = ngpu_pgcraft_get_block_index(s->crafter, "ngl_comp", NGPU_PROGRAM_STAGE_COMP);

    const enum ngpu_format format = rendertarget_layout->depth_stencil.format;
    if (graphics_state->depth_test && !ngpu_format_has_depth(format)) {
        LOG(ERROR, "depth testing is not supported on rendertargets with no depth attachment");
        return NGL_ERROR_INVALID_USAGE;
    }

    if (graphics_state->stencil_test && !ngpu_format_has_stencil(format)) {
        LOG(ERROR, "stencil operations are not supported on rendertargets with no stencil attachment");
        return NGL_ERROR_INVALID_USAGE;
    }

    struct ngpu_graphics_state state = *graphics_state;
    ret = ngli_blending_apply_preset(&state, s->params.blending);
    if (ret < 0)
        return ret;

    struct pipeline_desc *desc = &s->pipeline_desc;

    desc->pipeline_compat = ngli_pipeline_compat_create(gpu_ctx);
    if (!desc->pipeline_compat)
        return NGL_ERROR_MEMORY;

    const struct ngpu_pgcraft_texture_infos tex_infos = ngpu_pgcraft_get_texture_infos(s->crafter);

    const struct pipeline_compat_params params = {
        .type = s->pipeline_type,
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
        .texture_infos    = tex_infos,
    };
    ret = ngli_pipeline_compat_init(desc->pipeline_compat, &params);
    if (ret < 0)
        return ret;

    ret = build_blocks_map(s, desc);
    if (ret < 0)
        return ret;

    ngli_darray_init(&desc->textures_map, sizeof(struct texture_map), 0);
    for (size_t i = 0; i < tex_infos.nb_infos; i++) {
        const struct texture_map map = {.image = tex_infos.infos[i].image, .image_rev = SIZE_MAX};
        if (!ngli_darray_push(&desc->textures_map, &map))
            return NGL_ERROR_MEMORY;
    }

    return 0;
}

int ngli_pass_init(struct pass *s, struct ngl_ctx *ctx, const struct pass_params *params)
{
    s->ctx = ctx;
    s->params = *params;

    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    ngli_darray_init(&s->crafter_attributes, sizeof(struct ngpu_pgcraft_attribute), 0);
    ngli_darray_init(&s->crafter_textures, sizeof(struct ngpu_pgcraft_texture), 0);
    ngli_darray_init(&s->crafter_blocks, sizeof(struct ngpu_pgcraft_block), 0);
    ngli_darray_init(&s->user_data_ptrs, sizeof(struct user_uniform_entry), 0);

    ngpu_block_desc_init(gpu_ctx, &s->user_vert_block, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_init(gpu_ctx, &s->user_frag_block, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_init(gpu_ctx, &s->user_comp_block, NGPU_BLOCK_LAYOUT_STD140);

    int ret;

    if (params->geometry) {
        ret = init_builtin_blocks(s);
        if (ret < 0)
            return ret;
    }

    ret = params->geometry ? pass_graphics_init(s)
                           : pass_compute_init(s);
    if (ret < 0)
        return ret;

    /* Register user uniform blocks (non-empty ones only) */
    struct ngpu_buffer *staging_buf = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);

    const struct {
        struct ngpu_block_desc *desc;
        const char *name;
        enum ngpu_program_stage stage;
    } user_blocks[] = {
        {&s->user_vert_block, "ngl_vert", NGPU_PROGRAM_STAGE_VERT},
        {&s->user_frag_block, "ngl_frag", NGPU_PROGRAM_STAGE_FRAG},
        {&s->user_comp_block, "ngl_comp", NGPU_PROGRAM_STAGE_COMP},
    };

    for (size_t i = 0; i < NGLI_ARRAY_NB(user_blocks); i++) {
        const size_t size = ngpu_block_desc_get_size(user_blocks[i].desc, 0);
        if (!size)
            continue;
        struct ngpu_pgcraft_block blk = {
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = user_blocks[i].stage,
            .block         = user_blocks[i].desc,
            .buffer        = {.buffer = staging_buf, .size = size},
        };
        snprintf(blk.name, sizeof(blk.name), "%s", user_blocks[i].name);
        if (!ngli_darray_push(&s->crafter_blocks, &blk))
            return NGL_ERROR_MEMORY;
    }

    return 0;
}

void ngli_pass_uninit(struct pass *s)
{
    if (!s->ctx)
        return;

    struct pipeline_desc *desc = &s->pipeline_desc;
    ngli_pipeline_compat_freep(&desc->pipeline_compat);
    ngli_darray_reset(&desc->blocks_map);
    ngli_darray_reset(&desc->textures_map);

    ngpu_pgcraft_freep(&s->crafter);
    ngpu_block_desc_reset(&s->user_vert_block);
    ngpu_block_desc_reset(&s->user_frag_block);
    ngpu_block_desc_reset(&s->user_comp_block);
    ngli_darray_reset(&s->user_data_ptrs);
    ngli_darray_reset(&s->crafter_attributes);
    ngli_darray_reset(&s->crafter_textures);
    ngli_darray_reset(&s->crafter_blocks);

    memset(s, 0, sizeof(*s));
}

int ngli_pass_exec(struct pass *s)
{
    struct ngl_ctx *ctx = s->ctx;
    const struct pass_params *params = &s->params;
    struct pipeline_desc *desc = &s->pipeline_desc;
    struct pipeline_compat *pipeline_compat = desc->pipeline_compat;


    /* Fill and push user uniform blocks */
    const struct {
        const struct ngpu_block_desc *desc;
        int32_t index;
        enum ngpu_program_stage stage;
    } user_block_infos[] = {
        {&s->user_vert_block, s->user_vert_block_index, NGPU_PROGRAM_STAGE_VERT},
        {&s->user_frag_block, s->user_frag_block_index, NGPU_PROGRAM_STAGE_FRAG},
        {&s->user_comp_block, s->user_comp_block_index, NGPU_PROGRAM_STAGE_COMP},
    };

    for (size_t b = 0; b < NGLI_ARRAY_NB(user_block_infos); b++) {
        const struct ngpu_block_desc *block = user_block_infos[b].desc;
        const int32_t block_idx = user_block_infos[b].index;
        const size_t block_size = ngpu_block_desc_get_size(block, 0);
        if (!block_size || block_idx < 0)
            continue;

        size_t offset = 0;
        uint8_t *data = ngli_staging_buffer_reserve(ctx->current_staging_buffer, block_size, &offset);

        /* Write builtin uniforms directly into the staging buffer */
        if (user_block_infos[b].stage == NGPU_PROGRAM_STAGE_VERT) {
            const float *modelview_matrix = ngli_darray_tail(&ctx->modelview_matrix_stack);
            const float *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);
            ngpu_block_field_copy(&block->fields[PASS_BUILTIN_VERT_MODELVIEW_MATRIX],
                                  data + block->fields[PASS_BUILTIN_VERT_MODELVIEW_MATRIX].offset,
                                  (const uint8_t *)modelview_matrix);
            ngpu_block_field_copy(&block->fields[PASS_BUILTIN_VERT_PROJECTION_MATRIX],
                                  data + block->fields[PASS_BUILTIN_VERT_PROJECTION_MATRIX].offset,
                                  (const uint8_t *)projection_matrix);
            float normal_matrix[3 * 3];
            ngli_mat3_from_mat4(normal_matrix, modelview_matrix);
            ngli_mat3_inverse(normal_matrix, normal_matrix);
            ngli_mat3_transpose(normal_matrix, normal_matrix);
            ngpu_block_field_copy(&block->fields[PASS_BUILTIN_VERT_NORMAL_MATRIX],
                                  data + block->fields[PASS_BUILTIN_VERT_NORMAL_MATRIX].offset,
                                  (const uint8_t *)normal_matrix);
        } else if (user_block_infos[b].stage == NGPU_PROGRAM_STAGE_FRAG && s->resolution_field_index >= 0) {
            const float resolution[2] = {(float)ctx->viewport.width, (float)ctx->viewport.height};
            ngpu_block_field_copy(&block->fields[s->resolution_field_index],
                                  data + block->fields[s->resolution_field_index].offset,
                                  (const uint8_t *)resolution);
        }

        /* Write user uniforms directly into the staging buffer */
        const struct user_uniform_entry *entries = ngli_darray_data(&s->user_data_ptrs);
        for (size_t i = 0; i < ngli_darray_count(&s->user_data_ptrs); i++) {
            const struct user_uniform_entry *entry = &entries[i];
            if (entry->stage != user_block_infos[b].stage)
                continue;
            const struct ngpu_block_field *field = &block->fields[entry->field_index];
            if (entry->data)
                ngpu_block_field_copy_count(field, data + field->offset, (const uint8_t *)entry->data, 0);
        }

        struct ngpu_buffer *buffer = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);
        ngli_pipeline_compat_update_buffer(pipeline_compat, block_idx, buffer, offset, block_size);
    }

    struct texture_map *texture_map = ngli_darray_data(&desc->textures_map);
    for (size_t i = 0; i < ngli_darray_count(&desc->textures_map); i++) {
        ngli_pipeline_compat_update_image(pipeline_compat, (int32_t)i, texture_map[i].image, ctx->current_staging_buffer);
    }

    struct resource_map *resource_map = ngli_darray_data(&desc->blocks_map);
    for (size_t i = 0; i < ngli_darray_count(&desc->blocks_map); i++) {
        const struct block_info *info = resource_map[i].info;
        if (resource_map[i].buffer_rev != info->buffer_rev) {
            ngli_pipeline_compat_update_buffer(pipeline_compat, resource_map[i].index, info->buffer, 0, 0);
            resource_map[i].buffer_rev = info->buffer_rev;
        }
    }

    if (s->pipeline_type == NGPU_PIPELINE_TYPE_GRAPHICS) {
        struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

        if (!ngpu_ctx_is_render_pass_active(gpu_ctx)) {
            ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);
        }

        ngpu_ctx_set_viewport(gpu_ctx, &ctx->viewport);
        ngpu_ctx_set_scissor(gpu_ctx, &ctx->scissor);

        if (s->indices)
            ngli_pipeline_compat_draw_indexed(pipeline_compat, s->indices, s->indices_layout->format,
                                              (uint32_t)s->indices_layout->count, s->nb_instances);
        else
            ngli_pipeline_compat_draw(pipeline_compat, s->nb_vertices, s->nb_instances, 0);
    } else {
        struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

        if (ngpu_ctx_is_render_pass_active(gpu_ctx)) {
            ngpu_ctx_end_render_pass(gpu_ctx);
        }

        ngli_pipeline_compat_dispatch(pipeline_compat, NGLI_ARG_VEC3(params->workgroup_count));
    }

    return 0;
}
