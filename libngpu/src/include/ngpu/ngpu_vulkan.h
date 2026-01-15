/*
 * Copyright 2023-2025 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2018-2022 GoPro Inc.
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

#ifndef NGPU_VULKAN_H
#define NGPU_VULKAN_H

#include <vulkan/vulkan.h>

#include <ngpu/ngpu.h>

struct AHardwareBuffer;

NGPU_API VkDevice ngpu_ctx_vk_get_device(struct ngpu_ctx *ctx);

struct ngpu_ycbcr_sampler_vk_params {
    /* Conversion params */
    uint64_t android_external_format;
    VkFormat format;
    VkSamplerYcbcrModelConversion ycbcr_model;
    VkSamplerYcbcrRange ycbcr_range;
    VkComponentMapping components;
    VkChromaLocation x_chroma_offset;
    VkChromaLocation y_chroma_offset;
    /* Sampler params */
    VkFilter filter;
};

struct ngpu_ycbcr_sampler_vk;

NGPU_API int ngpu_ycbcr_sampler_vk_params_from_ahb(struct ngpu_ctx *gpu_ctx, struct AHardwareBuffer *ahb, enum ngpu_filter filter, struct ngpu_ycbcr_sampler_vk_params *params);

NGPU_API struct ngpu_ycbcr_sampler_vk *ngpu_ycbcr_sampler_vk_create(struct ngpu_ctx *gpu_ctx);
NGPU_API VkResult ngpu_ycbcr_sampler_vk_init(struct ngpu_ycbcr_sampler_vk *s,
                                    const struct ngpu_ycbcr_sampler_vk_params *params);
NGPU_API int ngpu_ycbcr_sampler_vk_is_compat(const struct ngpu_ycbcr_sampler_vk *s,
                                    const struct ngpu_ycbcr_sampler_vk_params *params);
NGPU_API struct ngpu_ycbcr_sampler_vk *ngpu_ycbcr_sampler_vk_ref(struct ngpu_ycbcr_sampler_vk *s);
NGPU_API void ngpu_ycbcr_sampler_vk_unrefp(struct ngpu_ycbcr_sampler_vk **sp);


#endif
