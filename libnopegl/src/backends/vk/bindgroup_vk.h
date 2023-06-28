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

#ifndef BINDGROUP_VK_H
#define BINDGROUP_VK_H

#include <stdlib.h>
#include <vulkan/vulkan.h>

#include "bindgroup.h"

struct gpu_ctx;

struct bindgroup_layout_vk {
    struct bindgroup_layout parent;
    struct darray desc_set_layout_bindings; // array of VkDescriptorSetLayoutBinding
    struct darray immutable_samplers;       // array of ycbcr_sampler_vk pointers
    VkDescriptorSetLayout desc_set_layout;
    VkDescriptorPool desc_pool;
};

struct bindgroup_vk {
    struct bindgroup parent;
    int64_t refcount;
    struct darray texture_bindings;   // array of texture_binding_vk
    struct darray buffer_bindings;    // array of buffer_binding_vk
    VkDescriptorSet desc_set;
    struct darray write_desc_sets;    // array of VkWriteDescriptrSet
};

struct bindgroup_layout *ngli_bindgroup_layout_vk_create(struct gpu_ctx *gpu_ctx);
int ngli_bindgroup_layout_vk_init(struct bindgroup_layout *s);
void ngli_bindgroup_layout_vk_freep(struct bindgroup_layout **sp);

struct bindgroup *ngli_bindgroup_vk_create(struct gpu_ctx *gpu_ctx);
int ngli_bindgroup_vk_init(struct bindgroup *s, const struct bindgroup_params *params);
int ngli_bindgroup_vk_set_texture(struct bindgroup *s, int32_t index, const struct texture_binding *binding);
int ngli_bindgroup_vk_set_buffer(struct bindgroup *s, int32_t index, const struct buffer_binding *binding);
int32_t ngli_bindgroup_vk_update(struct bindgroup *s, const struct bindgroup_update_params *params);
int ngli_bindgroup_vk_update_descriptor_sets(struct bindgroup *s);
int ngli_bindgroup_vk_bind(struct bindgroup *s);
void ngli_bindgroup_vk_freep(struct bindgroup **sp);

#endif
