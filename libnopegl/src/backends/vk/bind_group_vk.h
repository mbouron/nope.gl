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

#ifndef BIND_GROUP_VK_H
#define BIND_GROUP_VK_H

#include <stdlib.h>
#include <vulkan/vulkan.h>

#include "bind_group.h"

struct gpu_ctx;

struct bindgroup_layout_vk {
    struct bindgroup_layout parent;
    struct darray desc_set_layout_bindings; // array of VkDescriptorSetLayoutBinding
    VkDescriptorSetLayout desc_set_layout;
    VkDescriptorPool desc_pool;
};

struct bindgroup_vk {
    struct bindgroup parent;
    int64_t refcount;
    struct darray texture_bindings;   // array of texture_binding_vk
    struct darray buffer_bindings;    // array of buffer_binding_vk
    VkDescriptorSet desc_set;
    struct darray desc_sets; // array of VkDescriptorSet
};

struct bindgroup_layout *ngli_bindgroup_layout_vk_create(struct gpu_ctx *gpu_ctx);
int ngli_bindgroup_layout_vk_init(struct bindgroup_layout *s, const struct bindgroup_layout_params *params);
void ngli_bindgroup_layout_vk_freep(struct bindgroup_layout **sp);

struct bindgroup *ngli_bindgroup_vk_create(struct gpu_ctx *gpu_ctx);
int ngli_bindgroup_vk_init(struct bindgroup *s, const struct bindgroup_params *params);
int ngli_bindgroup_vk_set_texture(struct bindgroup *s, int32_t index, const struct texture *texture);
int ngli_bindgroup_vk_set_buffer(struct bindgroup *s, int32_t index, const struct buffer *buffer, size_t offset, size_t size);
int ngli_bindgroup_vk_update_descriptor_sets(struct bindgroup *s);
int ngli_bindgroup_vk_insert_memory_barriers(struct bindgroup *s);
int ngli_bindgroup_vk_bind(struct bindgroup *s);
void ngli_bindgroup_vk_freep(struct bindgroup **sp);

#endif
