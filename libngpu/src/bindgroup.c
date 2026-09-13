/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include "utils/log.h"
#include "bindgroup.h"
#include "ctx.h"
#include "utils/memory.h"
#include "utils/utils.h"

static int layout_entry_is_compatible(const struct ngpu_bindgroup_layout_entry *a,
                                      const struct ngpu_bindgroup_layout_entry *b)
{
    return a->type        == b->type    &&
           a->binding     == b->binding &&
           a->access      == b->access  &&
           a->stage_flags == b->stage_flags &&
           a->immutable_sampler == b->immutable_sampler;
}

static void bindgroup_layout_freep(void **layoutp)
{
    struct ngpu_bindgroup_layout **sp = (struct ngpu_bindgroup_layout **)layoutp;
    if (!*sp)
        return;

    struct ngpu_bindgroup_layout *s = *sp;
    while (s->free_bindgroups) {
        struct ngpu_bindgroup *bindgroup = s->free_bindgroups;
        s->free_bindgroups = bindgroup->next_free;
        ngpu_freep(&bindgroup->textures);
        ngpu_freep(&bindgroup->buffers);
        s->gpu_ctx->cls->bindgroup_freep(&bindgroup);
    }
    ngpu_freep(&s->buffers);
    ngpu_freep(&s->textures);

    (*sp)->gpu_ctx->cls->bindgroup_layout_freep(sp);
}

struct ngpu_bindgroup_layout *ngpu_bindgroup_layout_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngpu_bindgroup_layout *s = gpu_ctx->cls->bindgroup_layout_create(gpu_ctx);
    if (!s)
        return NULL;
    s->rc = NGPU_RC_CREATE(bindgroup_layout_freep);
    return s;
}

int ngpu_bindgroup_layout_init(struct ngpu_bindgroup_layout *s,
                               struct ngpu_bindgroup_layout_desc *desc)
{
    NGPU_ARRAY_MEMDUP(s, desc, textures);
    NGPU_ARRAY_MEMDUP(s, desc, buffers);

    size_t nb_uniform_buffers_dynamic = 0;
    size_t nb_storage_buffers_dynamic = 0;
    for (size_t i = 0; i < s->nb_buffers; i++) {
        const struct ngpu_bindgroup_layout_entry *entry = &s->buffers[i];
        if (entry->type == NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC)
            nb_uniform_buffers_dynamic++;
        else if (entry->type == NGPU_TYPE_STORAGE_BUFFER_DYNAMIC)
            nb_storage_buffers_dynamic++;
    }
    if (nb_uniform_buffers_dynamic > NGPU_MAX_UNIFORM_BUFFERS_DYNAMIC ||
        nb_storage_buffers_dynamic > NGPU_MAX_STORAGE_BUFFERS_DYNAMIC) {
        LOG(ERROR, "too many dynamic buffers (%zu uniform, %zu storage)",
            nb_uniform_buffers_dynamic, nb_storage_buffers_dynamic);
        return NGPU_ERROR_GRAPHICS_LIMIT_EXCEEDED;
    }
    s->nb_dynamic_offsets = nb_uniform_buffers_dynamic + nb_storage_buffers_dynamic;

    return s->gpu_ctx->cls->bindgroup_layout_init(s);
}

int ngpu_bindgroup_layout_is_compatible(const struct ngpu_bindgroup_layout *a, const struct ngpu_bindgroup_layout *b)
{
    if (a->nb_buffers  != b->nb_buffers ||
        a->nb_textures != b->nb_textures)
        return 0;

    for (size_t i = 0; i < a->nb_buffers; i++) {
        if (!layout_entry_is_compatible(&a->buffers[i], &b->buffers[i]))
            return 0;
    }

    for (size_t i = 0; i < a->nb_textures; i++) {
        if (!layout_entry_is_compatible(&a->textures[i], &b->textures[i]))
            return 0;
    }

    return 1;
}

void ngpu_bindgroup_layout_freep(struct ngpu_bindgroup_layout **sp)
{
    NGPU_RC_UNREFP(sp);
}

static int validate_texture(const struct ngpu_bindgroup_layout *layout, size_t index, const struct ngpu_texture_binding *binding)
{
    ngpu_assert(binding->immutable_sampler == layout->textures[index].immutable_sampler);

    if (binding->texture) {
        const struct ngpu_texture *texture = binding->texture;
        const struct ngpu_texture_params *texture_params = ngpu_texture_get_params(texture);
        const struct ngpu_bindgroup_layout_entry *entry = &layout->textures[index];
        switch (entry->type) {
        case NGPU_TYPE_SAMPLER_2D:
        case NGPU_TYPE_SAMPLER_2D_ARRAY:
        case NGPU_TYPE_SAMPLER_2D_RECT:
        case NGPU_TYPE_SAMPLER_3D:
        case NGPU_TYPE_SAMPLER_CUBE:
        case NGPU_TYPE_SAMPLER_EXTERNAL_OES:
        case NGPU_TYPE_SAMPLER_EXTERNAL_2D_Y2Y_EXT:
            ngpu_assert(texture_params->usage & NGPU_TEXTURE_USAGE_SAMPLED_BIT);
            break;
        case NGPU_TYPE_IMAGE_2D:
        case NGPU_TYPE_IMAGE_2D_ARRAY:
        case NGPU_TYPE_IMAGE_3D:
        case NGPU_TYPE_IMAGE_CUBE:
            ngpu_assert(texture_params->usage & NGPU_TEXTURE_USAGE_STORAGE_BIT);
            break;
        default:
            ngpu_assert(0);
        }
    }

    return 0;
}

static int validate_buffer(const struct ngpu_bindgroup_layout *layout, size_t index, const struct ngpu_buffer_binding *binding)
{
    if (!binding->buffer)
        return NGPU_ERROR_INVALID_ARG;

    if (binding->buffer) {
        const struct ngpu_limits *limits = ngpu_ctx_get_limits(layout->gpu_ctx);
        const struct ngpu_buffer *buffer = binding->buffer;
        const uint32_t buffer_usage = ngpu_buffer_get_usage(buffer);
        const size_t buffer_size = ngpu_buffer_get_size(buffer);
        const size_t binding_size = binding->size;
        ngpu_assert(binding_size);
        ngpu_assert(binding->offset <= buffer_size);
        ngpu_assert(binding_size <= buffer_size - binding->offset);
        const struct ngpu_bindgroup_layout_entry *entry = &layout->buffers[index];
        if (entry->type == NGPU_TYPE_UNIFORM_BUFFER ||
            entry->type == NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC) {
            ngpu_assert(buffer_usage & NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            const size_t alignment = limits->min_uniform_block_offset_alignment;
            ngpu_assert(!alignment || binding->offset % alignment == 0);
            if (binding_size > limits->max_uniform_block_size) {
                LOG(ERROR, "buffer (binding=%u) size (%zu) exceeds max uniform block size (%u)",
                    entry->binding, buffer_size, limits->max_uniform_block_size);
                return NGPU_ERROR_GRAPHICS_LIMIT_EXCEEDED;
            }
        } else if (entry->type == NGPU_TYPE_STORAGE_BUFFER ||
                   entry->type == NGPU_TYPE_STORAGE_BUFFER_DYNAMIC) {
            ngpu_assert(buffer_usage & NGPU_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            const size_t alignment = limits->min_storage_block_offset_alignment;
            ngpu_assert(!alignment || binding->offset % alignment == 0);
            if (binding_size > limits->max_storage_block_size) {
                LOG(ERROR, "buffer (binding=%u) size (%zu) exceeds max storage block size (%u)",
                    entry->binding, buffer_size, limits->max_storage_block_size);
                return NGPU_ERROR_GRAPHICS_LIMIT_EXCEEDED;
            }
        } else {
            ngpu_assert(0);
        }
    }

    return 0;
}

static void bindgroup_freep(void **bindgroupp)
{
    struct ngpu_bindgroup *s = *bindgroupp;
    struct ngpu_bindgroup_layout *layout = s->layout;

    /* Command buffers retain this object through completion, so recycling is safe. */
    s->gpu_ctx->cls->bindgroup_reset(s);
    for (size_t i = 0; i < layout->nb_textures; i++) {
        NGPU_RC_UNREFP(&s->textures[i].texture);
        s->textures[i] = (struct ngpu_texture_binding){0};
    }
    for (size_t i = 0; i < layout->nb_buffers; i++) {
        NGPU_RC_UNREFP(&s->buffers[i].buffer);
        s->buffers[i] = (struct ngpu_buffer_binding){0};
    }
    s->layout = NULL;
    s->next_free = layout->free_bindgroups;
    layout->free_bindgroups = s;
    *bindgroupp = NULL;

    /* Keep the allocator alive until the storage has been returned to it. */
    ngpu_bindgroup_layout_freep(&layout);
}

struct ngpu_bindgroup *ngpu_bindgroup_create(struct ngpu_ctx *gpu_ctx, const struct ngpu_bindgroup_desc *desc)
{
    if (!desc || !desc->layout || desc->layout->gpu_ctx != gpu_ctx ||
        desc->nb_textures != desc->layout->nb_textures ||
        desc->nb_buffers != desc->layout->nb_buffers ||
        (desc->nb_textures && !desc->textures) ||
        (desc->nb_buffers && !desc->buffers))
        return NULL;

    for (size_t i = 0; i < desc->nb_textures; i++) {
        if (validate_texture(desc->layout, i, &desc->textures[i]) < 0)
            return NULL;
    }
    for (size_t i = 0; i < desc->nb_buffers; i++) {
        if (validate_buffer(desc->layout, i, &desc->buffers[i]) < 0)
            return NULL;
    }

    struct ngpu_bindgroup_layout *layout = (struct ngpu_bindgroup_layout *)desc->layout;
    struct ngpu_bindgroup *s = layout->free_bindgroups;
    if (s) {
        layout->free_bindgroups = s->next_free;
        s->next_free = NULL;
    } else {
        s = gpu_ctx->cls->bindgroup_create(gpu_ctx);
        if (!s)
            return NULL;
        s->textures = ngpu_try_calloc(layout->nb_textures, sizeof(*s->textures));
        s->buffers = ngpu_try_calloc(layout->nb_buffers, sizeof(*s->buffers));
        if ((layout->nb_textures && !s->textures) || (layout->nb_buffers && !s->buffers)) {
            ngpu_freep(&s->textures);
            ngpu_freep(&s->buffers);
            gpu_ctx->cls->bindgroup_freep(&s);
            return NULL;
        }
    }
    s->rc = NGPU_RC_CREATE(bindgroup_freep);
    s->layout = NGPU_RC_REF(layout);
    for (size_t i = 0; i < desc->nb_textures; i++) {
        s->textures[i] = desc->textures[i];
        if (s->textures[i].texture)
            NGPU_RC_REF(s->textures[i].texture);
    }
    for (size_t i = 0; i < desc->nb_buffers; i++) {
        s->buffers[i] = desc->buffers[i];
        NGPU_RC_REF(s->buffers[i].buffer);
    }
    const int ret = gpu_ctx->cls->bindgroup_init(s);
    if (ret < 0) {
        LOG(ERROR, "could not initialize bindgroup: %d", ret);
        ngpu_bindgroup_freep(&s);
    }
    return s;
}

void ngpu_bindgroup_freep(struct ngpu_bindgroup **sp)
{
    NGPU_RC_UNREFP(sp);
}

size_t ngpu_bindgroup_layout_get_nb_dynamic_offsets(const struct ngpu_bindgroup_layout *s)
{
    return s->nb_dynamic_offsets;
}
