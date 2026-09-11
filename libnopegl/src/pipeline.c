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
#include "utils/memory.h"
#include "utils/utils.h"

/* Two entries cover the observed recurring staging-buffer binding sets. */
#define BINDGROUP_CACHE_CAPACITY 2

enum slot_mode {
    SLOT_UNBOUND,
    SLOT_DIRECT,
    SLOT_SOURCE,
    SLOT_IMAGE,
};

struct buffer_slot {
    enum slot_mode mode;
    struct buffer_resource *source;
    size_t source_offset;
    size_t source_size;
    struct ngpu_buffer_binding binding;
    int32_t dynamic_offset_index;
    /* Binding invariants already verified, cleared whenever it changes */
    bool checked;
};

struct vertex_slot {
    enum slot_mode mode;
    struct buffer_resource *source;
    struct ngpu_buffer *buffer;
    bool checked;
};

struct texture_slot {
    enum slot_mode mode;
    struct texture_resource *source;
    struct ngpu_texture_binding binding;
    struct ngpu_texture *empty_texture;
    bool checked;
};

struct index_slot {
    enum slot_mode mode;
    struct buffer_resource *source;
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
    struct ngpu_bindgroup *cur_bindgroup;
    struct ngpu_bindgroup_cache *bindgroup_cache;
    /* Borrowed scratch arrays for immutable bindgroup creation. */
    struct ngpu_texture_binding *textures;
    struct ngpu_buffer_binding *buffers;
    struct vertex_slot *vertex_slots;
    size_t nb_vertex_buffers;
    struct texture_slot *texture_slots;
    size_t nb_textures;
    struct buffer_slot *buffer_slots;
    size_t nb_buffers;
    uint32_t dynamic_offsets[NGPU_MAX_DYNAMIC_OFFSETS];
    size_t nb_dynamic_offsets;
    struct image_resource **image_sources;
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

    /* Initialize bindgroup before first pipeline execution */
    s->updated = 1;

    return 0;
}

static void reset_pipeline(struct ngli_pipeline *s)
{
    ngpu_pipeline_freep(&s->gpu_pipeline);
    ngpu_bindgroup_cache_clear(s->bindgroup_cache);
    ngpu_bindgroup_freep(&s->cur_bindgroup);
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
        s->buffers = ngli_try_calloc(s->nb_buffers, sizeof(*s->buffers));
        if (!s->buffer_slots || !s->buffers)
            return NGL_ERROR_MEMORY;
    }
    if (s->nb_textures) {
        s->texture_slots = ngli_try_calloc(s->nb_textures, sizeof(*s->texture_slots));
        s->textures = ngli_try_calloc(s->nb_textures, sizeof(*s->textures));
        if (!s->texture_slots || !s->textures)
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

    s->nb_dynamic_offsets = ngpu_bindgroup_layout_get_nb_dynamic_offsets(s->bindgroup_layout);
    size_t dynamic_index = 0;
    for (size_t i = 0; i < s->nb_buffers; i++) {
        const enum ngpu_type type = s->bindgroup_layout_desc.buffers[i].type;
        s->buffer_slots[i].dynamic_offset_index = -1;
        if (type == NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC || type == NGPU_TYPE_STORAGE_BUFFER_DYNAMIC)
            s->buffer_slots[i].dynamic_offset_index = (int32_t)dynamic_index++;
    }
    ngli_assert(dynamic_index == s->nb_dynamic_offsets);

    /* All images share one internally managed metadata binding. */
    if (s->texture_infos.nb_metadata && s->texture_infos.block_index >= 0) {
        if (s->texture_infos.block_index >= s->nb_buffers)
            return NGL_ERROR_INVALID_ARG;
        s->buffer_slots[s->texture_infos.block_index].mode = SLOT_IMAGE;
    }
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
    slot->checked = false;
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
    slot->checked = false;
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

static int apply_buffer(struct ngli_pipeline *s, int32_t index, const struct ngpu_buffer *buffer, size_t offset, size_t size);

static int push_texture_info_block(struct ngli_pipeline *s, struct ngpu_staging_buffer *staging)
{
    if (!s->texture_infos.nb_metadata || s->texture_infos.block_index < 0)
        return 0;
    if (!staging)
        return NGL_ERROR_INVALID_USAGE;

    const size_t size = s->texture_infos.nb_metadata * sizeof(struct ngpu_pgcraft_texture_info_block);
    size_t offset = 0;
    struct ngpu_pgcraft_texture_info_block *metadata = ngpu_staging_buffer_reserve(staging, size, &offset);
    memset(metadata, 0, size);
    for (size_t i = 0; i < s->texture_infos.nb_infos; i++) {
        const int32_t index = s->texture_infos.infos[i].metadata_index;
        if (index < 0)
            continue;
        const struct image_resource *source = s->image_sources[i];
        const struct ngli_image *image = source ? ngli_image_resource_get(source) : NULL;
        const struct ngli_image_params *params = ngli_image_get_params(image);
        struct ngpu_pgcraft_texture_info_block *info = &metadata[index];
        memcpy(info->coord_matrix, params->coordinates_matrix.m, sizeof(info->coord_matrix));
        memcpy(info->color_matrix, params->color_matrix.m, sizeof(info->color_matrix));
        memcpy(info->mapping_color_matrix, params->mapping_color_matrix.m, sizeof(info->mapping_color_matrix));
        if (params->layout) {
            info->dimensions[0] = (float)params->width;
            info->dimensions[1] = (float)params->height;
        }
        info->timestamp = params->ts;
        info->sampling_mode = (int32_t)params->layout;
    }
    struct ngpu_buffer *buffer = ngpu_staging_buffer_get_buffer(staging);
    return apply_buffer(s, s->texture_infos.block_index, buffer, offset, size);
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

/*
 * Generated sampler slots, in the order returned by get_image_texture_indices().
 * Each entry lists, for one layout, the slot receiving each image plane; -1
 * terminates the list. Slots not named here are unbound for that layout.
 */
static const int8_t layout_plane_slots[NGLI_NB_IMAGE_LAYOUTS][4] = {
    [NGLI_IMAGE_LAYOUT_NONE]           = {-1, -1, -1, -1},
    [NGLI_IMAGE_LAYOUT_DEFAULT]        = { 0, -1, -1, -1},
    [NGLI_IMAGE_LAYOUT_MEDIACODEC]     = { 3, -1, -1, -1},
    [NGLI_IMAGE_LAYOUT_NV12]           = { 0,  1, -1, -1},
    [NGLI_IMAGE_LAYOUT_NV12_RECTANGLE] = { 4,  5, -1, -1},
    [NGLI_IMAGE_LAYOUT_YUV]            = { 0,  1,  2, -1},
    [NGLI_IMAGE_LAYOUT_RECTANGLE]      = { 4, -1, -1, -1},
};

static int update_image_bindings(struct ngli_pipeline *s, int32_t index,
                                 const struct ngli_image *image,
                                 const struct ngpu_texture *empty_texture)
{
    if (index == -1)
        return 0;

    ngli_assert(index >= 0 && index < s->texture_infos.nb_infos);
    const struct ngli_image_params *params = ngli_image_get_params(image);
    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[index];

    int32_t indices[6];
    get_image_texture_indices(info, indices);

    struct ngpu_texture_binding bindings[NGLI_ARRAY_NB(indices)] = {0};
    if (params->layout == NGLI_IMAGE_LAYOUT_NONE) {
        bindings[0].texture = empty_texture;
    } else {
        const int8_t *slots = layout_plane_slots[params->layout];
        for (size_t i = 0; i < NGLI_ARRAY_NB(params->planes) && slots[i] >= 0; i++)
            bindings[slots[i]] = (struct ngpu_texture_binding) {
                .texture           = params->planes[i],
                .immutable_sampler = params->samplers[i],
            };
    }

    for (size_t i = 0; i < NGLI_ARRAY_NB(indices); i++) {
        const int ret = update_texture(s, indices[i], &bindings[i]);
        /* NOT_FOUND simply means this sampling variant was not generated */
        if (ret < 0 && ret != NGL_ERROR_NOT_FOUND)
            return ret;
    }

    return 0;
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

static int refresh_image(struct ngli_pipeline *s, int32_t index, const struct ngli_image *image)
{
    const enum ngli_image_layout layout = ngli_image_get_params(image)->layout;
    if (layout < NGLI_IMAGE_LAYOUT_NONE || layout >= NGLI_NB_IMAGE_LAYOUTS)
        return NGL_ERROR_INVALID_DATA;
    const struct ngpu_texture *empty_texture = NULL;
    if (layout == NGLI_IMAGE_LAYOUT_NONE) {
        int ret = prepare_empty_image(s, index);
        if (ret < 0)
            return ret;
        const int32_t sampler_index = s->texture_infos.infos[index].sampler_index;
        if (sampler_index >= 0)
            empty_texture = s->texture_slots[sampler_index].empty_texture;
    }
    const int ret = update_image_bindings(s, index, image, empty_texture);
    if (ret < 0)
        return ret;
    return 0;
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
    if (!size || size > allocation_size - offset)
        return NGL_ERROR_INVALID_ARG;
    struct buffer_slot *slot = &s->buffer_slots[index];
    if (slot->dynamic_offset_index >= 0) {
        if (offset > UINT32_MAX)
            return NGL_ERROR_GRAPHICS_LIMIT_EXCEEDED;
        s->dynamic_offsets[slot->dynamic_offset_index] = (uint32_t)offset;
        offset = 0;
    }
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
    slot->checked = false;
    s->updated = 1;
    return 0;
}

/* Drop the reference resolved into a buffer slot, leaving its registration alone */
static void clear_buffer_slot(struct ngli_pipeline *s, int32_t index)
{
    struct buffer_slot *slot = &s->buffer_slots[index];
    struct ngpu_buffer *buffer = (struct ngpu_buffer *)slot->binding.buffer;
    ngpu_buffer_freep(&buffer);
    slot->binding = (struct ngpu_buffer_binding){0};
    slot->checked = false;
    s->updated = 1;
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

int ngli_pipeline_set_buffer_source(struct ngli_pipeline *s, int32_t index, const struct buffer_resource *resource, size_t offset, size_t size)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_buffers)
        return NGL_ERROR_INVALID_ARG;
    struct buffer_slot *slot = &s->buffer_slots[index];
    if (slot->mode == SLOT_IMAGE || slot->mode == SLOT_DIRECT)
        return NGL_ERROR_INVALID_USAGE;
    if (resource && !size)
        return NGL_ERROR_INVALID_ARG;
    struct buffer_resource *ref = ngli_buffer_resource_ref(resource);
    ngli_buffer_resource_freep(&slot->source);
    slot->source = ref;
    slot->source_offset = resource ? offset : 0;
    slot->source_size = resource ? size : 0;
    slot->mode = resource ? SLOT_SOURCE : SLOT_UNBOUND;
    if (!resource)
        clear_buffer_slot(s, index);
    return 0;
}

int ngli_pipeline_set_vertex_source(struct ngli_pipeline *s, int32_t index, const struct buffer_resource *resource)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_vertex_buffers)
        return NGL_ERROR_INVALID_ARG;
    struct vertex_slot *slot = &s->vertex_slots[index];
    if (slot->mode == SLOT_DIRECT)
        return NGL_ERROR_INVALID_USAGE;
    struct buffer_resource *ref = ngli_buffer_resource_ref(resource);
    ngli_buffer_resource_freep(&slot->source);
    slot->source = ref;
    slot->mode = resource ? SLOT_SOURCE : SLOT_UNBOUND;
    if (!resource)
        apply_vertex_buffer(s, index, NULL);
    return 0;
}

int ngli_pipeline_set_texture_source(struct ngli_pipeline *s, int32_t index, const struct texture_resource *resource)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (index < 0 || index >= s->nb_textures)
        return NGL_ERROR_INVALID_ARG;
    struct texture_slot *slot = &s->texture_slots[index];
    if (slot->mode == SLOT_IMAGE || slot->mode == SLOT_DIRECT)
        return NGL_ERROR_INVALID_USAGE;
    struct texture_resource *ref = ngli_texture_resource_ref(resource);
    ngli_texture_resource_freep(&slot->source);
    slot->source = ref;
    slot->mode = resource ? SLOT_SOURCE : SLOT_UNBOUND;
    if (!resource) {
        const struct ngpu_texture_binding empty = {0};
        update_texture(s, index, &empty);
    }
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

int ngli_pipeline_set_index_source(struct ngli_pipeline *s, const struct buffer_resource *resource, enum ngpu_format format)
{
    struct index_slot *slot = &s->index_slot;
    if (slot->mode == SLOT_DIRECT)
        return NGL_ERROR_INVALID_USAGE;
    if (!resource)
        apply_index_buffer(s, NULL, format);
    struct buffer_resource *ref = ngli_buffer_resource_ref(resource);
    ngli_buffer_resource_freep(&slot->source);
    slot->source = ref;
    slot->format = format;
    slot->mode = resource ? SLOT_SOURCE : SLOT_UNBOUND;
    return 0;
}


int ngli_pipeline_set_image_source(struct ngli_pipeline *s, int32_t image_index,
                                   const struct image_resource *resource)
{
    if (image_index == -1)
        return NGL_ERROR_NOT_FOUND;
    if (image_index < 0 || image_index >= s->texture_infos.nb_infos)
        return NGL_ERROR_INVALID_ARG;

    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[image_index];
    const enum slot_mode expected = s->image_sources[image_index] ? SLOT_IMAGE : SLOT_UNBOUND;
    int32_t indices[6];
    get_image_texture_indices(info, indices);
    int used = info->metadata_index >= 0 && s->texture_infos.block_index >= 0;
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
    for (size_t i = 0; i < NGLI_ARRAY_NB(indices); i++) {
        if (indices[i] < 0)
            continue;
        s->texture_slots[indices[i]].mode = mode;
        if (!resource) {
            const struct ngpu_texture_binding empty = {0};
            update_texture(s, indices[i], &empty);
        }
    }
    struct image_resource *ref = ngli_image_resource_ref(resource);
    ngli_image_resource_freep(&s->image_sources[image_index]);
    s->image_sources[image_index] = ref;
    return 0;
}

static int validate_buffer(struct ngli_pipeline *s, size_t index, size_t dynamic_offset)
{
    struct buffer_slot *slot = &s->buffer_slots[index];
    const struct ngpu_buffer_binding *binding = &slot->binding;
    const struct ngpu_bindgroup_layout_entry *entry = &s->bindgroup_layout_desc.buffers[index];
    if (slot->mode == SLOT_UNBOUND || !binding->buffer)
        return NGL_ERROR_INVALID_USAGE;
    const int uniform = entry->type == NGPU_TYPE_UNIFORM_BUFFER || entry->type == NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC;
    const struct ngpu_limits *limits = ngpu_ctx_get_limits(s->gpu_ctx);
    const size_t alignment = uniform ? limits->min_uniform_block_offset_alignment : limits->min_storage_block_offset_alignment;

    /* The dynamic offset is an execution parameter, so this part is per-execution */
    const size_t size = ngpu_buffer_get_size(binding->buffer);
    if (binding->offset > size || dynamic_offset > size - binding->offset ||
        !binding->size || binding->size > size - binding->offset - dynamic_offset)
        return NGL_ERROR_INVALID_ARG;
    if (alignment && dynamic_offset % alignment)
        return NGL_ERROR_INVALID_ARG;

    if (slot->checked)
        return 0;

    const uint32_t usage = uniform ? NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT : NGPU_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!(ngpu_buffer_get_usage(binding->buffer) & usage))
        return NGL_ERROR_INVALID_USAGE;
    if (alignment && binding->offset % alignment)
        return NGL_ERROR_INVALID_ARG;
    const size_t max_size = uniform ? limits->max_uniform_block_size : limits->max_storage_block_size;
    if (binding->size > max_size)
        return NGL_ERROR_GRAPHICS_LIMIT_EXCEEDED;
    slot->checked = true;
    return 0;
}

static int validate_texture(struct ngli_pipeline *s, size_t index)
{
    struct texture_slot *slot = &s->texture_slots[index];
    const struct ngpu_texture *texture = slot->binding.texture;
    const struct ngpu_bindgroup_layout_entry *entry = &s->bindgroup_layout_desc.textures[index];
    const enum ngpu_type type = entry->type;
    const int storage = type == NGPU_TYPE_IMAGE_2D || type == NGPU_TYPE_IMAGE_2D_ARRAY ||
                        type == NGPU_TYPE_IMAGE_3D || type == NGPU_TYPE_IMAGE_CUBE;
    if (slot->mode == SLOT_UNBOUND ||
        (!texture && (storage || slot->mode != SLOT_IMAGE)))
        return NGL_ERROR_INVALID_USAGE;
    /* Inactive sampling variants use the backend's empty texture fallback. */
    if (!texture || slot->checked)
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
    slot->checked = true;
    return 0;
}

static int refresh_sources(struct ngli_pipeline *s, const struct pipeline_execution *execution)
{
    int ret;

    for (size_t i = 0; i < s->nb_buffers; i++) {
        const struct buffer_slot *slot = &s->buffer_slots[i];
        if (slot->mode != SLOT_SOURCE)
            continue;
        const struct ngpu_buffer *buffer = ngli_buffer_resource_get(slot->source);
        if (!buffer) {
            /* Withdrawn publication: release our reference, let validation reject the slot */
            clear_buffer_slot(s, (int32_t)i);
            continue;
        }
        ret = apply_buffer(s, (int32_t)i, buffer, slot->source_offset, slot->source_size);
        if (ret < 0)
            return ret;
    }
    for (size_t i = 0; i < s->nb_vertex_buffers; i++) {
        struct vertex_slot *slot = &s->vertex_slots[i];
        if (slot->mode == SLOT_SOURCE)
            apply_vertex_buffer(s, (int32_t)i, ngli_buffer_resource_get(slot->source));
        const struct ngpu_buffer *buffer = slot->buffer;
        if (slot->mode == SLOT_UNBOUND || !buffer)
            return NGL_ERROR_INVALID_USAGE;
        if (slot->checked)
            continue;
        const size_t size = ngpu_buffer_get_size(buffer);
        if (!size || !(ngpu_buffer_get_usage(buffer) & NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT))
            return NGL_ERROR_INVALID_USAGE;
        const struct ngpu_vertex_buffer_layout *layout = &s->graphics.vertex_state.buffers[i];
        for (size_t j = 0; j < layout->nb_attributes; j++) {
            const struct ngpu_vertex_attribute *attribute = &layout->attributes[j];
            const size_t attribute_size = ngpu_format_get_bytes_per_pixel(attribute->format);
            if (attribute->offset > size || attribute_size > size - attribute->offset)
                return NGL_ERROR_INVALID_ARG;
        }
        slot->checked = true;
    }
    struct index_slot *index_slot = &s->index_slot;
    if (index_slot->mode == SLOT_SOURCE) {
        const struct ngpu_buffer *buffer = ngli_buffer_resource_get(index_slot->source);
        /* A withdrawn publication releases our reference here too */
        apply_index_buffer(s, buffer, index_slot->format);
        if (!buffer || !ngpu_buffer_get_size(buffer) || !(ngpu_buffer_get_usage(buffer) & NGPU_BUFFER_USAGE_INDEX_BUFFER_BIT))
            return NGL_ERROR_INVALID_USAGE;
    }
    for (size_t i = 0; i < s->nb_textures; i++) {
        const struct texture_slot *slot = &s->texture_slots[i];
        if (slot->mode == SLOT_SOURCE) {
            const struct ngpu_texture_binding binding = {.texture = ngli_texture_resource_get(slot->source)};
            ret = update_texture(s, (int32_t)i, &binding);
            if (ret < 0)
                return ret;
        }
    }
    for (size_t i = 0; i < s->texture_infos.nb_infos; i++) {
        const struct image_resource *source = s->image_sources[i];
        if (!source)
            continue;
        const struct ngli_image *image = ngli_image_resource_get(source);
        ret = refresh_image(s, (int32_t)i, image);
        if (ret < 0)
            return ret;
    }
    /* Upload all image metadata once per execution, including repeated draws. */
    ret = push_texture_info_block(s, execution ? execution->staging : NULL);
    if (ret < 0)
        return ret;
    for (size_t i = 0; i < s->nb_buffers; i++) {
        const int32_t index = s->buffer_slots[i].dynamic_offset_index;
        const size_t dynamic_offset = index < 0 ? 0 : s->dynamic_offsets[index];
        ret = validate_buffer(s, i, dynamic_offset);
        if (ret < 0)
            return ret;
    }
    for (size_t i = 0; i < s->nb_textures; i++) {
        ret = validate_texture(s, i);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int prepare_bindgroup(struct ngli_pipeline *s)
{
    if (s->need_pipeline_recreation || !s->gpu_pipeline) {
        reset_pipeline(s);
        int ret = create_pipeline(s);
        if (ret < 0) {
            reset_pipeline(s);
            return ret;
        }
        s->need_pipeline_recreation = 0;
    }
    if (!s->updated && s->cur_bindgroup)
        return 0;

    for (size_t i = 0; i < s->nb_textures; i++)
        s->textures[i] = s->texture_slots[i].binding;
    for (size_t i = 0; i < s->nb_buffers; i++)
        s->buffers[i] = s->buffer_slots[i].binding;
    const struct ngpu_bindgroup_desc desc = {
        .layout      = s->bindgroup_layout,
        .textures    = s->textures,
        .nb_textures = s->nb_textures,
        .buffers     = s->buffers,
        .nb_buffers  = s->nb_buffers,
    };
    if (!s->bindgroup_cache) {
        s->bindgroup_cache = ngpu_bindgroup_cache_create(s->gpu_ctx, BINDGROUP_CACHE_CAPACITY);
        if (!s->bindgroup_cache)
            return NGL_ERROR_MEMORY;
    }
    struct ngpu_bindgroup *bindgroup = ngpu_bindgroup_cache_get(s->bindgroup_cache, &desc);
    if (!bindgroup)
        return NGL_ERROR_MEMORY;
    ngpu_bindgroup_freep(&s->cur_bindgroup);
    s->cur_bindgroup = bindgroup;
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
    /* Recorded commands retain their own references until submission retires. */
    ngpu_bindgroup_cache_clear(s->bindgroup_cache);
    ngpu_bindgroup_freep(&s->cur_bindgroup);
    for (size_t i = 0; s->vertex_slots && i < s->nb_vertex_buffers; i++) {
        struct vertex_slot *slot = &s->vertex_slots[i];
        ngpu_buffer_freep(&slot->buffer);
        slot->checked = false;
        if (slot->mode == SLOT_DIRECT)
            slot->mode = SLOT_UNBOUND;
    }
    for (size_t i = 0; s->buffer_slots && i < s->nb_buffers; i++) {
        struct buffer_slot *slot = &s->buffer_slots[i];
        clear_buffer_slot(s, (int32_t)i);
        if (slot->mode == SLOT_DIRECT)
            slot->mode = SLOT_UNBOUND;
    }
    for (size_t i = 0; s->texture_slots && i < s->nb_textures; i++) {
        struct texture_slot *slot = &s->texture_slots[i];
        struct ngpu_texture *texture = (struct ngpu_texture *)slot->binding.texture;
        ngpu_texture_freep(&texture);
        slot->binding = (struct ngpu_texture_binding){0};
        slot->checked = false;
        ngpu_texture_freep(&slot->empty_texture);
        if (slot->mode == SLOT_DIRECT)
            slot->mode = SLOT_UNBOUND;
        /* Only a sampler change alters the layout, and hence the GPU pipeline */
        struct ngpu_bindgroup_layout_entry *entry = &s->bindgroup_layout_desc.textures[i];
        if (entry->immutable_sampler) {
            entry->immutable_sampler = NULL;
            s->need_pipeline_recreation = 1;
        }
    }
    ngpu_buffer_freep(&s->index_slot.buffer);
    if (s->index_slot.mode == SLOT_DIRECT)
        s->index_slot.mode = SLOT_UNBOUND;
    memset(s->dynamic_offsets, 0, sizeof(s->dynamic_offsets));
    s->updated = 1;
}

void ngli_pipeline_freep(struct ngli_pipeline **sp)
{
    struct ngli_pipeline *s = *sp;
    if (!s)
        return;

    ngli_pipeline_discard_resources(s);
    reset_pipeline(s);
    ngpu_bindgroup_cache_freep(&s->bindgroup_cache);

    ngpu_pipeline_graphics_reset(&s->graphics);

    ngli_freep(&s->bindgroup_layout_desc.textures);
    ngli_freep(&s->bindgroup_layout_desc.buffers);

    for (size_t i = 0; s->buffer_slots && i < s->nb_buffers; i++)
        ngli_buffer_resource_freep(&s->buffer_slots[i].source);
    for (size_t i = 0; s->vertex_slots && i < s->nb_vertex_buffers; i++)
        ngli_buffer_resource_freep(&s->vertex_slots[i].source);
    for (size_t i = 0; s->texture_slots && i < s->nb_textures; i++)
        ngli_texture_resource_freep(&s->texture_slots[i].source);
    for (size_t i = 0; s->image_sources && i < s->texture_infos.nb_infos; i++)
        ngli_image_resource_freep(&s->image_sources[i]);
    ngli_buffer_resource_freep(&s->index_slot.source);

    ngli_freep(&s->buffers);
    ngli_freep(&s->textures);
    ngli_freep(&s->buffer_slots);
    ngli_freep(&s->vertex_slots);
    ngli_freep(&s->texture_slots);
    ngli_freep(&s->image_sources);
    ngli_freep(sp);
}
