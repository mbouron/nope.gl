/*
 * Copyright 2023-2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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
#include "vulkan/bindgroup_vk.h"
#include "vulkan/buffer_vk.h"
#include "vulkan/ctx_vk.h"
#include "vulkan/priv_vk.h"
#include "vulkan/texture_vk.h"
#include "vulkan/vkcontext.h"
#include "vulkan/vkutils.h"
#include "vulkan/ycbcr_sampler_vk.h"
#include "utils/darray.h"
#include "utils/memory.h"

#define INITIAL_MAX_DESC_SETS 32

struct ngpu_bindgroup_layout *ngpu_bindgroup_layout_vk_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngpu_bindgroup_layout_vk *s = ngpu_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->parent.gpu_ctx = gpu_ctx;
    return (struct ngpu_bindgroup_layout *)s;
}

static VkShaderStageFlags get_vk_stage_flags(uint32_t stage_flags)
{
    VkShaderStageFlags flags = 0;
    if (NGPU_HAS_ALL_FLAGS(stage_flags, NGPU_PROGRAM_STAGE_VERTEX_BIT))
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    if (NGPU_HAS_ALL_FLAGS(stage_flags, NGPU_PROGRAM_STAGE_FRAGMENT_BIT))
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (NGPU_HAS_ALL_FLAGS(stage_flags, NGPU_PROGRAM_STAGE_COMPUTE_BIT))
        flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    return flags;
}

static const VkDescriptorType descriptor_type_map[NGPU_TYPE_NB] = {
    [NGPU_TYPE_UNIFORM_BUFFER]         = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    [NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC] = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
    [NGPU_TYPE_STORAGE_BUFFER]         = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    [NGPU_TYPE_STORAGE_BUFFER_DYNAMIC] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
    [NGPU_TYPE_SAMPLER_2D]             = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    [NGPU_TYPE_SAMPLER_2D_ARRAY]       = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    [NGPU_TYPE_SAMPLER_3D]             = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    [NGPU_TYPE_SAMPLER_CUBE]           = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    [NGPU_TYPE_IMAGE_2D]               = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    [NGPU_TYPE_IMAGE_2D_ARRAY]         = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    [NGPU_TYPE_IMAGE_3D]               = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    [NGPU_TYPE_IMAGE_CUBE]             = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
};

static VkDescriptorType get_vk_descriptor_type(enum ngpu_type type)
{
    const VkDescriptorType descriptor_type = descriptor_type_map[type];
    ngpu_assert(descriptor_type);
    return descriptor_type;
}

static void unref_immutable_sampler(void *user_arg, void *data)
{
    struct ngpu_ycbcr_sampler_vk **ycbcr_samplerp = data;
    ngpu_ycbcr_sampler_vk_unrefp(ycbcr_samplerp);
}

static void destroy_desc_pool(void *user_data, void *data)
{
    const struct ngpu_ctx *gpu_ctx = user_data;
    const struct ngpu_ctx_vk *gpu_ctx_vk = NGPU_PRIV_VK(gpu_ctx);
    const struct vkcontext *vk = gpu_ctx_vk->vkcontext;

    VkDescriptorPool *desc_pool = data;
    if (!desc_pool)
        return;
    vk->funcs.ResetDescriptorPool(vk->device, *desc_pool, 0);
    vk->funcs.DestroyDescriptorPool(vk->device, *desc_pool, NULL);
    *desc_pool = VK_NULL_HANDLE;
}

static VkResult allocate_desc_pool(struct ngpu_bindgroup_layout *s, uint32_t factor)
{
    const struct ngpu_ctx *gpu_ctx = s->gpu_ctx;
    const struct ngpu_ctx_vk *gpu_ctx_vk = NGPU_PRIV_VK(gpu_ctx);
    const struct vkcontext *vk = gpu_ctx_vk->vkcontext;
    struct ngpu_bindgroup_layout_vk *s_priv = NGPU_PRIV_VK(s);

    if (NGPU_CHK_MUL(&s_priv->max_desc_sets, s_priv->max_desc_sets, factor))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (size_t i = 0; i < s_priv->desc_pool_size_count; i++) {
        if (NGPU_CHK_MUL(&s_priv->desc_pool_sizes[i].descriptorCount,
                         s_priv->desc_pool_sizes[i].descriptorCount, factor))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    const VkDescriptorPoolCreateInfo descriptor_pool_create_info = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = s_priv->desc_pool_size_count,
        .pPoolSizes    = s_priv->desc_pool_sizes,
        .maxSets       = s_priv->max_desc_sets,
    };

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult res = vk->funcs.CreateDescriptorPool(vk->device, &descriptor_pool_create_info, NULL, &pool);
    if (res != VK_SUCCESS)
        return res;

    if (ngpu_darray_try_push(&s_priv->desc_pools, pool) < 0) {
        vk->funcs.DestroyDescriptorPool(vk->device, pool, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    s_priv->desc_pool_index = s_priv->desc_pools.count - 1;

    return VK_SUCCESS;
}

static VkResult create_desc_set_layout_bindings(struct ngpu_bindgroup_layout *s)
{
    const struct ngpu_ctx *gpu_ctx = s->gpu_ctx;
    const struct ngpu_ctx_vk *gpu_ctx_vk = NGPU_PRIV_VK(gpu_ctx);
    const struct vkcontext *vk = gpu_ctx_vk->vkcontext;
    struct ngpu_bindgroup_layout_vk *s_priv = NGPU_PRIV_VK(s);

    ngpu_darray_set_free_func(&s_priv->immutable_samplers, unref_immutable_sampler, NULL);

    VkDescriptorPoolSize desc_pool_size_map[NGPU_TYPE_NB] = {
        [NGPU_TYPE_UNIFORM_BUFFER]         = {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
        [NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC] = {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
        [NGPU_TYPE_STORAGE_BUFFER]         = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
        [NGPU_TYPE_STORAGE_BUFFER_DYNAMIC] = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC},
        [NGPU_TYPE_SAMPLER_2D]             = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        [NGPU_TYPE_SAMPLER_2D_ARRAY]       = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        [NGPU_TYPE_SAMPLER_3D]             = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        [NGPU_TYPE_SAMPLER_CUBE]           = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        [NGPU_TYPE_IMAGE_2D]               = {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        [NGPU_TYPE_IMAGE_2D_ARRAY]         = {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        [NGPU_TYPE_IMAGE_3D]               = {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        [NGPU_TYPE_IMAGE_CUBE]             = {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    };

    s_priv->max_desc_sets = INITIAL_MAX_DESC_SETS;

    for (size_t i = 0; i < s->nb_buffers; i++) {
        const struct ngpu_bindgroup_layout_entry *entry = &s->buffers[i];

        const VkDescriptorType type = get_vk_descriptor_type(entry->type);
        const VkDescriptorSetLayoutBinding binding = {
            .binding         = entry->binding,
            .descriptorType  = type,
            .descriptorCount = 1,
            .stageFlags      = get_vk_stage_flags(entry->stage_flags),
        };
        if (ngpu_darray_try_push(&s_priv->desc_set_layout_bindings, binding) < 0)
            return VK_ERROR_OUT_OF_HOST_MEMORY;

        ngpu_assert(desc_pool_size_map[entry->type].type);
        desc_pool_size_map[entry->type].descriptorCount += gpu_ctx->nb_in_flight_frames * s_priv->max_desc_sets;
    }

    for (size_t i = 0; i < s->nb_textures; i++) {
        const struct ngpu_bindgroup_layout_entry *entry = &s->textures[i];

        const VkDescriptorType type = get_vk_descriptor_type(entry->type);
        VkDescriptorSetLayoutBinding binding = {
            .binding            = entry->binding,
            .descriptorType     = type,
            .descriptorCount    = 1,
            .stageFlags         = get_vk_stage_flags(entry->stage_flags),
        };
        if (entry->immutable_sampler) {
            struct ngpu_ycbcr_sampler_vk *ycbcr_sampler = entry->immutable_sampler;
            binding.pImmutableSamplers =  &ycbcr_sampler->sampler;

            if (ngpu_darray_try_push(&s_priv->immutable_samplers, ycbcr_sampler) < 0)
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            ngpu_ycbcr_sampler_vk_ref(ycbcr_sampler);
        }
        if (ngpu_darray_try_push(&s_priv->desc_set_layout_bindings, binding) < 0)
            return VK_ERROR_OUT_OF_HOST_MEMORY;

        ngpu_assert(desc_pool_size_map[entry->type].type);
        desc_pool_size_map[entry->type].descriptorCount += gpu_ctx->nb_in_flight_frames * s_priv->max_desc_sets;
    }

    const VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = (uint32_t)s_priv->desc_set_layout_bindings.count,
        .pBindings    = s_priv->desc_set_layout_bindings.data,
    };

    VkResult res = vk->funcs.CreateDescriptorSetLayout(vk->device, &descriptor_set_layout_create_info, NULL, &s_priv->desc_set_layout);
    if (res != VK_SUCCESS)
        return res;

    s_priv->desc_pool_size_count = 0;
    for (size_t i = 0; i < NGPU_ARRAY_NB(desc_pool_size_map); i++) {
        if (desc_pool_size_map[i].descriptorCount)
            s_priv->desc_pool_sizes[s_priv->desc_pool_size_count++] = desc_pool_size_map[i];
    }

    ngpu_darray_set_free_func(&s_priv->desc_pools, destroy_desc_pool, (void *)gpu_ctx);

    if (!s_priv->desc_pool_size_count)
        return VK_SUCCESS;

    res = allocate_desc_pool(s, 1);
    if (res != VK_SUCCESS)
        return res;

    return VK_SUCCESS;
}

static VkResult ngpu_bindgroup_layout_vk_allocate_set(struct ngpu_bindgroup_layout *s, VkDescriptorSet *desc_set)
{
    const struct ngpu_ctx *gpu_ctx = s->gpu_ctx;
    const struct ngpu_ctx_vk *gpu_ctx_vk = NGPU_PRIV_VK(gpu_ctx);
    const struct vkcontext *vk = gpu_ctx_vk->vkcontext;
    struct ngpu_bindgroup_layout_vk *s_priv = NGPU_PRIV_VK(s);

    *desc_set = VK_NULL_HANDLE;

    if (!ngpu_darray_is_empty(&s_priv->free_desc_sets)) {
        *desc_set = *ngpu_darray_pop(&s_priv->free_desc_sets);
        return VK_SUCCESS;
    }

    for (size_t i = 0; i < s_priv->desc_pools.count; i++) {
        const size_t pool_index = (i + s_priv->desc_pool_index) % s_priv->desc_pools.count;

        VkDescriptorPool *desc_pool = &s_priv->desc_pools.data[pool_index];
        const VkDescriptorSetAllocateInfo descriptor_set_allocate_info = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = *desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &s_priv->desc_set_layout,
        };

        VkResult res = vk->funcs.AllocateDescriptorSets(vk->device, &descriptor_set_allocate_info, desc_set);
        if (res == VK_SUCCESS) {
            return VK_SUCCESS;
        } else if (res == VK_ERROR_OUT_OF_POOL_MEMORY || res == VK_ERROR_FRAGMENTED_POOL) {
            /* pass */
        } else {
            return res;
        }
    }

    VkResult res = allocate_desc_pool(s, 2);
    if (res != VK_SUCCESS)
        return res;

    VkDescriptorPool *desc_pool = &s_priv->desc_pools.data[s_priv->desc_pool_index];
    const VkDescriptorSetAllocateInfo descriptor_set_allocate_info = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = *desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &s_priv->desc_set_layout,
    };

    res = vk->funcs.AllocateDescriptorSets(vk->device, &descriptor_set_allocate_info, desc_set);
    if (res != VK_SUCCESS)
        return res;

    return VK_SUCCESS;
}

int ngpu_bindgroup_layout_vk_init(struct ngpu_bindgroup_layout *s)
{
    VkResult res = create_desc_set_layout_bindings(s);
    if (res != VK_SUCCESS)
        return ngpu_vk_res2ret(res);

    return 0;
}

void ngpu_bindgroup_layout_vk_freep(struct ngpu_bindgroup_layout **sp)
{
    if (!*sp)
        return;

    struct ngpu_bindgroup_layout *s = *sp;
    struct ngpu_bindgroup_layout_vk *s_priv = NGPU_PRIV_VK(s);
    const struct ngpu_ctx_vk *gpu_ctx_vk = NGPU_PRIV_VK(s->gpu_ctx);
    const struct vkcontext *vk = gpu_ctx_vk->vkcontext;

    ngpu_darray_reset(&s_priv->desc_set_layout_bindings);
    ngpu_darray_reset(&s_priv->immutable_samplers);
    ngpu_darray_reset(&s_priv->free_desc_sets);
    ngpu_darray_reset(&s_priv->desc_pools);

    vk->funcs.DestroyDescriptorSetLayout(vk->device, s_priv->desc_set_layout, NULL);

    ngpu_freep(sp);
}

struct ngpu_bindgroup *ngpu_bindgroup_vk_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngpu_bindgroup_vk *s = ngpu_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->parent.gpu_ctx = gpu_ctx;
    return (struct ngpu_bindgroup *)s;
}

int ngpu_bindgroup_vk_init(struct ngpu_bindgroup *s)
{
    struct ngpu_bindgroup_vk *s_priv = NGPU_PRIV_VK(s);
    struct ngpu_ctx_vk *gpu_ctx_vk = NGPU_PRIV_VK(s->gpu_ctx);
    struct vkcontext *vk = gpu_ctx_vk->vkcontext;
    const struct ngpu_bindgroup_layout *layout = s->layout;

    /* Reserve backing storage before any write points into it. */
    if (ngpu_darray_try_reserve(&s_priv->texture_bindings, layout->nb_textures) < 0 ||
        ngpu_darray_try_reserve(&s_priv->buffer_bindings, layout->nb_buffers) < 0 ||
        ngpu_darray_try_reserve(&s_priv->image_infos, layout->nb_textures) < 0 ||
        ngpu_darray_try_reserve(&s_priv->buffer_infos, layout->nb_buffers) < 0 ||
        ngpu_darray_try_reserve(&s_priv->write_desc_sets, layout->nb_textures + layout->nb_buffers) < 0)
        return NGPU_ERROR_MEMORY;

    VkResult res = ngpu_bindgroup_layout_vk_allocate_set(s->layout, &s_priv->desc_set);
    if (res != VK_SUCCESS)
        return ngpu_vk_res2ret(res);

    for (size_t i = 0; i < layout->nb_textures; i++) {
        const struct ngpu_bindgroup_layout_entry *entry = &layout->textures[i];
        const struct ngpu_texture *texture = s->textures[i].texture;
        if (!texture)
            texture = gpu_ctx_vk->dummy_texture;
        const struct ngpu_texture_vk *texture_vk = NGPU_PRIV_VK(texture);
        ngpu_darray_push(&s_priv->texture_bindings, (struct texture_binding_vk){
            .layout_entry = *entry,
            .texture = texture,
        });
        ngpu_darray_push(&s_priv->image_infos, (VkDescriptorImageInfo){
            .imageLayout = texture_vk->default_image_layout,
            .imageView   = texture_vk->image_view,
            .sampler     = texture_vk->sampler,
        });
        ngpu_darray_push(&s_priv->write_desc_sets, (VkWriteDescriptorSet){
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = s_priv->desc_set,
            .dstBinding      = entry->binding,
            .descriptorType  = get_vk_descriptor_type(entry->type),
            .descriptorCount = 1,
            .pImageInfo      = &s_priv->image_infos.data[i],
        });
    }

    for (size_t i = 0; i < layout->nb_buffers; i++) {
        const struct ngpu_bindgroup_layout_entry *entry = &layout->buffers[i];
        const struct ngpu_buffer_binding *binding = &s->buffers[i];
        const struct ngpu_buffer_vk *buffer_vk = NGPU_PRIV_VK(binding->buffer);
        ngpu_darray_push(&s_priv->buffer_bindings, (struct buffer_binding_vk){
            .layout_entry = *entry,
            .buffer = binding->buffer,
            .offset = binding->offset,
            .size = binding->size,
        });
        ngpu_darray_push(&s_priv->buffer_infos, (VkDescriptorBufferInfo){
            .buffer = buffer_vk->buffer,
            .offset = binding->offset,
            .range  = binding->size,
        });
        ngpu_darray_push(&s_priv->write_desc_sets, (VkWriteDescriptorSet){
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = s_priv->desc_set,
            .dstBinding      = entry->binding,
            .descriptorType  = get_vk_descriptor_type(entry->type),
            .descriptorCount = 1,
            .pBufferInfo     = &s_priv->buffer_infos.data[i],
        });
    }

    if (s_priv->write_desc_sets.count)
        vk->funcs.UpdateDescriptorSets(vk->device, (uint32_t)s_priv->write_desc_sets.count,
                                      s_priv->write_desc_sets.data, 0, NULL);
    return 0;
}

void ngpu_bindgroup_vk_reset(struct ngpu_bindgroup *s)
{
    struct ngpu_bindgroup_vk *s_priv = NGPU_PRIV_VK(s);
    if (s_priv->desc_set != VK_NULL_HANDLE) {
        struct ngpu_bindgroup_layout_vk *layout_priv = NGPU_PRIV_VK(s->layout);
        if (ngpu_darray_try_push(&layout_priv->free_desc_sets, s_priv->desc_set) < 0)
            LOG(WARNING, "could not recycle descriptor set, its pool will keep it until the layout is destroyed");
        s_priv->desc_set = VK_NULL_HANDLE;
    }
    ngpu_darray_clear(&s_priv->texture_bindings);
    ngpu_darray_clear(&s_priv->buffer_bindings);
    ngpu_darray_clear(&s_priv->write_desc_sets);
    ngpu_darray_clear(&s_priv->image_infos);
    ngpu_darray_clear(&s_priv->buffer_infos);
}

void ngpu_bindgroup_vk_freep(struct ngpu_bindgroup **sp)
{
    if (!*sp)
        return;
    struct ngpu_bindgroup_vk *s_priv = NGPU_PRIV_VK(*sp);
    ngpu_darray_reset(&s_priv->texture_bindings);
    ngpu_darray_reset(&s_priv->buffer_bindings);
    ngpu_darray_reset(&s_priv->write_desc_sets);
    ngpu_darray_reset(&s_priv->image_infos);
    ngpu_darray_reset(&s_priv->buffer_infos);
    ngpu_freep(sp);
}
