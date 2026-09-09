/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

struct buffer_slot {
    enum slot_mode mode;
    struct pipeline_buffer_source source;
    struct ngpu_buffer_binding binding;
};

struct vertex_slot {
    enum slot_mode mode;
    struct resource *source;
    struct ngpu_buffer *buffer;
};

struct texture_slot {
    enum slot_mode mode;
    struct resource *source;
    struct ngpu_texture_binding binding;
    struct ngpu_texture *empty_texture;
};

struct index_slot {
    enum slot_mode mode;
    struct resource *source;
    struct ngpu_buffer *buffer;
    enum ngpu_format format;
};

struct ngli_pipeline {
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
    struct vertex_slot *vertex_slots;
    size_t nb_vertex_buffers;
    struct texture_slot *texture_slots;
    size_t nb_textures;
    struct buffer_slot *buffer_slots;
    size_t nb_buffers;
    uint32_t dynamic_offsets[NGPU_MAX_DYNAMIC_OFFSETS];
    size_t nb_dynamic_offsets;
    struct resource **image_sources;
    struct index_slot index_slot;
    int updated;
    int need_pipeline_recreation;
    struct ngpu_pgcraft_texture_infos texture_infos;
};

struct ngli_pipeline *ngli_pipeline_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngli_pipeline *s = ngli_try_calloc(1, sizeof(*s));
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

static int grow_bindgroup_array(struct ngli_pipeline *s)
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
            .layout = s->bindgroup_layout,
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

static int create_pipeline(struct ngli_pipeline *s)
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

static void reset_pipeline(struct ngli_pipeline *s)
{
    ngpu_pipeline_freep(&s->gpu_pipeline);
    ngli_darray_clear(&s->bindgroups);
    s->cur_bindgroup = NULL;
    s->cur_bindgroup_index = 0;
    ngpu_bindgroup_layout_freep(&s->bindgroup_layout);
}

int ngli_pipeline_init(struct ngli_pipeline *s, const struct ngli_pipeline_params *params)
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
        s->buffer_slots = ngli_try_calloc(s->nb_buffers, sizeof(*s->buffer_slots));
        if (!s->buffer_slots)
            return NGL_ERROR_MEMORY;
    }
    if (s->nb_textures) {
        s->texture_slots = ngli_try_calloc(s->nb_textures, sizeof(*s->texture_slots));
        if (!s->texture_slots)
            return NGL_ERROR_MEMORY;
    }
    if (s->nb_vertex_buffers) {
        s->vertex_slots = ngli_try_calloc(s->nb_vertex_buffers, sizeof(*s->vertex_slots));
        if (!s->vertex_slots)
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

static int apply_vertex_buffer(struct ngli_pipeline *s, int32_t index, const struct ngpu_buffer *buffer)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;

    if (index < 0 || index >= s->nb_vertex_buffers)
        return NGL_ERROR_INVALID_ARG;
    struct vertex_slot *slot = &s->vertex_slots[index];
    if (slot->buffer == buffer)
        return 0;
    struct ngpu_buffer *ref = ngpu_buffer_ref(buffer);
    ngpu_buffer_freep(&slot->buffer);
    slot->buffer = ref;
    return 0;
}

static int update_texture(struct ngli_pipeline *s, int32_t index, const struct ngpu_texture_binding *binding)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;

    if (index < 0 || index >= s->nb_textures)
        return NGL_ERROR_INVALID_ARG;
    struct texture_slot *slot = &s->texture_slots[index];
    const struct ngpu_texture_binding *previous = &slot->binding;
    if (previous->texture == binding->texture &&
        previous->immutable_sampler == binding->immutable_sampler)
        return 0;

    if (slot->binding.immutable_sampler != binding->immutable_sampler) {
        struct ngpu_bindgroup_layout_entry *entry = &s->bindgroup_layout_desc.textures[index];
        entry->immutable_sampler = binding->immutable_sampler;
        s->need_pipeline_recreation = 1;
    }

    /* Imported samplers belong to their texture wrappers. Retaining the texture
     * keeps the sampler valid while desired bindings outlive a bindgroup/layout. */
    struct ngpu_texture *ref = ngpu_texture_ref(binding->texture);
    struct ngpu_texture *old = (struct ngpu_texture *)slot->binding.texture;
    ngpu_texture_freep(&old);
    slot->binding = *binding;
    slot->binding.texture = ref;
    s->updated = 1;

    return 0;
}

int ngli_pipeline_update_texture(struct ngli_pipeline *s, int32_t index, const struct ngpu_texture *texture)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_textures)
        return NGL_ERROR_INVALID_ARG;
    struct texture_slot *slot = &s->texture_slots[index];
    if (slot->mode == SLOT_SOURCE || slot->mode == SLOT_IMAGE)
        return NGL_ERROR_INVALID_USAGE;
    slot->mode = SLOT_DIRECT;
    const struct ngpu_texture_binding binding = {.texture = texture};
    return update_texture(s, index, &binding);
}

int ngli_pipeline_update_dynamic_offsets(struct ngli_pipeline *s, const uint32_t *offsets, size_t nb_offsets)
{
    if (nb_offsets > NGPU_MAX_DYNAMIC_OFFSETS || (nb_offsets && !offsets))
        return NGL_ERROR_INVALID_ARG;
    memcpy(s->dynamic_offsets, offsets, nb_offsets * sizeof(*s->dynamic_offsets));
    s->nb_dynamic_offsets = nb_offsets;
    return 0;
}

static int apply_buffer(struct ngli_pipeline *s, int32_t index, const struct ngpu_buffer *buffer, size_t offset, size_t size);

static int push_texture_info_block(struct ngli_pipeline *s,
                                    struct ngpu_staging_buffer *staging,
                                    size_t tex_index, const struct image *image)
{
    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[tex_index];
    if (info->block_index < 0)
        return 0;

    struct ngpu_pgcraft_texture_info_block texture_info = {0};
    memcpy(texture_info.coord_matrix, image->coordinates_matrix.m, sizeof(texture_info.coord_matrix));
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

static void update_image_bindings(struct ngli_pipeline *s, int32_t index,
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

static int prepare_empty_image(struct ngli_pipeline *s, int32_t image_index)
{
    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[image_index];
    const int32_t index = info->sampler_index;
    if (index < 0)
        return 0;
    struct texture_slot *slot = &s->texture_slots[index];
    if (!slot->empty_texture) {
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
        slot->empty_texture = texture;
    }
    return 0;
}

static int refresh_image(struct ngli_pipeline *s, int32_t index, const struct image *image,
                         struct ngpu_staging_buffer *staging)
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
            empty_texture = s->texture_slots[sampler_index].empty_texture;
    }
    update_image_bindings(s, index, image, empty_texture);
    return push_texture_info_block(s, staging, (size_t)index, image);
}

static int apply_buffer(struct ngli_pipeline *s, int32_t index, const struct ngpu_buffer *buffer, size_t offset, size_t size)
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
    struct buffer_slot *slot = &s->buffer_slots[index];
    const struct ngpu_buffer_binding *previous = &slot->binding;
    if (previous->buffer == buffer && previous->offset == offset && previous->size == size)
        return 0;
    struct ngpu_buffer *ref = ngpu_buffer_ref(buffer);
    struct ngpu_buffer *old = (struct ngpu_buffer *)previous->buffer;
    ngpu_buffer_freep(&old);
    slot->binding = (struct ngpu_buffer_binding) {
        .buffer = ref,
        .offset = offset,
        .size   = size,
    };
    s->updated = 1;
    return 0;
}

int ngli_pipeline_update_vertex_buffer(struct ngli_pipeline *s, int32_t index, const struct ngpu_buffer *buffer)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_vertex_buffers)
        return NGL_ERROR_INVALID_ARG;
    struct vertex_slot *slot = &s->vertex_slots[index];
    if (slot->mode == SLOT_SOURCE)
        return NGL_ERROR_INVALID_USAGE;
    int ret = apply_vertex_buffer(s, index, buffer);
    if (ret < 0)
        return ret;
    slot->mode = SLOT_DIRECT;
    return 0;
}

int ngli_pipeline_update_buffer(struct ngli_pipeline *s, int32_t index, const struct ngpu_buffer *buffer, size_t offset, size_t size)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_buffers)
        return NGL_ERROR_INVALID_ARG;
    struct buffer_slot *slot = &s->buffer_slots[index];
    if (slot->mode == SLOT_SOURCE || slot->mode == SLOT_IMAGE)
        return NGL_ERROR_INVALID_USAGE;
    int ret = apply_buffer(s, index, buffer, offset, size);
    if (ret < 0)
        return ret;
    slot->mode = SLOT_DIRECT;
    return 0;
}

int ngli_pipeline_set_buffer_source(struct ngli_pipeline *s, int32_t index, const struct pipeline_buffer_source *source)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_buffers)
        return NGL_ERROR_INVALID_ARG;
    struct buffer_slot *slot = &s->buffer_slots[index];
    if (slot->mode == SLOT_IMAGE || slot->mode == SLOT_DIRECT)
        return NGL_ERROR_INVALID_USAGE;
    if (source && (!source->resource || !source->size ||
                   ngli_resource_get_type(source->resource) != NGLI_RESOURCE_BUFFER))
        return NGL_ERROR_INVALID_ARG;
    const struct pipeline_buffer_source next = source ? *source : (struct pipeline_buffer_source){0};
    struct resource *ref = ngli_resource_ref(next.resource);
    ngli_resource_freep(&slot->source.resource);
    slot->source = next;
    slot->source.resource = ref;
    slot->mode = source ? SLOT_SOURCE : SLOT_UNBOUND;
    return 0;
}

int ngli_pipeline_set_vertex_source(struct ngli_pipeline *s, int32_t index, const struct resource *resource)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_vertex_buffers)
        return NGL_ERROR_INVALID_ARG;
    struct vertex_slot *slot = &s->vertex_slots[index];
    if (slot->mode == SLOT_DIRECT)
        return NGL_ERROR_INVALID_USAGE;
    if (resource && ngli_resource_get_type(resource) != NGLI_RESOURCE_BUFFER)
        return NGL_ERROR_INVALID_ARG;
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&slot->source);
    slot->source = ref;
    slot->mode = resource ? SLOT_SOURCE : SLOT_UNBOUND;
    return 0;
}

int ngli_pipeline_set_texture_source(struct ngli_pipeline *s, int32_t index, const struct resource *resource)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_textures)
        return NGL_ERROR_INVALID_ARG;
    struct texture_slot *slot = &s->texture_slots[index];
    if (slot->mode == SLOT_IMAGE || slot->mode == SLOT_DIRECT)
        return NGL_ERROR_INVALID_USAGE;
    if (resource && ngli_resource_get_type(resource) != NGLI_RESOURCE_TEXTURE)
        return NGL_ERROR_INVALID_ARG;
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&slot->source);
    slot->source = ref;
    slot->mode = resource ? SLOT_SOURCE : SLOT_UNBOUND;
    return 0;
}

static void apply_index_buffer(struct ngli_pipeline *s, const struct ngpu_buffer *buffer, enum ngpu_format format)
{
    struct index_slot *slot = &s->index_slot;
    if (slot->buffer != buffer) {
        struct ngpu_buffer *ref = ngpu_buffer_ref(buffer);
        ngpu_buffer_freep(&slot->buffer);
        slot->buffer = ref;
    }
    slot->format = format;
}

int ngli_pipeline_update_index_buffer(struct ngli_pipeline *s, const struct ngpu_buffer *buffer, enum ngpu_format format)
{
    struct index_slot *slot = &s->index_slot;
    if (slot->mode == SLOT_SOURCE)
        return NGL_ERROR_INVALID_USAGE;
    apply_index_buffer(s, buffer, format);
    slot->mode = buffer ? SLOT_DIRECT : SLOT_UNBOUND;
    return 0;
}

int ngli_pipeline_set_index_source(struct ngli_pipeline *s, const struct resource *resource, enum ngpu_format format)
{
    struct index_slot *slot = &s->index_slot;
    if (slot->mode == SLOT_DIRECT)
        return NGL_ERROR_INVALID_USAGE;
    if (resource && ngli_resource_get_type(resource) != NGLI_RESOURCE_BUFFER)
        return NGL_ERROR_INVALID_ARG;
    if (!resource)
        apply_index_buffer(s, NULL, format);
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&slot->source);
    slot->source = ref;
    slot->format = format;
    slot->mode = resource ? SLOT_SOURCE : SLOT_UNBOUND;
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

int ngli_pipeline_set_image_source(struct ngli_pipeline *s, int32_t image_index,
                                   const struct resource *resource)
{
    if (image_index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (image_index < 0 || image_index >= s->texture_infos.nb_infos)
        return NGL_ERROR_INVALID_ARG;
    if (resource && ngli_resource_get_type(resource) != NGLI_RESOURCE_IMAGE)
        return NGL_ERROR_INVALID_ARG;

    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[image_index];
    const enum slot_mode expected = s->image_sources[image_index] ? SLOT_IMAGE : SLOT_UNBOUND;
    int32_t indices[6];
    get_image_texture_indices(info, indices);
    int used = info->block_index >= 0;
    if (info->block_index >= 0 &&
        (info->block_index >= s->nb_buffers || s->buffer_slots[info->block_index].mode != expected))
        return NGL_ERROR_INVALID_USAGE;
    for (size_t i = 0; i < NGLI_ARRAY_NB(indices); i++) {
        const int32_t index = indices[i];
        if (index < 0)
            continue;
        if (index >= s->nb_textures || s->texture_slots[index].mode != expected)
            return NGL_ERROR_INVALID_USAGE;
        used = 1;
    }
    if (!used)
        return NGL_ERROR_NOT_FOUND;

    const enum slot_mode mode = resource ? SLOT_IMAGE : SLOT_UNBOUND;
    if (info->block_index >= 0)
        s->buffer_slots[info->block_index].mode = mode;
    for (size_t i = 0; i < NGLI_ARRAY_NB(indices); i++)
        if (indices[i] >= 0)
            s->texture_slots[indices[i]].mode = mode;
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&s->image_sources[image_index]);
    s->image_sources[image_index] = ref;
    return 0;
}

static int validate_buffer(struct ngli_pipeline *s, size_t index, size_t dynamic_offset)
{
    const struct buffer_slot *slot = &s->buffer_slots[index];
    const struct ngpu_buffer_binding *binding = &slot->binding;
    const struct ngpu_bindgroup_layout_entry *entry = &s->bindgroup_layout_desc.buffers[index];
    if (slot->mode == SLOT_UNBOUND || !binding->buffer)
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

static int validate_texture(struct ngli_pipeline *s, size_t index)
{
    const struct texture_slot *slot = &s->texture_slots[index];
    const struct ngpu_texture *texture = slot->binding.texture;
    const struct ngpu_bindgroup_layout_entry *entry = &s->bindgroup_layout_desc.textures[index];
    const enum ngpu_type type = entry->type;
    const int storage = type == NGPU_TYPE_IMAGE_2D || type == NGPU_TYPE_IMAGE_2D_ARRAY ||
                        type == NGPU_TYPE_IMAGE_3D || type == NGPU_TYPE_IMAGE_CUBE;
    if (slot->mode == SLOT_UNBOUND ||
        (!texture && (storage || slot->mode != SLOT_IMAGE)))
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

static int refresh_sources(struct ngli_pipeline *s, const struct pipeline_execution *execution)
{
    int ret;

    for (size_t i = 0; i < s->nb_buffers; i++) {
        const struct buffer_slot *slot = &s->buffer_slots[i];
        if (slot->mode != SLOT_SOURCE)
            continue;
        const struct pipeline_buffer_source *source = &slot->source;
        ret = apply_buffer(s, (int32_t)i, ngli_resource_get_buffer(source->resource), source->offset, source->size);
        if (ret < 0)
            return ret;
    }
    for (size_t i = 0; i < s->nb_vertex_buffers; i++) {
        const struct vertex_slot *slot = &s->vertex_slots[i];
        if (slot->mode == SLOT_SOURCE)
            apply_vertex_buffer(s, (int32_t)i, ngli_resource_get_buffer(slot->source));
        const struct ngpu_buffer *buffer = slot->buffer;
        if (slot->mode == SLOT_UNBOUND || !buffer || !ngpu_buffer_get_size(buffer) ||
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
    struct index_slot *index_slot = &s->index_slot;
    if (index_slot->mode == SLOT_SOURCE) {
        const struct ngpu_buffer *buffer = ngli_resource_get_buffer(index_slot->source);
        if (!buffer || !ngpu_buffer_get_size(buffer) || !(ngpu_buffer_get_usage(buffer) & NGPU_BUFFER_USAGE_INDEX_BUFFER_BIT))
            return NGL_ERROR_INVALID_USAGE;
        apply_index_buffer(s, buffer, index_slot->format);
    }
    for (size_t i = 0; i < s->nb_textures; i++) {
        const struct texture_slot *slot = &s->texture_slots[i];
        if (slot->mode == SLOT_SOURCE) {
            const struct ngpu_texture_binding binding = {.texture = ngli_resource_get_texture(slot->source)};
            ret = update_texture(s, (int32_t)i, &binding);
            if (ret < 0)
                return ret;
        }
    }
    for (size_t i = 0; i < s->texture_infos.nb_infos; i++) {
        const struct resource *source = s->image_sources[i];
        if (!source)
            continue;
        const struct image *image = ngli_resource_get_image(source);
        const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[i];
        if (info->block_index >= 0 && (!execution || !execution->staging))
            return NGL_ERROR_INVALID_USAGE;
        /* Every execution uploads its own metadata, even within the same frame. */
        ret = refresh_image(s, (int32_t)i, image, execution ? execution->staging : NULL);
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
        ret = validate_buffer(s, i, dynamic_offset);
        if (ret < 0)
            return ret;
    }
    if (dynamic_index != s->nb_dynamic_offsets)
        return NGL_ERROR_INVALID_ARG;
    for (size_t i = 0; i < s->nb_textures; i++) {
        ret = validate_texture(s, i);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int select_next_available_bindgroup(struct ngli_pipeline *s, struct ngpu_bindgroup **groupp, size_t *indexp)
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

static int prepare_bindgroup(struct ngli_pipeline *s)
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
        ret = ngpu_bindgroup_update_texture(group, (int32_t)i, &s->texture_slots[i].binding);
        if (ret < 0)
            return ret;
    }

    for (size_t i = 0; i < s->nb_buffers; i++) {
        ret = ngpu_bindgroup_update_buffer(group, (int32_t)i, &s->buffer_slots[i].binding);
        if (ret < 0)
            return ret;
    }

    s->cur_bindgroup = group;
    s->cur_bindgroup_index = group_index;
    s->updated = 0;
    return 0;
}

static int prepare_pipeline(struct ngli_pipeline *s, const struct pipeline_execution *execution)
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

int ngli_pipeline_draw(struct ngli_pipeline *s, const struct pipeline_execution *execution, uint32_t nb_vertices, uint32_t nb_instances, uint32_t first_vertex)
{
    if (s->type != NGPU_PIPELINE_TYPE_GRAPHICS)
        return NGL_ERROR_INVALID_USAGE;

    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    int ret = prepare_pipeline(s, execution);
    if (ret < 0)
        return ret;

    ngpu_ctx_set_pipeline(gpu_ctx, s->gpu_pipeline);
    for (size_t i = 0; i < s->nb_vertex_buffers; i++)
        ngpu_ctx_set_vertex_buffer(gpu_ctx, (uint32_t)i, s->vertex_slots[i].buffer);
    ngpu_ctx_set_bindgroup(gpu_ctx, s->cur_bindgroup, s->dynamic_offsets, s->nb_dynamic_offsets);
    ngpu_ctx_draw(gpu_ctx, nb_vertices, nb_instances, first_vertex);
    return 0;
}

int ngli_pipeline_draw_indexed(struct ngli_pipeline *s, const struct pipeline_execution *execution, uint32_t nb_indices, uint32_t nb_instances)
{
    if (s->type != NGPU_PIPELINE_TYPE_GRAPHICS)
        return NGL_ERROR_INVALID_USAGE;

    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    int ret = prepare_pipeline(s, execution);
    if (ret < 0)
        return ret;

    const struct index_slot *index_slot = &s->index_slot;
    const size_t index_size = index_slot->format == NGPU_FORMAT_R16_UNORM ? 2
                            : index_slot->format == NGPU_FORMAT_R32_UINT ? 4 : 0;
    if (!index_slot->buffer || !index_size ||
        !(ngpu_buffer_get_usage(index_slot->buffer) & NGPU_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
        nb_indices > ngpu_buffer_get_size(index_slot->buffer) / index_size) {
        LOG(ERROR, "invalid pipeline index buffer");
        return NGL_ERROR_INVALID_USAGE;
    }

    ngpu_ctx_set_pipeline(gpu_ctx, s->gpu_pipeline);
    for (size_t i = 0; i < s->nb_vertex_buffers; i++)
        ngpu_ctx_set_vertex_buffer(gpu_ctx, (uint32_t)i, s->vertex_slots[i].buffer);
    ngpu_ctx_set_index_buffer(gpu_ctx, index_slot->buffer, index_slot->format);
    ngpu_ctx_set_bindgroup(gpu_ctx, s->cur_bindgroup, s->dynamic_offsets, s->nb_dynamic_offsets);
    ngpu_ctx_draw_indexed(gpu_ctx, nb_indices, nb_instances, 0);
    return 0;
}

int ngli_pipeline_dispatch(struct ngli_pipeline *s, const struct pipeline_execution *execution, uint32_t nb_group_x, uint32_t nb_group_y, uint32_t nb_group_z)
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

void ngli_pipeline_discard_resources(struct ngli_pipeline *s)
{
    if (!s)
        return;
    reset_pipeline(s);
    for (size_t i = 0; s->vertex_slots && i < s->nb_vertex_buffers; i++) {
        struct vertex_slot *slot = &s->vertex_slots[i];
        ngpu_buffer_freep(&slot->buffer);
        if (slot->mode == SLOT_DIRECT)
            slot->mode = SLOT_UNBOUND;
    }
    for (size_t i = 0; s->buffer_slots && i < s->nb_buffers; i++) {
        struct buffer_slot *slot = &s->buffer_slots[i];
        struct ngpu_buffer *buffer = (struct ngpu_buffer *)slot->binding.buffer;
        ngpu_buffer_freep(&buffer);
        slot->binding = (struct ngpu_buffer_binding){0};
        if (slot->mode == SLOT_DIRECT)
            slot->mode = SLOT_UNBOUND;
    }
    for (size_t i = 0; s->texture_slots && i < s->nb_textures; i++) {
        struct texture_slot *slot = &s->texture_slots[i];
        struct ngpu_texture *texture = (struct ngpu_texture *)slot->binding.texture;
        ngpu_texture_freep(&texture);
        slot->binding = (struct ngpu_texture_binding){0};
        ngpu_texture_freep(&slot->empty_texture);
        if (slot->mode == SLOT_DIRECT)
            slot->mode = SLOT_UNBOUND;
        s->bindgroup_layout_desc.textures[i].immutable_sampler = NULL;
    }
    ngpu_buffer_freep(&s->index_slot.buffer);
    if (s->index_slot.mode == SLOT_DIRECT)
        s->index_slot.mode = SLOT_UNBOUND;
    s->nb_dynamic_offsets = 0;
    s->updated = 1;
}

void ngli_pipeline_freep(struct ngli_pipeline **sp)
{
    struct ngli_pipeline *s = *sp;
    if (!s)
        return;

    ngli_pipeline_discard_resources(s);
    ngli_darray_reset(&s->bindgroups);

    ngpu_pipeline_graphics_reset(&s->graphics);

    ngli_freep(&s->bindgroup_layout_desc.textures);
    ngli_freep(&s->bindgroup_layout_desc.buffers);

    for (size_t i = 0; s->buffer_slots && i < s->nb_buffers; i++)
        ngli_resource_freep(&s->buffer_slots[i].source.resource);
    for (size_t i = 0; s->vertex_slots && i < s->nb_vertex_buffers; i++)
        ngli_resource_freep(&s->vertex_slots[i].source);
    for (size_t i = 0; s->texture_slots && i < s->nb_textures; i++)
        ngli_resource_freep(&s->texture_slots[i].source);
    for (size_t i = 0; s->image_sources && i < s->texture_infos.nb_infos; i++)
        ngli_resource_freep(&s->image_sources[i]);
    ngli_resource_freep(&s->index_slot.source);

    ngli_freep(&s->buffer_slots);
    ngli_freep(&s->vertex_slots);
    ngli_freep(&s->texture_slots);
    ngli_freep(&s->image_sources);
    ngli_freep(sp);
}
