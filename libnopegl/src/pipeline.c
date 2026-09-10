/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2022 GoPro Inc.
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

#include <string.h>

#include "image.h"
#include "log.h"
#include "transforms.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "nopegl/nopegl.h"
#include "pipeline.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

#define NB_BINDGROUPS 16

NGLI_DECLARE_DARRAY_WITH_NAME(bindgroup_darray, struct ngpu_bindgroup *);

enum slot_mode {
    SLOT_UNBOUND,
    SLOT_DIRECT,
    SLOT_SOURCE,
    SLOT_IMAGE,
};

struct image_source {
    struct resource *resource;
    struct ngl_node *reframing_node;
};

struct pipeline {
    struct ngpu_ctx *gpu_ctx;
    enum ngpu_pipeline_type type;
    struct ngpu_pipeline_graphics graphics;
    const struct ngpu_program *program;
    struct ngpu_pipeline *gpu_pipeline;
    struct ngpu_bindgroup_layout_desc bindgroup_layout_desc;
    struct ngpu_bindgroup_layout *bindgroup_layout;
    struct bindgroup_darray bindgroups;
    struct ngpu_bindgroup *cur_bindgroup;
    size_t cur_bindgroup_index;
    struct ngpu_buffer **vertex_buffers;
    size_t nb_vertex_buffers;
    struct ngpu_texture_binding *textures;
    struct ngpu_texture **empty_textures;
    size_t nb_textures;
    struct ngpu_buffer_binding *buffers;
    size_t nb_buffers;
    uint32_t dynamic_offsets[NGPU_MAX_DYNAMIC_OFFSETS];
    size_t nb_dynamic_offsets;
    struct pipeline_buffer_source *buffer_sources;
    struct resource **vertex_sources;
    struct resource **texture_sources;
    struct image_source *image_sources;
    enum slot_mode *buffer_modes;
    enum slot_mode *vertex_modes;
    enum slot_mode *texture_modes;
    struct resource *index_source;
    struct ngpu_buffer *index_buffer;
    enum ngpu_format index_format;
    int updated;
    int need_pipeline_recreation;
    struct ngpu_pgcraft_texture_infos texture_infos;
};

struct pipeline *ngli_pipeline_create(struct ngpu_ctx *gpu_ctx)
{
    struct pipeline *s = ngli_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->gpu_ctx = gpu_ctx;
    return s;
}

static void free_bindgroup(void *user_arg, void *data)
{
    struct ngpu_bindgroup **bindgroup = data;
    ngpu_bindgroup_freep(bindgroup);
}

static int grow_bindgroup_array(struct pipeline *s)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    size_t count = s->bindgroups.count;
    if (count == 0) {
        ngli_darray_set_free_func(&s->bindgroups, free_bindgroup, NULL);
        count = NB_BINDGROUPS;
    }

    for (size_t i = 0; i < count; i++) {
        struct ngpu_bindgroup *bindgroup = ngpu_bindgroup_create(gpu_ctx);
        if (!bindgroup)
            return NGL_ERROR_MEMORY;

        struct ngpu_bindgroup_params params = {
            .layout    = s->bindgroup_layout,
            .resources = {
                .textures    = s->textures,
                .nb_textures = s->nb_textures,
                .buffers     = s->buffers,
                .nb_buffers  = s->nb_buffers,
            },
        };

        int ret = ngpu_bindgroup_init(bindgroup, &params);
        if (ret < 0) {
            ngpu_bindgroup_freep(&bindgroup);
            return ret;
        }

        if (ngli_darray_try_push(&s->bindgroups, bindgroup) < 0) {
            ngpu_bindgroup_freep(&bindgroup);
            return NGL_ERROR_MEMORY;
        }
    }

    return 0;
}

static int create_pipeline(struct pipeline *s)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    s->bindgroup_layout = ngpu_bindgroup_layout_create(gpu_ctx);
    if (!s->bindgroup_layout)
        return NGL_ERROR_MEMORY;

    int ret = ngpu_bindgroup_layout_init(s->bindgroup_layout, &s->bindgroup_layout_desc);
    if (ret < 0)
        return ret;

    s->gpu_pipeline = ngpu_pipeline_create(gpu_ctx);
    if (!s->gpu_pipeline)
        return NGL_ERROR_MEMORY;

    const struct ngpu_pipeline_params pipeline_params = {
        .type     = s->type,
        .graphics = s->graphics,
        .program  = s->program,
        .layout   = {
            .bindgroup_layout = s->bindgroup_layout,
        }
    };

    ret = ngpu_pipeline_init(s->gpu_pipeline, &pipeline_params);
    if (ret < 0)
        return ret;

    ret = grow_bindgroup_array(s);
    if (ret < 0)
        return ret;

    s->cur_bindgroup_index = 0;

    /* Initialize bindgroup before first pipeline execution */
    s->updated = 1;

    return 0;
}

static void reset_pipeline(struct pipeline *s)
{
    ngpu_pipeline_freep(&s->gpu_pipeline);
    ngli_darray_clear(&s->bindgroups);
    s->cur_bindgroup = NULL;
    s->cur_bindgroup_index = 0;
    ngpu_bindgroup_layout_freep(&s->bindgroup_layout);
}

int ngli_pipeline_init(struct pipeline *s, const struct pipeline_params *params)
{
    s->type = params->type;

    int ret = ngpu_pipeline_graphics_copy(&s->graphics, &params->graphics);
    if (ret < 0)
        return ret;

    s->program = params->program;

    NGLI_ARRAY_MEMDUP(&s->bindgroup_layout_desc, &params->layout_desc, textures);
    NGLI_ARRAY_MEMDUP(&s->bindgroup_layout_desc, &params->layout_desc, buffers);

    s->nb_buffers = params->layout_desc.nb_buffers;
    s->nb_textures = params->layout_desc.nb_textures;
    s->nb_vertex_buffers = params->graphics.vertex_state.nb_buffers;
    if (s->nb_buffers) {
        s->buffers = ngli_try_calloc(s->nb_buffers, sizeof(*s->buffers));
        if (!s->buffers)
            return NGL_ERROR_MEMORY;
        s->buffer_sources = ngli_try_calloc(s->nb_buffers, sizeof(*s->buffer_sources));
        s->buffer_modes = ngli_try_calloc(s->nb_buffers, sizeof(*s->buffer_modes));
        if (!s->buffer_sources || !s->buffer_modes)
            return NGL_ERROR_MEMORY;
    }
    if (s->nb_textures) {
        s->textures = ngli_try_calloc(s->nb_textures, sizeof(*s->textures));
        if (!s->textures)
            return NGL_ERROR_MEMORY;
        s->empty_textures = ngli_try_calloc(s->nb_textures, sizeof(*s->empty_textures));
        if (!s->empty_textures)
            return NGL_ERROR_MEMORY;
        s->texture_sources = ngli_try_calloc(s->nb_textures, sizeof(*s->texture_sources));
        s->texture_modes = ngli_try_calloc(s->nb_textures, sizeof(*s->texture_modes));
        if (!s->texture_sources || !s->texture_modes)
            return NGL_ERROR_MEMORY;
    }
    if (s->nb_vertex_buffers) {
        s->vertex_buffers = ngli_try_calloc(s->nb_vertex_buffers, sizeof(*s->vertex_buffers));
        if (!s->vertex_buffers)
            return NGL_ERROR_MEMORY;
        s->vertex_sources = ngli_try_calloc(s->nb_vertex_buffers, sizeof(*s->vertex_sources));
        s->vertex_modes = ngli_try_calloc(s->nb_vertex_buffers, sizeof(*s->vertex_modes));
        if (!s->vertex_sources || !s->vertex_modes)
            return NGL_ERROR_MEMORY;
    }

    s->texture_infos = params->texture_infos;
    if (s->texture_infos.nb_infos) {
        s->image_sources = ngli_try_calloc(s->texture_infos.nb_infos, sizeof(*s->image_sources));
        if (!s->image_sources)
            return NGL_ERROR_MEMORY;
    }

    ret = create_pipeline(s);
    if (ret < 0)
        return ret;

    return 0;
}

static int apply_vertex_buffer(struct pipeline *s, int32_t index, const struct ngpu_buffer *buffer)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;

    if (index < 0 || index >= s->nb_vertex_buffers)
        return NGL_ERROR_INVALID_ARG;
    if (s->vertex_buffers[index] == buffer)
        return 0;
    struct ngpu_buffer *ref = ngpu_buffer_ref(buffer);
    ngpu_buffer_freep(&s->vertex_buffers[index]);
    s->vertex_buffers[index] = ref;
    return 0;
}

static int update_texture(struct pipeline *s, int32_t index, const struct ngpu_texture_binding *binding)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;

    if (index < 0 || index >= s->nb_textures)
        return NGL_ERROR_INVALID_ARG;
    const struct ngpu_texture_binding *previous = &s->textures[index];
    if (previous->texture == binding->texture &&
        previous->immutable_sampler == binding->immutable_sampler)
        return 0;

    if (s->textures[index].immutable_sampler != binding->immutable_sampler) {
        struct ngpu_bindgroup_layout_entry *entry = &s->bindgroup_layout_desc.textures[index];
        entry->immutable_sampler = binding->immutable_sampler;
        s->need_pipeline_recreation = 1;
    }

    /* Imported samplers belong to their texture wrappers. Retaining the texture
     * keeps the sampler valid while desired bindings outlive a bindgroup/layout. */
    struct ngpu_texture *ref = ngpu_texture_ref(binding->texture);
    struct ngpu_texture *old = (struct ngpu_texture *)s->textures[index].texture;
    ngpu_texture_freep(&old);
    s->textures[index] = *binding;
    s->textures[index].texture = ref;
    s->updated = 1;

    return 0;
}

int ngli_pipeline_update_texture(struct pipeline *s, int32_t index, const struct ngpu_texture *texture)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_textures)
        return NGL_ERROR_INVALID_ARG;
    if (s->texture_modes[index] == SLOT_SOURCE || s->texture_modes[index] == SLOT_IMAGE)
        return NGL_ERROR_INVALID_USAGE;
    s->texture_modes[index] = SLOT_DIRECT;
    const struct ngpu_texture_binding binding = {.texture = texture};
    return update_texture(s, index, &binding);
}

int ngli_pipeline_update_dynamic_offsets(struct pipeline *s, const uint32_t *offsets, size_t nb_offsets)
{
    if (nb_offsets > NGPU_MAX_DYNAMIC_OFFSETS || (nb_offsets && !offsets))
        return NGL_ERROR_INVALID_ARG;
    memcpy(s->dynamic_offsets, offsets, nb_offsets * sizeof(*s->dynamic_offsets));
    s->nb_dynamic_offsets = nb_offsets;
    return 0;
}

static int apply_buffer(struct pipeline *s, int32_t index, const struct ngpu_buffer *buffer, size_t offset, size_t size);

static int push_texture_info_block(struct pipeline *s,
                                    struct ngpu_staging_buffer *staging,
                                    size_t tex_index, const struct image *image,
                                    const void *coord_matrix_override)
{
    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[tex_index];
    if (info->block_index < 0)
        return 0;

    struct ngpu_pgcraft_texture_info_block texture_info = {0};
    const void *coord_src = coord_matrix_override ? coord_matrix_override : image->coordinates_matrix.m;
    memcpy(texture_info.coord_matrix, coord_src, sizeof(texture_info.coord_matrix));
    memcpy(texture_info.color_matrix, image->color_matrix.m, sizeof(texture_info.color_matrix));
    memcpy(texture_info.mapping_color_matrix, image->mapping_color_matrix.m, sizeof(texture_info.mapping_color_matrix));
    if (image->params.layout) {
        texture_info.dimensions[0] = (float)image->params.width;
        texture_info.dimensions[1] = (float)image->params.height;
    }
    texture_info.timestamp = image->ts;
    texture_info.sampling_mode = (int32_t)image->params.layout;

    const size_t offset = ngpu_staging_buffer_push(staging, &texture_info, sizeof(texture_info));
    struct ngpu_buffer *buffer = ngpu_staging_buffer_get_buffer(staging);
    return apply_buffer(s, info->block_index, buffer, offset, sizeof(texture_info));
}

static int apply_reframing_matrix(struct pipeline *s, int32_t index,
                                                 const struct image *image, const float *reframing,
                                                 struct ngpu_staging_buffer *staging)
{
    if (index == -1)
        return 0;

    ngli_assert(index >= 0 && index < s->texture_infos.nb_infos);
    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[index];

    /* No texture metadata block means no reframing to apply */
    if (info->block_index < 0)
        return 0;

    /* Scale up from normalized [0,1] UV to centered [-1,1], swapping y-axis */
    static const struct ngli_mat4 remap_uv_to_centered = {.m = {
        2.f,  0.f, 0.f, 0.f,
        0.f, -2.f, 0.f, 0.f,
        0.f,  0.f, 1.f, 0.f,
       -1.f,  1.f, 0.f, 1.f,
    }};

    /* Scale down from centered [-1,1] to normalized [0,1] UV, swapping y-axis */
    static const struct ngli_mat4 remap_centered_to_uv = {.m = {
        .5f,  0.f, 0.f, 0.f,
        0.f, -.5f, 0.f, 0.f,
        0.f,  0.f, 1.f, 0.f,
        .5f,  .5f, 0.f, 1.f,
    }};

    struct ngli_mat4 inverse_reframing;
    ngli_mat4_inverse(inverse_reframing.m, reframing);

    struct ngli_mat4 matrix;
    ngli_mat4_mul(matrix.m, remap_uv_to_centered.m, image->coordinates_matrix.m);
    ngli_mat4_mul(matrix.m, inverse_reframing.m, matrix.m);
    ngli_mat4_mul(matrix.m, remap_centered_to_uv.m, matrix.m);

    return push_texture_info_block(s, staging, (size_t)index, image, matrix.m);
}

static void update_image_bindings(struct pipeline *s, int32_t index,
                                  const struct image *image,
                                  const struct ngpu_texture *empty_texture)
{
    if (index == -1)
        return;

    ngli_assert(index >= 0 && index < s->texture_infos.nb_infos);
    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[index];

    const struct ngpu_texture_binding empty_binding = {0};

    switch (image->params.layout) {
    case NGLI_IMAGE_LAYOUT_DEFAULT:
        update_texture(s, info->sampler_index,        &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_1_index,      &empty_binding);
        update_texture(s, info->sampler_2_index,      &empty_binding);
        update_texture(s, info->sampler_oes_index,    &empty_binding);
        update_texture(s, info->sampler_rect_0_index, &empty_binding);
        update_texture(s, info->sampler_rect_1_index, &empty_binding);
        break;
    case NGLI_IMAGE_LAYOUT_MEDIACODEC:
        update_texture(s, info->sampler_index,        &empty_binding);
        update_texture(s, info->sampler_1_index,      &empty_binding);
        update_texture(s, info->sampler_2_index,      &empty_binding);
        update_texture(s, info->sampler_oes_index,    &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_rect_0_index, &empty_binding);
        update_texture(s, info->sampler_rect_1_index, &empty_binding);
        break;
    case NGLI_IMAGE_LAYOUT_NV12:
        update_texture(s, info->sampler_index,        &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_1_index,      &(struct ngpu_texture_binding){.texture = image->planes[1], .immutable_sampler = image->samplers[1]});
        update_texture(s, info->sampler_2_index,      &empty_binding);
        update_texture(s, info->sampler_oes_index,    &empty_binding);
        update_texture(s, info->sampler_rect_0_index, &empty_binding);
        update_texture(s, info->sampler_rect_1_index, &empty_binding);
        break;
    case NGLI_IMAGE_LAYOUT_NV12_RECTANGLE:
        update_texture(s, info->sampler_index,        &empty_binding);
        update_texture(s, info->sampler_1_index,      &empty_binding);
        update_texture(s, info->sampler_2_index,      &empty_binding);
        update_texture(s, info->sampler_oes_index,    &empty_binding);
        update_texture(s, info->sampler_rect_0_index, &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_rect_1_index, &(struct ngpu_texture_binding){.texture = image->planes[1], .immutable_sampler = image->samplers[1]});
        break;
    case NGLI_IMAGE_LAYOUT_YUV:
        update_texture(s, info->sampler_index,        &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_1_index,      &(struct ngpu_texture_binding){.texture = image->planes[1], .immutable_sampler = image->samplers[1]});
        update_texture(s, info->sampler_2_index,      &(struct ngpu_texture_binding){.texture = image->planes[2], .immutable_sampler = image->samplers[2]});
        update_texture(s, info->sampler_oes_index,    &empty_binding);
        update_texture(s, info->sampler_rect_0_index, &empty_binding);
        update_texture(s, info->sampler_rect_1_index, &empty_binding);
        break;
    case NGLI_IMAGE_LAYOUT_RECTANGLE:
        update_texture(s, info->sampler_index,        &empty_binding);
        update_texture(s, info->sampler_1_index,      &empty_binding);
        update_texture(s, info->sampler_2_index,      &empty_binding);
        update_texture(s, info->sampler_oes_index,    &empty_binding);
        update_texture(s, info->sampler_rect_0_index, &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_rect_1_index, &empty_binding);
        break;
    default:
        update_texture(s, info->sampler_index,        &(struct ngpu_texture_binding){.texture = empty_texture});
        update_texture(s, info->sampler_1_index,      &empty_binding);
        update_texture(s, info->sampler_2_index,      &empty_binding);
        update_texture(s, info->sampler_oes_index,    &empty_binding);
        update_texture(s, info->sampler_rect_0_index, &empty_binding);
        update_texture(s, info->sampler_rect_1_index, &empty_binding);
        break;
    }
}

static int prepare_empty_image(struct pipeline *s, int32_t image_index)
{
    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[image_index];
    const int32_t index = info->sampler_index;
    if (index < 0)
        return 0;
    if (!s->empty_textures[index]) {
        const enum ngpu_type type = s->bindgroup_layout_desc.textures[index].type;
        enum ngpu_texture_type texture_type;
        switch (type) {
        case NGPU_TYPE_SAMPLER_2D:       texture_type = NGPU_TEXTURE_TYPE_2D;       break;
        case NGPU_TYPE_SAMPLER_2D_ARRAY: texture_type = NGPU_TEXTURE_TYPE_2D_ARRAY; break;
        case NGPU_TYPE_SAMPLER_3D:       texture_type = NGPU_TEXTURE_TYPE_3D;       break;
        case NGPU_TYPE_SAMPLER_CUBE:     texture_type = NGPU_TEXTURE_TYPE_CUBE;     break;
        default: return NGL_ERROR_INVALID_USAGE; /* Storage images are required. */
        }
        struct ngpu_texture *texture = ngpu_texture_create(s->gpu_ctx);
        if (!texture)
            return NGL_ERROR_MEMORY;
        const struct ngpu_texture_params params = {
            .type = texture_type,
            .format = NGPU_FORMAT_R8G8B8A8_UNORM,
            .width = 1, .height = 1, .depth = 1,
            .usage = NGPU_TEXTURE_USAGE_SAMPLED_BIT | NGPU_TEXTURE_USAGE_TRANSFER_DST_BIT,
        };
        int ret = ngpu_texture_init(texture, &params);
        const uint8_t pixels[6 * 4] = {0};
        if (ret >= 0)
            ret = ngpu_texture_upload(texture, pixels, 0);
        if (ret < 0) {
            ngpu_texture_freep(&texture);
            return ret;
        }
        s->empty_textures[index] = texture;
    }
    return 0;
}

static int refresh_image(struct pipeline *s, int32_t index, const struct image *image,
                          struct ngl_node *reframing_node, struct ngpu_staging_buffer *staging)
{
    if (image->params.layout < NGLI_IMAGE_LAYOUT_NONE || image->params.layout >= NGLI_NB_IMAGE_LAYOUTS)
        return NGL_ERROR_INVALID_DATA;
    const struct ngpu_texture *empty_texture = NULL;
    if (image->params.layout == NGLI_IMAGE_LAYOUT_NONE) {
        int ret = prepare_empty_image(s, index);
        if (ret < 0)
            return ret;
        const int32_t sampler_index = s->texture_infos.infos[index].sampler_index;
        if (sampler_index >= 0)
            empty_texture = s->empty_textures[sampler_index];
    }
    update_image_bindings(s, index, image, empty_texture);
    if (reframing_node) {
        struct ngli_mat4 matrix;
        ngli_transform_chain_compute(reframing_node, matrix.m);
        return apply_reframing_matrix(s, index, image, matrix.m, staging);
    }
    return push_texture_info_block(s, staging, (size_t)index, image, NULL);
}

static int apply_buffer(struct pipeline *s, int32_t index, const struct ngpu_buffer *buffer, size_t offset, size_t size)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;

    if (index < 0 || index >= s->nb_buffers)
        return NGL_ERROR_INVALID_ARG;
    if (!buffer)
        return NGL_ERROR_INVALID_USAGE;
    const size_t allocation_size = ngpu_buffer_get_size(buffer);
    if (offset > allocation_size)
        return NGL_ERROR_INVALID_ARG;
    if (size == NGPU_BUFFER_WHOLE_SIZE)
        size = allocation_size - offset;
    else if (!size)
        size = allocation_size; /* Legacy direct-update convention */
    if (!size || size > allocation_size - offset)
        return NGL_ERROR_INVALID_ARG;
    const struct ngpu_buffer_binding *previous = &s->buffers[index];
    if (previous->buffer == buffer && previous->offset == offset && previous->size == size)
        return 0;
    struct ngpu_buffer *ref = ngpu_buffer_ref(buffer);
    struct ngpu_buffer *old = (struct ngpu_buffer *)previous->buffer;
    ngpu_buffer_freep(&old);
    s->buffers[index] = (struct ngpu_buffer_binding) {
        .buffer = ref,
        .offset = offset,
        .size   = size,
    };
    s->updated = 1;
    return 0;
}

int ngli_pipeline_update_vertex_buffer(struct pipeline *s, int32_t index, const struct ngpu_buffer *buffer)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_vertex_buffers)
        return NGL_ERROR_INVALID_ARG;
    if (s->vertex_modes[index] == SLOT_SOURCE)
        return NGL_ERROR_INVALID_USAGE;
    int ret = apply_vertex_buffer(s, index, buffer);
    if (ret < 0)
        return ret;
    s->vertex_modes[index] = SLOT_DIRECT;
    return 0;
}

int ngli_pipeline_update_buffer(struct pipeline *s, int32_t index, const struct ngpu_buffer *buffer, size_t offset, size_t size)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_buffers)
        return NGL_ERROR_INVALID_ARG;
    if (s->buffer_modes[index] == SLOT_SOURCE || s->buffer_modes[index] == SLOT_IMAGE)
        return NGL_ERROR_INVALID_USAGE;
    int ret = apply_buffer(s, index, buffer, offset, size);
    if (ret < 0)
        return ret;
    s->buffer_modes[index] = SLOT_DIRECT;
    return 0;
}

int ngli_pipeline_set_buffer_source(struct pipeline *s, int32_t index, const struct pipeline_buffer_source *source)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_buffers)
        return NGL_ERROR_INVALID_ARG;
    if (s->buffer_modes[index] == SLOT_IMAGE || s->buffer_modes[index] == SLOT_DIRECT)
        return NGL_ERROR_INVALID_USAGE;
    if (source && (!source->resource || !source->size ||
                   ngli_resource_get_type(source->resource) != NGLI_RESOURCE_BUFFER))
        return NGL_ERROR_INVALID_ARG;
    const struct pipeline_buffer_source next = source ? *source : (struct pipeline_buffer_source){0};
    struct resource *ref = ngli_resource_ref(next.resource);
    ngli_resource_freep(&s->buffer_sources[index].resource);
    s->buffer_sources[index] = next;
    s->buffer_sources[index].resource = ref;
    s->buffer_modes[index] = source ? SLOT_SOURCE : SLOT_UNBOUND;
    return 0;
}

int ngli_pipeline_set_vertex_source(struct pipeline *s, int32_t index, const struct resource *resource)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_vertex_buffers)
        return NGL_ERROR_INVALID_ARG;
    if (s->vertex_modes[index] == SLOT_DIRECT)
        return NGL_ERROR_INVALID_USAGE;
    if (resource && ngli_resource_get_type(resource) != NGLI_RESOURCE_BUFFER)
        return NGL_ERROR_INVALID_ARG;
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&s->vertex_sources[index]);
    s->vertex_sources[index] = ref;
    s->vertex_modes[index] = resource ? SLOT_SOURCE : SLOT_UNBOUND;
    return 0;
}

int ngli_pipeline_set_texture_source(struct pipeline *s, int32_t index, const struct resource *resource)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_textures)
        return NGL_ERROR_INVALID_ARG;
    if (s->texture_modes[index] == SLOT_IMAGE || s->texture_modes[index] == SLOT_DIRECT)
        return NGL_ERROR_INVALID_USAGE;
    if (resource && ngli_resource_get_type(resource) != NGLI_RESOURCE_TEXTURE)
        return NGL_ERROR_INVALID_ARG;
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&s->texture_sources[index]);
    s->texture_sources[index] = ref;
    s->texture_modes[index] = resource ? SLOT_SOURCE : SLOT_UNBOUND;
    return 0;
}

static void apply_index_buffer(struct pipeline *s, const struct ngpu_buffer *buffer, enum ngpu_format format)
{
    if (s->index_buffer != buffer) {
        struct ngpu_buffer *ref = ngpu_buffer_ref(buffer);
        ngpu_buffer_freep(&s->index_buffer);
        s->index_buffer = ref;
    }
    s->index_format = format;
}

int ngli_pipeline_update_index_buffer(struct pipeline *s, const struct ngpu_buffer *buffer, enum ngpu_format format)
{
    if (s->index_source)
        return NGL_ERROR_INVALID_USAGE;
    apply_index_buffer(s, buffer, format);
    return 0;
}

int ngli_pipeline_set_index_source(struct pipeline *s, const struct resource *resource, enum ngpu_format format)
{
    if (!s->index_source && s->index_buffer)
        return NGL_ERROR_INVALID_USAGE;
    if (resource && ngli_resource_get_type(resource) != NGLI_RESOURCE_BUFFER)
        return NGL_ERROR_INVALID_ARG;
    if (!resource)
        apply_index_buffer(s, NULL, format);
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&s->index_source);
    s->index_source = ref;
    s->index_format = format;
    return 0;
}

static void get_image_texture_indices(const struct ngpu_pgcraft_texture_info *info, int32_t indices[6])
{
    indices[0] = info->sampler_index;
    indices[1] = info->sampler_1_index;
    indices[2] = info->sampler_2_index;
    indices[3] = info->sampler_oes_index;
    indices[4] = info->sampler_rect_0_index;
    indices[5] = info->sampler_rect_1_index;
}

int ngli_pipeline_set_image_source(struct pipeline *s, int32_t image_index,
                                   const struct resource *resource,
                                   struct ngl_node *reframing_node)
{
    if (image_index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (image_index < 0 || image_index >= s->texture_infos.nb_infos)
        return NGL_ERROR_INVALID_ARG;
    if (resource && ngli_resource_get_type(resource) != NGLI_RESOURCE_IMAGE)
        return NGL_ERROR_INVALID_ARG;

    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[image_index];
    const struct image_source *previous = &s->image_sources[image_index];
    const enum slot_mode expected = previous->resource ? SLOT_IMAGE : SLOT_UNBOUND;
    int32_t indices[6];
    get_image_texture_indices(info, indices);
    int used = info->block_index >= 0;
    if (info->block_index >= 0 && (info->block_index >= s->nb_buffers || s->buffer_modes[info->block_index] != expected))
        return NGL_ERROR_INVALID_USAGE;
    for (size_t i = 0; i < NGLI_ARRAY_NB(indices); i++) {
        const int32_t index = indices[i];
        if (index < 0)
            continue;
        if (index >= s->nb_textures || s->texture_modes[index] != expected)
            return NGL_ERROR_INVALID_USAGE;
        used = 1;
    }
    if (!used)
        return NGL_ERROR_NOT_FOUND;

    const enum slot_mode mode = resource ? SLOT_IMAGE : SLOT_UNBOUND;
    if (info->block_index >= 0)
        s->buffer_modes[info->block_index] = mode;
    for (size_t i = 0; i < NGLI_ARRAY_NB(indices); i++)
        if (indices[i] >= 0)
            s->texture_modes[indices[i]] = mode;
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&s->image_sources[image_index].resource);
    s->image_sources[image_index] = (struct image_source){
        .resource = ref,
        .reframing_node = resource ? reframing_node : NULL,
    };
    return 0;
}

static int validate_buffer(struct pipeline *s, size_t index, size_t dynamic_offset)
{
    const struct ngpu_buffer_binding *binding = &s->buffers[index];
    const struct ngpu_bindgroup_layout_entry *entry = &s->bindgroup_layout_desc.buffers[index];
    if (s->buffer_modes[index] == SLOT_UNBOUND || !binding->buffer)
        return NGL_ERROR_INVALID_USAGE;
    const size_t size = ngpu_buffer_get_size(binding->buffer);
    if (binding->offset > size || dynamic_offset > size - binding->offset ||
        !binding->size || binding->size > size - binding->offset - dynamic_offset)
        return NGL_ERROR_INVALID_ARG;
    const int uniform = entry->type == NGPU_TYPE_UNIFORM_BUFFER || entry->type == NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC;
    const uint32_t usage = uniform ? NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT : NGPU_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!(ngpu_buffer_get_usage(binding->buffer) & usage))
        return NGL_ERROR_INVALID_USAGE;
    const struct ngpu_limits *limits = ngpu_ctx_get_limits(s->gpu_ctx);
    const size_t alignment = uniform ? limits->min_uniform_block_offset_alignment : limits->min_storage_block_offset_alignment;
    if (alignment && (binding->offset % alignment || dynamic_offset % alignment))
        return NGL_ERROR_INVALID_ARG;
    const size_t max_size = uniform ? limits->max_uniform_block_size : limits->max_storage_block_size;
    if (binding->size > max_size)
        return NGL_ERROR_GRAPHICS_LIMIT_EXCEEDED;
    return 0;
}

static int validate_texture(struct pipeline *s, size_t index)
{
    const struct ngpu_texture *texture = s->textures[index].texture;
    const struct ngpu_bindgroup_layout_entry *entry = &s->bindgroup_layout_desc.textures[index];
    const enum ngpu_type type = entry->type;
    const int storage = type == NGPU_TYPE_IMAGE_2D || type == NGPU_TYPE_IMAGE_2D_ARRAY ||
                        type == NGPU_TYPE_IMAGE_3D || type == NGPU_TYPE_IMAGE_CUBE;
    if (s->texture_modes[index] == SLOT_UNBOUND ||
        (!texture && (storage || s->texture_modes[index] != SLOT_IMAGE)))
        return NGL_ERROR_INVALID_USAGE;
    /* Inactive sampling variants use the backend's empty texture fallback. */
    if (!texture)
        return 0;
    const struct ngpu_texture_params *params = ngpu_texture_get_params(texture);
    const uint32_t usage = storage ? NGPU_TEXTURE_USAGE_STORAGE_BIT : NGPU_TEXTURE_USAGE_SAMPLED_BIT;
    if (storage && entry->format != NGPU_FORMAT_UNDEFINED && params->format != entry->format)
        return NGL_ERROR_INVALID_USAGE;
    if (!(params->usage & usage))
        return NGL_ERROR_INVALID_USAGE;
    enum ngpu_texture_type expected;
    switch (type) {
    case NGPU_TYPE_SAMPLER_2D_ARRAY: case NGPU_TYPE_IMAGE_2D_ARRAY: expected = NGPU_TEXTURE_TYPE_2D_ARRAY; break;
    case NGPU_TYPE_SAMPLER_3D:       case NGPU_TYPE_IMAGE_3D:       expected = NGPU_TEXTURE_TYPE_3D;       break;
    case NGPU_TYPE_SAMPLER_CUBE:     case NGPU_TYPE_IMAGE_CUBE:     expected = NGPU_TEXTURE_TYPE_CUBE;     break;
    default: expected = NGPU_TEXTURE_TYPE_2D; break;
    }
    if (params->type != expected)
        return NGL_ERROR_INVALID_USAGE;
    return 0;
}

static int refresh_sources(struct pipeline *s, const struct pipeline_execution *execution)
{
    for (size_t i = 0; i < s->nb_buffers; i++) {
        if (s->buffer_modes[i] != SLOT_SOURCE)
            continue;
        const struct pipeline_buffer_source *source = &s->buffer_sources[i];
        int ret = apply_buffer(s, (int32_t)i, ngli_resource_get_buffer(source->resource), source->offset, source->size);
        if (ret < 0)
            return ret;
    }
    for (size_t i = 0; i < s->nb_vertex_buffers; i++) {
        if (s->vertex_modes[i] == SLOT_SOURCE)
            apply_vertex_buffer(s, (int32_t)i, ngli_resource_get_buffer(s->vertex_sources[i]));
        const struct ngpu_buffer *buffer = s->vertex_buffers[i];
        if (s->vertex_modes[i] == SLOT_UNBOUND || !buffer || !ngpu_buffer_get_size(buffer) ||
            !(ngpu_buffer_get_usage(buffer) & NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT))
            return NGL_ERROR_INVALID_USAGE;
        const struct ngpu_vertex_buffer_layout *layout = &s->graphics.vertex_state.buffers[i];
        const size_t size = ngpu_buffer_get_size(buffer);
        for (size_t j = 0; j < layout->nb_attributes; j++) {
            const struct ngpu_vertex_attribute *attribute = &layout->attributes[j];
            const size_t attribute_size = ngpu_format_get_bytes_per_pixel(attribute->format);
            if (attribute->offset > size || attribute_size > size - attribute->offset)
                return NGL_ERROR_INVALID_ARG;
        }
    }
    if (s->index_source) {
        const struct ngpu_buffer *buffer = ngli_resource_get_buffer(s->index_source);
        if (!buffer || !ngpu_buffer_get_size(buffer) || !(ngpu_buffer_get_usage(buffer) & NGPU_BUFFER_USAGE_INDEX_BUFFER_BIT))
            return NGL_ERROR_INVALID_USAGE;
        apply_index_buffer(s, buffer, s->index_format);
    }
    for (size_t i = 0; i < s->nb_textures; i++) {
        if (s->texture_modes[i] == SLOT_SOURCE) {
            const struct ngpu_texture_binding binding = {.texture = ngli_resource_get_texture(s->texture_sources[i])};
            int ret = update_texture(s, (int32_t)i, &binding);
            if (ret < 0)
                return ret;
        }
    }
    for (size_t i = 0; i < s->texture_infos.nb_infos; i++) {
        const struct image_source *source = &s->image_sources[i];
        if (!source->resource)
            continue;
        const struct image *image = ngli_resource_get_image(source->resource);
        const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[i];
        if (info->block_index >= 0 && (!execution || !execution->staging))
            return NGL_ERROR_INVALID_USAGE;
        /* Every execution uploads its own metadata, even within the same frame. */
        int ret = refresh_image(s, (int32_t)i, image, source->reframing_node, execution ? execution->staging : NULL);
        if (ret < 0)
            return ret;
    }
    size_t dynamic_index = 0;
    for (size_t i = 0; i < s->nb_buffers; i++) {
        const enum ngpu_type type = s->bindgroup_layout_desc.buffers[i].type;
        size_t dynamic_offset = 0;
        if (type == NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC || type == NGPU_TYPE_STORAGE_BUFFER_DYNAMIC) {
            if (dynamic_index >= s->nb_dynamic_offsets)
                return NGL_ERROR_INVALID_USAGE;
            dynamic_offset = s->dynamic_offsets[dynamic_index++];
        }
        int ret = validate_buffer(s, i, dynamic_offset);
        if (ret < 0)
            return ret;
    }
    if (dynamic_index != s->nb_dynamic_offsets)
        return NGL_ERROR_INVALID_ARG;
    for (size_t i = 0; i < s->nb_textures; i++) {
        int ret = validate_texture(s, i);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int select_next_available_bindgroup(struct pipeline *s, struct ngpu_bindgroup **groupp, size_t *indexp)
{
    size_t index = s->cur_bindgroup_index;
    if (s->cur_bindgroup && ngpu_bindgroup_get_refcount(s->cur_bindgroup) == 1) {
        *groupp = s->cur_bindgroup;
        *indexp = index;
        return 0;
    }

    index = s->cur_bindgroup ? (index + 1) % s->bindgroups.count : 0;
    struct ngpu_bindgroup *group = *ngli_darray_get(&s->bindgroups, index);
    if (ngpu_bindgroup_get_refcount(group) != 1) {
        index = s->bindgroups.count;
        int ret = grow_bindgroup_array(s);
        if (ret < 0)
            return ret;
        group = *ngli_darray_get(&s->bindgroups, index);
    }
    *groupp = group;
    *indexp = index;
    return 0;
}

static int prepare_bindgroup(struct pipeline *s)
{
    if (!s->updated)
        return 0;

    if (s->need_pipeline_recreation || !s->gpu_pipeline) {
        s->need_pipeline_recreation = 1;
        reset_pipeline(s);
        int ret = create_pipeline(s);
        if (ret < 0)
            return ret;
        s->need_pipeline_recreation = 0;
    }

    struct ngpu_bindgroup *group;
    size_t group_index;
    int ret = select_next_available_bindgroup(s, &group, &group_index);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < s->nb_textures; i++) {
        ret = ngpu_bindgroup_update_texture(group, (int32_t) i, &s->textures[i]);
        if (ret < 0)
            return ret;
    }

    for (size_t i = 0; i < s->nb_buffers; i++) {
        ret = ngpu_bindgroup_update_buffer(group, (int32_t) i, &s->buffers[i]);
        if (ret < 0)
            return ret;
    }

    s->cur_bindgroup = group;
    s->cur_bindgroup_index = group_index;
    s->updated = 0;
    return 0;
}

static int prepare_pipeline(struct pipeline *s, const struct pipeline_execution *execution)
{
    int ret = refresh_sources(s, execution);
    if (ret < 0) {
        LOG(ERROR, "pipeline resource resolution failed: %s", NGLI_RET_STR(ret));
        return ret;
    }
    ret = prepare_bindgroup(s);
    if (ret < 0) {
        LOG(ERROR, "pipeline binding preparation failed: %s", NGLI_RET_STR(ret));
        return ret;
    }

    return 0;
}

int ngli_pipeline_draw(struct pipeline *s, const struct pipeline_execution *execution, uint32_t nb_vertices, uint32_t nb_instances, uint32_t first_vertex)
{
    if (s->type != NGPU_PIPELINE_TYPE_GRAPHICS)
        return NGL_ERROR_INVALID_USAGE;

    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    int ret = prepare_pipeline(s, execution);
    if (ret < 0)
        return ret;

    ngpu_ctx_set_pipeline(gpu_ctx, s->gpu_pipeline);
    for (size_t i = 0; i < s->nb_vertex_buffers; i++)
        ngpu_ctx_set_vertex_buffer(gpu_ctx, (uint32_t)i, s->vertex_buffers[i]);
    ngpu_ctx_set_bindgroup(gpu_ctx, s->cur_bindgroup, s->dynamic_offsets, s->nb_dynamic_offsets);
    ngpu_ctx_draw(gpu_ctx, nb_vertices, nb_instances, first_vertex);
    return 0;
}

int ngli_pipeline_draw_indexed(struct pipeline *s, const struct pipeline_execution *execution, uint32_t nb_indices, uint32_t nb_instances)
{
    if (s->type != NGPU_PIPELINE_TYPE_GRAPHICS)
        return NGL_ERROR_INVALID_USAGE;

    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    int ret = prepare_pipeline(s, execution);
    if (ret < 0)
        return ret;

    const size_t index_size = s->index_format == NGPU_FORMAT_R16_UNORM ? 2
                            : s->index_format == NGPU_FORMAT_R32_UINT ? 4 : 0;
    if (!s->index_buffer || !index_size ||
        !(ngpu_buffer_get_usage(s->index_buffer) & NGPU_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
        nb_indices > ngpu_buffer_get_size(s->index_buffer) / index_size) {
        LOG(ERROR, "invalid pipeline index buffer");
        return NGL_ERROR_INVALID_USAGE;
    }

    ngpu_ctx_set_pipeline(gpu_ctx, s->gpu_pipeline);
    for (size_t i = 0; i < s->nb_vertex_buffers; i++)
        ngpu_ctx_set_vertex_buffer(gpu_ctx, (uint32_t)i, s->vertex_buffers[i]);
    ngpu_ctx_set_index_buffer(gpu_ctx, s->index_buffer, s->index_format);
    ngpu_ctx_set_bindgroup(gpu_ctx, s->cur_bindgroup, s->dynamic_offsets, s->nb_dynamic_offsets);
    ngpu_ctx_draw_indexed(gpu_ctx, nb_indices, nb_instances, 0);
    return 0;
}

int ngli_pipeline_dispatch(struct pipeline *s, const struct pipeline_execution *execution, uint32_t nb_group_x, uint32_t nb_group_y, uint32_t nb_group_z)
{
    if (s->type != NGPU_PIPELINE_TYPE_COMPUTE)
        return NGL_ERROR_INVALID_USAGE;

    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    int ret = prepare_pipeline(s, execution);
    if (ret < 0)
        return ret;

    ngpu_ctx_set_pipeline(gpu_ctx, s->gpu_pipeline);
    ngpu_ctx_set_bindgroup(gpu_ctx, s->cur_bindgroup, s->dynamic_offsets, s->nb_dynamic_offsets);
    ngpu_ctx_dispatch(gpu_ctx, nb_group_x, nb_group_y, nb_group_z);
    return 0;
}

void ngli_pipeline_discard_resources(struct pipeline *s)
{
    if (!s)
        return;
    reset_pipeline(s);
    for (size_t i = 0; s->vertex_buffers && i < s->nb_vertex_buffers; i++)
        ngpu_buffer_freep(&s->vertex_buffers[i]);
    for (size_t i = 0; s->buffers && i < s->nb_buffers; i++) {
        struct ngpu_buffer *buffer = (struct ngpu_buffer *)s->buffers[i].buffer;
        ngpu_buffer_freep(&buffer);
        s->buffers[i] = (struct ngpu_buffer_binding){0};
    }
    for (size_t i = 0; s->textures && i < s->nb_textures; i++) {
        struct ngpu_texture *texture = (struct ngpu_texture *)s->textures[i].texture;
        ngpu_texture_freep(&texture);
        s->textures[i] = (struct ngpu_texture_binding){0};
        s->bindgroup_layout_desc.textures[i].immutable_sampler = NULL;
    }
    ngpu_buffer_freep(&s->index_buffer);
    for (size_t i = 0; s->empty_textures && i < s->nb_textures; i++)
        ngpu_texture_freep(&s->empty_textures[i]);
    for (size_t i = 0; s->buffer_modes && i < s->nb_buffers; i++)
        if (s->buffer_modes[i] == SLOT_DIRECT)
            s->buffer_modes[i] = SLOT_UNBOUND;
    for (size_t i = 0; s->texture_modes && i < s->nb_textures; i++)
        if (s->texture_modes[i] == SLOT_DIRECT)
            s->texture_modes[i] = SLOT_UNBOUND;
    for (size_t i = 0; s->vertex_modes && i < s->nb_vertex_buffers; i++)
        if (s->vertex_modes[i] == SLOT_DIRECT)
            s->vertex_modes[i] = SLOT_UNBOUND;
    s->nb_dynamic_offsets = 0;
    s->updated = 1;
}

void ngli_pipeline_freep(struct pipeline **sp)
{
    struct pipeline *s = *sp;
    if (!s)
        return;

    ngli_pipeline_discard_resources(s);
    ngli_darray_reset(&s->bindgroups);

    ngpu_pipeline_graphics_reset(&s->graphics);

    ngli_freep(&s->bindgroup_layout_desc.textures);
    ngli_freep(&s->bindgroup_layout_desc.buffers);

    ngli_freep(&s->vertex_buffers);
    ngli_freep(&s->textures);
    ngli_freep(&s->empty_textures);
    ngli_freep(&s->buffers);

    for (size_t i = 0; s->buffer_sources && i < s->nb_buffers; i++)
        ngli_resource_freep(&s->buffer_sources[i].resource);
    for (size_t i = 0; s->vertex_sources && i < s->nb_vertex_buffers; i++)
        ngli_resource_freep(&s->vertex_sources[i]);
    for (size_t i = 0; s->texture_sources && i < s->nb_textures; i++)
        ngli_resource_freep(&s->texture_sources[i]);
    for (size_t i = 0; s->image_sources && i < s->texture_infos.nb_infos; i++)
        ngli_resource_freep(&s->image_sources[i].resource);
    ngli_resource_freep(&s->index_source);

    ngli_freep(&s->buffer_sources);
    ngli_freep(&s->vertex_sources);
    ngli_freep(&s->texture_sources);
    ngli_freep(&s->image_sources);
    ngli_freep(&s->buffer_modes);
    ngli_freep(&s->vertex_modes);
    ngli_freep(&s->texture_modes);
    ngli_freep(sp);
}
