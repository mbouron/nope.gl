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

#include "config.h"

#include <stddef.h>

#if defined(TARGET_ANDROID)
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#endif

#include "ngpu/utils/log.h"
#include "ngpu/vulkan/ctx_vk.h"
#include "ngpu/vulkan/vkutils.h"
#include "ngpu/vulkan/texture_vk.h"
#include "ngpu/vulkan/ycbcr_sampler_vk.h"
#include "ngpu/utils/memory.h"

int ngpu_ycbcr_sampler_vk_params_from_ahb(struct ngpu_ctx *gpu_ctx, struct AHardwareBuffer *ahb, enum ngpu_filter filter, struct ngpu_ycbcr_sampler_vk_params *params)
{
#if defined(TARGET_ANDROID)
    struct ngpu_ctx_vk *gpu_ctx_vk = (struct ngpu_ctx_vk *)gpu_ctx;
    struct vkcontext *vk = gpu_ctx_vk->vkcontext;

    VkAndroidHardwareBufferFormatPropertiesANDROID ahb_format_props = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
    };

    VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        .pNext = &ahb_format_props,
    };

    VkResult res =
        vk->GetAndroidHardwareBufferPropertiesANDROID(vk->device, ahb, &ahb_props);
    if (res != VK_SUCCESS) {
        LOG(ERROR, "could not get android hardware buffer properties: %s", ngpu_vk_res2str(res));
        return NGPU_ERROR_GRAPHICS_GENERIC;
    }

    uint64_t external_format = 0;
    if (ahb_format_props.format == VK_FORMAT_UNDEFINED)
        external_format = ahb_format_props.externalFormat;

    const struct ngpu_ycbcr_sampler_vk_params sampler_params = {
        /* Conversion params */
        .android_external_format = external_format,
        .format                  = VK_FORMAT_UNDEFINED,
        .ycbcr_model             = ahb_format_props.suggestedYcbcrModel,
        .ycbcr_range             = ahb_format_props.suggestedYcbcrRange,
        .components              = ahb_format_props.samplerYcbcrConversionComponents,
        .x_chroma_offset         = ahb_format_props.suggestedXChromaOffset,
        .y_chroma_offset         = ahb_format_props.suggestedYChromaOffset,
        /* Sampler params */
        .filter                  = ngpu_vk_get_filter(filter),
    };

    *params = sampler_params;

    return 0;
#else
    return NGPU_ERROR_UNSUPPORTED;
#endif
}

static void ycbcr_sampler_freep(void **texturep)
{
    struct ngpu_ycbcr_sampler_vk **sp = (struct ngpu_ycbcr_sampler_vk **)texturep;
    if (!*sp)
        return;

    struct ngpu_ycbcr_sampler_vk *s = *sp;
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;
    struct ngpu_ctx_vk *gpu_ctx_vk = (struct ngpu_ctx_vk *)gpu_ctx;
    struct vkcontext *vk = gpu_ctx_vk->vkcontext;
    vkDestroySampler(vk->device, s->sampler, NULL);
    vk->DestroySamplerYcbcrConversionKHR(vk->device, s->conv, NULL);

    ngpu_freep(sp);
}

struct ngpu_ycbcr_sampler_vk *ngpu_ycbcr_sampler_vk_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngpu_ycbcr_sampler_vk *s = ngpu_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->rc = NGPU_RC_CREATE(ycbcr_sampler_freep);
    s->gpu_ctx = gpu_ctx;
    return s;
}

VkResult ngpu_ycbcr_sampler_vk_init(struct ngpu_ycbcr_sampler_vk *s, const struct ngpu_ycbcr_sampler_vk_params *params)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;
    struct ngpu_ctx_vk *gpu_ctx_vk = (struct ngpu_ctx_vk *)gpu_ctx;
    struct vkcontext *vk = gpu_ctx_vk->vkcontext;

    s->params = *params;

    const VkExternalFormatANDROID external_format = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
        .externalFormat = params->android_external_format,
    };

    const VkSamplerYcbcrConversionCreateInfoKHR sampler_ycbcr_info = {
        .sType         = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO_KHR,
        .pNext         = &external_format,
        .format        = params->format,
        .ycbcrModel    = params->ycbcr_model,
        .ycbcrRange    = params->ycbcr_range,
        .components    = params->components,
        .xChromaOffset = params->x_chroma_offset,
        .yChromaOffset = params->y_chroma_offset,
        .chromaFilter  = params->filter,
        .forceExplicitReconstruction = VK_FALSE,
    };

    VkResult res = vk->CreateSamplerYcbcrConversionKHR(vk->device, &sampler_ycbcr_info, 0, &s->conv);
    if (res != VK_SUCCESS) {
        LOG(ERROR, "could not create sampler YCbCr conversion: %s", ngpu_vk_res2str(res));
        return NGPU_ERROR_GRAPHICS_GENERIC;
    }

    const VkSamplerYcbcrConversionInfoKHR sampler_ycbcr_conv_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO_KHR,
        .conversion = s->conv,
    };

    const VkSamplerCreateInfo sampler_info = {
        .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext                   = &sampler_ycbcr_conv_info,
        .magFilter               = params->filter,
        .minFilter               = params->filter,
        .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias              = 0.0f,
        .anisotropyEnable        = VK_FALSE,
        .maxAnisotropy           = 1.0f,
        .compareEnable           = VK_FALSE,
        .compareOp               = VK_COMPARE_OP_NEVER,
        .minLod                  = 0.0f,
        .maxLod                  = 0.0f,
        .borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };

    res = vkCreateSampler(vk->device, &sampler_info, 0, &s->sampler);
    if (res != VK_SUCCESS) {
        LOG(ERROR, "could not create sampler: %s", ngpu_vk_res2str(res));
        return NGPU_ERROR_GRAPHICS_GENERIC;
    }

    return VK_SUCCESS;
}

static int ycbcr_sampler_vk_params_eq(const struct ngpu_ycbcr_sampler_vk_params *p0,
                                      const struct ngpu_ycbcr_sampler_vk_params *p1)
{
    return p0->android_external_format == p1->android_external_format &&
           p0->format                  == p1->format &&
           p0->ycbcr_model             == p1->ycbcr_model &&
           p0->ycbcr_range             == p1->ycbcr_range &&
           p0->components.r            == p1->components.r &&
           p0->components.g            == p1->components.g &&
           p0->components.b            == p1->components.b &&
           p0->components.a            == p1->components.a &&
           p0->x_chroma_offset         == p1->x_chroma_offset &&
           p0->y_chroma_offset         == p1->y_chroma_offset &&
           p0->filter                  == p1->filter;
}

int ngpu_ycbcr_sampler_vk_is_compat(const struct ngpu_ycbcr_sampler_vk *s,
                                    const struct ngpu_ycbcr_sampler_vk_params *params)
{
    return ycbcr_sampler_vk_params_eq(&s->params, params);
}

struct ngpu_ycbcr_sampler_vk *ngpu_ycbcr_sampler_vk_ref(struct ngpu_ycbcr_sampler_vk *s)
{
    return NGPU_RC_REF(s);
}

void ngpu_ycbcr_sampler_vk_unrefp(struct ngpu_ycbcr_sampler_vk **sp)
{
    NGPU_RC_UNREFP(sp);
}
