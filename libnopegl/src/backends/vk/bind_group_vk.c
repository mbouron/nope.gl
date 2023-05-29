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

#include "bind_group_vk.h"
#include "gpu_ctx_vk.h"
#include "memory.h"
#include "vkutils.h"
#include "ycbcr_sampler_vk.h"
#include "texture_vk.h"
#include "buffer_vk.h"

struct texture_binding_vk {
    struct bindgroup_layout_entry layout_entry;
    const struct texture *texture;
    int use_ycbcr_sampler;
    struct ycbcr_sampler_vk *ycbcr_sampler;
    uint32_t update_desc_flags;
};

struct buffer_binding_vk {
    struct bindgroup_layout_entry layout_entry;
    const struct buffer *buffer;
    size_t offset;
    size_t size;
    uint32_t update_desc_flags;
};

struct bindgroup_layout *ngli_bindgroup_layout_vk_create(struct gpu_ctx *gpu_ctx)
{
    struct bindgroup_layout_vk *s = ngli_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->parent.gpu_ctx = gpu_ctx;
    return (struct bindgroup_layout *)s;
}

static const VkShaderStageFlags stage_flag_map[NGLI_PROGRAM_SHADER_NB] = {
    [NGLI_PROGRAM_SHADER_VERT] = VK_SHADER_STAGE_VERTEX_BIT,
    [NGLI_PROGRAM_SHADER_FRAG] = VK_SHADER_STAGE_FRAGMENT_BIT,
    [NGLI_PROGRAM_SHADER_COMP] = VK_SHADER_STAGE_COMPUTE_BIT,
};

static VkShaderStageFlags get_vk_stage_flags(int stage)
{
    return stage_flag_map[stage];
}

static const VkDescriptorType descriptor_type_map[NGLI_TYPE_NB] = {
    [NGLI_TYPE_UNIFORM_BUFFER] = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    [NGLI_TYPE_STORAGE_BUFFER] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    [NGLI_TYPE_SAMPLER_2D]     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    [NGLI_TYPE_SAMPLER_2D_ARRAY] = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    [NGLI_TYPE_SAMPLER_3D]     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    [NGLI_TYPE_SAMPLER_CUBE]   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    [NGLI_TYPE_IMAGE_2D]       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    [NGLI_TYPE_IMAGE_2D_ARRAY] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    [NGLI_TYPE_IMAGE_3D]       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    [NGLI_TYPE_IMAGE_CUBE]     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
};

static VkDescriptorType get_vk_descriptor_type(int type)
{
    const VkDescriptorType descriptor_type = descriptor_type_map[type];
    ngli_assert(descriptor_type);
    return descriptor_type;
}

static VkResult create_desc_set_layout_bindings(struct bindgroup_layout *s)
{
    const struct gpu_ctx_vk *gpu_ctx_vk = (struct gpu_ctx_vk *)s->gpu_ctx;
    const struct vkcontext *vk = gpu_ctx_vk->vkcontext;
    struct bindgroup_layout_vk *s_priv = (struct bindgroup_layout_vk *)s;

    ngli_darray_init(&s_priv->desc_set_layout_bindings, sizeof(VkDescriptorSetLayoutBinding), 0);

    VkDescriptorPoolSize desc_pool_size_map[NGLI_TYPE_NB] = {
        [NGLI_TYPE_UNIFORM_BUFFER] = {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
        [NGLI_TYPE_STORAGE_BUFFER] = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
        [NGLI_TYPE_SAMPLER_2D]     = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        [NGLI_TYPE_SAMPLER_2D_ARRAY] = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        [NGLI_TYPE_SAMPLER_3D]     = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        [NGLI_TYPE_SAMPLER_CUBE]   = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        [NGLI_TYPE_IMAGE_2D]       = {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        [NGLI_TYPE_IMAGE_2D_ARRAY] = {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        [NGLI_TYPE_IMAGE_3D]       = {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        [NGLI_TYPE_IMAGE_CUBE]     = {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    };

    for (size_t i = 0; i < s->nb_buffer_entries; i++) {
        const struct bindgroup_layout_entry *entry = &s->buffer_entries[i];

        const VkDescriptorType type = get_vk_descriptor_type(entry->type);
        const VkDescriptorSetLayoutBinding binding = {
            .binding         = entry->binding,
            .descriptorType  = type,
            .descriptorCount = 1,
            .stageFlags      = get_vk_stage_flags(entry->stage),
        };
        if (!ngli_darray_push(&s_priv->desc_set_layout_bindings, &binding))
            return VK_ERROR_OUT_OF_HOST_MEMORY;

        ngli_assert(desc_pool_size_map[entry->type].type);
        desc_pool_size_map[entry->type].descriptorCount += gpu_ctx_vk->nb_in_flight_frames;
    }

    for (size_t i = 0; i < s->nb_texture_entries; i++) {
        const struct bindgroup_layout_entry *desc = &s->texture_entries[i];

        const VkDescriptorType type = get_vk_descriptor_type(desc->type);
        const VkDescriptorSetLayoutBinding binding = {
            .binding         = desc->binding,
            .descriptorType  = type,
            .descriptorCount = 1,
            .stageFlags      = get_vk_stage_flags(desc->stage),
        };
        if (!ngli_darray_push(&s_priv->desc_set_layout_bindings, &binding))
            return VK_ERROR_OUT_OF_HOST_MEMORY;

        ngli_assert(desc_pool_size_map[desc->type].type);
        desc_pool_size_map[desc->type].descriptorCount += gpu_ctx_vk->nb_in_flight_frames;
    }

    const VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = (uint32_t)ngli_darray_count(&s_priv->desc_set_layout_bindings),
        .pBindings    = ngli_darray_data(&s_priv->desc_set_layout_bindings),
    };

    VkResult res = vkCreateDescriptorSetLayout(vk->device, &descriptor_set_layout_create_info, NULL, &s_priv->desc_set_layout);
    if (res != VK_SUCCESS)
        return res;
    
    uint32_t nb_desc_pool_sizes = 0;
    VkDescriptorPoolSize desc_pool_sizes[NGLI_TYPE_NB];
    for (size_t i = 0; i < NGLI_ARRAY_NB(desc_pool_size_map); i++) {
        if (desc_pool_size_map[i].descriptorCount)
            desc_pool_sizes[nb_desc_pool_sizes++] = desc_pool_size_map[i];
    }
    if (!nb_desc_pool_sizes)
        return VK_SUCCESS;

    const VkDescriptorPoolCreateInfo descriptor_pool_create_info = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = nb_desc_pool_sizes,
        .pPoolSizes    = desc_pool_sizes,
        .maxSets       = gpu_ctx_vk->nb_in_flight_frames,
    };

    res = vkCreateDescriptorPool(vk->device, &descriptor_pool_create_info, NULL, &s_priv->desc_pool);
    if (res != VK_SUCCESS)
        return res;

    return VK_SUCCESS;
}

int ngli_bindgroup_layout_vk_init(struct bindgroup_layout *s, const struct bindgroup_layout_params *params)
{
    if (params->nb_texture_entries) {
        s->texture_entries = ngli_memdup(params->texture_entries, params->nb_texture_entries * sizeof(*params->texture_entries));
        if (!s->texture_entries)
            return NGL_ERROR_MEMORY;
        s->nb_texture_entries = params->nb_texture_entries;
    }

    if (params->nb_buffer_entries) {
        s->buffer_entries = ngli_memdup(params->buffer_entries, params->nb_buffer_entries * sizeof(*params->buffer_entries));
        if (!s->buffer_entries)
            return NGL_ERROR_MEMORY;
        s->nb_buffer_entries = params->nb_buffer_entries;
    }

    VkResult res = create_desc_set_layout_bindings(s);
    if (res != VK_SUCCESS)
        return ngli_vk_res2ret(res);

    return 0;
}

void ngli_bindgroup_layout_vk_freep(struct bindgroup_layout **sp)
{
    struct bindgroup_layout *s = *sp;
    if (!s)
        return;
    ngli_free(s->texture_entries);
    ngli_free(s->buffer_entries);
    ngli_freep(sp);
}

struct bindgroup *ngli_bindgroup_vk_create(struct gpu_ctx *gpu_ctx)
{
    struct bindgroup_vk *s = ngli_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->parent.gpu_ctx = gpu_ctx;
    return (struct bindgroup *)s;
}

static VkResult create_desc_sets(struct bindgroup *s)
{
    const struct gpu_ctx_vk *gpu_ctx_vk = (struct gpu_ctx_vk *)s->gpu_ctx;
    const struct vkcontext *vk = gpu_ctx_vk->vkcontext;
    struct bindgroup_vk *s_priv = (struct bindgroup_vk *)s;
    const struct bindgroup_layout_vk *layout_vk = (const struct bindgroup_layout_vk *)s->layout;

    VkDescriptorSetLayout *desc_set_layouts = ngli_calloc(gpu_ctx_vk->nb_in_flight_frames, sizeof(*desc_set_layouts));
    if (!desc_set_layouts)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (uint32_t i = 0; i < gpu_ctx_vk->nb_in_flight_frames; i++)
        desc_set_layouts[i] = layout_vk->desc_set_layout;

    const VkDescriptorSetAllocateInfo descriptor_set_allocate_info = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = layout_vk->desc_pool,
        .descriptorSetCount = gpu_ctx_vk->nb_in_flight_frames,
        .pSetLayouts        = desc_set_layouts
    };

    s_priv->desc_sets = ngli_calloc(gpu_ctx_vk->nb_in_flight_frames, sizeof(*s_priv->desc_sets));
    if (!s_priv->desc_sets) {
        ngli_free(desc_set_layouts);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    VkResult res = vkAllocateDescriptorSets(vk->device, &descriptor_set_allocate_info, s_priv->desc_sets);
    if (res != VK_SUCCESS) {
        ngli_free(desc_set_layouts);
        return res;
    }

    ngli_free(desc_set_layouts);
    return VK_SUCCESS;
}

int ngli_bindgroup_vk_init(struct bindgroup *s, const struct bindgroup_params *params)
{
    struct bindgroup_vk *s_priv = (struct bindgroup_vk *)s;

    s->layout = params->layout;

    ngli_darray_init(&s_priv->texture_bindings, sizeof(struct texture_binding_vk), 0);
    ngli_darray_init(&s_priv->buffer_bindings, sizeof(struct buffer_binding_vk), 0);

    VkResult res = create_desc_sets(s);
    if (res != VK_SUCCESS)
        return ngli_vk_res2ret(res);

    const struct bindgroup_layout *layout = s->layout;
    for (size_t i = 0; i < layout->nb_buffer_entries; i++) {
        const struct pipeline_buffer_desc *desc = &layout->buffer_entries[i];

        const struct buffer_binding_vk buffer_binding = {
            .layout_entry = *desc,
        };
        if (!ngli_darray_push(&s_priv->buffer_bindings, &buffer_binding))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    for (size_t i = 0; i < layout->nb_texture_entries; i++) {
        const struct pipeline_texture_desc *desc = &layout->texture_entries[i];

        const struct texture_binding_vk texture_binding = {
            .layout_entry = *desc,
        };
        if (!ngli_darray_push(&s_priv->texture_bindings, &texture_binding))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    return 0;
}

int ngli_bindgroup_vk_set_texture(struct bindgroup *s, int32_t index, const struct texture *texture)
{
    struct bindgroup_vk *s_priv = (struct bindgroup_vk *)s;
    struct gpu_ctx_vk *gpu_ctx_vk = (struct gpu_ctx_vk *)s->gpu_ctx;
    struct vkcontext *vk = gpu_ctx_vk->vkcontext;

    struct texture_binding_vk *texture_binding = ngli_darray_get(&s_priv->texture_bindings, index);
    ngli_assert(texture_binding);

    texture_binding->texture = texture ? texture : gpu_ctx_vk->dummy_texture;
    texture_binding->update_desc_flags = ~0;

    if (texture) {
        struct texture_vk *texture_vk = (struct texture_vk *)texture;
        #if 0
        if (need_desc_set_layout_update(texture_binding, texture)) {
            VkDescriptorSetLayoutBinding *desc_bindings = ngli_darray_data(&s_priv->desc_set_layout_bindings);
            VkDescriptorSetLayoutBinding *desc_binding = &desc_bindings[texture_binding->desc_binding_index];
            texture_binding->use_ycbcr_sampler = texture_vk->use_ycbcr_sampler;
            ngli_ycbcr_sampler_vk_unrefp(&texture_binding->ycbcr_sampler);
            if (texture_vk->use_ycbcr_sampler) {
                /*
                 * YCbCr conversion enabled samplers must be set at bindgroup
                 * creation (through the pImmutableSamplers field). Moreover,
                 * the YCbCr conversion and sampler objects must be kept alive
                 * as long as they are used by the bindgroup. For that matter,
                 * we hold a reference on the ycbcr_sampler_vk structure
                 * (containing the conversion and sampler objects) as long as
                 * necessary.
                 */
                texture_binding->ycbcr_sampler = ngli_ycbcr_sampler_vk_ref(texture_vk->ycbcr_sampler);
                desc_binding->pImmutableSamplers = &texture_vk->ycbcr_sampler->sampler;
            } else {
                desc_binding->pImmutableSamplers = VK_NULL_HANDLE;
            }

            VkResult res = recreate_bindgroup(s);
            if (res != VK_SUCCESS) {
                LOG(ERROR, "could not recreate bindgroup");
                return ngli_vk_res2ret(res);
            }
        }
        #endif
    }
    struct texture_binding_vk *texture_bindings = ngli_darray_data(&s_priv->texture_bindings);
    struct texture_binding_vk *binding = &texture_bindings[index];
    const struct texture_vk *texture_vk = (struct texture_vk *)binding->texture;
    const VkDescriptorImageInfo image_info = {
        .imageLayout = texture_vk->default_image_layout,
        .imageView   = texture_vk->image_view,
        .sampler     = texture_vk->sampler,
    };
    const struct bindgroup_layout_entry *layout_entry = &binding->layout_entry;
    const VkWriteDescriptorSet write_descriptor_set = {
        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet           = s_priv->desc_sets[gpu_ctx_vk->cur_frame_index],
        .dstBinding       = layout_entry->binding,
        .dstArrayElement  = 0,
        .descriptorType   = get_vk_descriptor_type(layout_entry->type),
        .descriptorCount  = 1,
        .pImageInfo       = &image_info,
    };
    vkUpdateDescriptorSets(vk->device, 1, &write_descriptor_set, 0, NULL);

    return 0;
}

#include "log.h"

int ngli_bindgroup_vk_set_buffer(struct bindgroup *s, int32_t index, const struct buffer *buffer, size_t offset, size_t size)
{
    struct bindgroup_vk *s_priv = (struct bindgroup_vk *)s;
    struct gpu_ctx_vk *gpu_ctx_vk = (struct gpu_ctx_vk *)s->gpu_ctx;
    struct vkcontext *vk = gpu_ctx_vk->vkcontext;

    struct buffer_binding_vk *buffer_binding = ngli_darray_get(&s_priv->buffer_bindings, index);
    ngli_assert(buffer_binding);
    ngli_assert(buffer);

    LOG(ERROR, "%zd %zd", buffer_binding->offset, offset);
    buffer_binding->buffer = buffer;
    buffer_binding->offset = offset;
    buffer_binding->size = size;
    buffer_binding->update_desc_flags = ~0;

    struct buffer_binding_vk *buffer_bindings = ngli_darray_data(&s_priv->buffer_bindings);
    struct buffer_binding_vk *binding = &buffer_bindings[index];
    const struct bindgroup_layout_entry *desc = &binding->layout_entry;
    const struct buffer_vk *buffer_vk = (struct buffer_vk *)(binding->buffer);
    const VkDescriptorBufferInfo descriptor_buffer_info = {
        .buffer = buffer_vk->buffer,
        .offset = binding->offset,
        .range  = binding->size ? binding->size : binding->buffer->size,
    };
    const VkWriteDescriptorSet write_descriptor_set = {
        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet           = s_priv->desc_sets[0],
        .dstBinding       = desc->binding,
        .dstArrayElement  = 0,
        .descriptorType   = get_vk_descriptor_type(desc->type),
        .descriptorCount  = 1,
        .pBufferInfo      = &descriptor_buffer_info,
        .pImageInfo       = NULL,
        .pTexelBufferView = NULL,
    };
    vkUpdateDescriptorSets(vk->device, 1, &write_descriptor_set, 0, NULL);
    return 0;
}

int ngli_bindgroup_vk_insert_memory_barriers(struct bindgroup *s)
{
    return 0;
}

void ngli_bindgroup_vk_freep(struct bindgroup **sp)
{
    if (!*sp)
        return;

    struct bindgroup *s = *sp;
    struct bindgroup_vk *s_priv = (struct bindgroup_vk *)s;

    ngli_darray_reset(&s_priv->texture_bindings);
    ngli_darray_reset(&s_priv->buffer_bindings);
    ngli_freep(sp);
}
