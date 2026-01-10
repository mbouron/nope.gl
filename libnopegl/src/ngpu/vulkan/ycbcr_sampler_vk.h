/*
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

#ifndef YCBCR_SAMPLER_VK_H
#define YCBCR_SAMPLER_VK_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#include "utils/refcount.h"

struct ngpu_ctx;

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

struct ngpu_ycbcr_sampler_vk {
    struct ngli_rc rc;
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_ycbcr_sampler_vk_params params;
    VkSamplerYcbcrConversion conv;
    VkSampler sampler;
};

NGLI_RC_CHECK_STRUCT(ngpu_ycbcr_sampler_vk);

struct ngpu_ycbcr_sampler_vk *ngpu_ycbcr_sampler_vk_create(struct ngpu_ctx *gpu_ctx);
VkResult ngpu_ycbcr_sampler_vk_init(struct ngpu_ycbcr_sampler_vk *s,
                                    const struct ngpu_ycbcr_sampler_vk_params *params);
int ngpu_ycbcr_sampler_vk_is_compat(const struct ngpu_ycbcr_sampler_vk *s,
                                    const struct ngpu_ycbcr_sampler_vk_params *params);
struct ngpu_ycbcr_sampler_vk *ngpu_ycbcr_sampler_vk_ref(struct ngpu_ycbcr_sampler_vk *s);
void ngpu_ycbcr_sampler_vk_unrefp(struct ngpu_ycbcr_sampler_vk **sp);

#endif
