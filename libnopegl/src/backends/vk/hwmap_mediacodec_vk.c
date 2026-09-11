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

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <nopemd.h>
#include <vulkan/vulkan.h>
#include <android/hardware_buffer.h>

#include "android_imagereader.h"
#include "hwmap.h"
#include "image.h"
#include "internal.h"
#include "log.h"
#include <ngpu/ngpu.h>
#include "ngpu/ngpu_vulkan.h"
#include "nopegl/nopegl.h"

struct hwmap_mc {
    struct ngli_image_color_cache color_cache;
    struct android_image *android_image;
    struct ngpu_texture *texture;
    struct ngpu_ycbcr_sampler_vk *ycbcr_sampler;
};

static bool support_direct_rendering(struct hwmap *hwmap)
{
    const struct hwmap_params *params = &hwmap->params;

    if (params->texture_mipmap_filter) {
        LOG(WARNING, "samplers with YCbCr conversion enabled do not support mipmapping: "
            "disabling direct rendering");
        return false;
    } else if (params->texture_wrap_s != NGPU_WRAP_CLAMP_TO_EDGE ||
               params->texture_wrap_t != NGPU_WRAP_CLAMP_TO_EDGE) {
        LOG(WARNING, "samplers with YCbCr conversion enabled only support clamp to edge wrapping: "
            "disabling direct rendering");
        return false;
    } else if (params->texture_min_filter != params->texture_mag_filter) {
        LOG(WARNING, "samplers with YCbCr conversion enabled must have the same min/mag filters: "
            "disabling direct_rendering");
        return false;
    }

    return true;
}

static int mc_init(struct hwmap *hwmap, struct nmd_frame *frame)
{
    hwmap->require_hwconv = !support_direct_rendering(hwmap);

    return 0;
}

static void mc_release_frame_resources(struct hwmap *hwmap)
{
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;

    ngpu_texture_freep(&mc->texture);

    ngli_android_image_freep(&mc->android_image);
}

static int mc_map_frame(struct hwmap *hwmap, struct nmd_frame *frame, struct ngli_image **imagep)
{
    const struct hwmap_params *params = &hwmap->params;
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;
    struct ngl_ctx *ctx = hwmap->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct android_ctx *android_ctx = &ctx->android_ctx;

    const float ts = (float)frame->ts;
    const uint32_t width = (uint32_t)frame->width;
    const uint32_t height = (uint32_t)frame->height;
    const struct color_info color_info = ngli_color_info_from_nopemd_frame(frame);

    int ret = nmd_mc_frame_render_and_releasep(&frame);
    if (ret < 0)
        return ret;

    struct android_image *android_image;
    ret = ngli_android_imagereader_acquire_next_image(params->android_imagereader, &android_image);
    if (ret < 0)
        return ret;

    mc_release_frame_resources(hwmap);
    mc->android_image = android_image;

    AHardwareBuffer *hardware_buffer = ngli_android_image_get_hardware_buffer(mc->android_image);
    if (!hardware_buffer)
        return NGL_ERROR_EXTERNAL;

    AHardwareBuffer_Desc desc;
    android_ctx->AHardwareBuffer_describe(hardware_buffer, &desc);

    AImageCropRect crop_rect;
    ret = ngli_android_image_get_crop_rect(mc->android_image, &crop_rect);
    if (ret < 0)
        return ret;

    struct ngli_mat4 coordinates_matrix = {.m = NGLI_MAT4_IDENTITY};
    const int filtering = params->texture_min_filter || params->texture_mag_filter;
    ngli_android_get_crop_matrix(coordinates_matrix.m, &desc, &crop_rect, filtering);

    struct ngpu_ycbcr_sampler_vk_params sampler_params;
    ret = ngpu_ycbcr_sampler_vk_params_from_ahb(gpu_ctx, hardware_buffer, params->texture_min_filter, &sampler_params);
    // FIXME
    if (ret < 0)
        return ret;

    if (!mc->ycbcr_sampler || !ngpu_ycbcr_sampler_vk_is_compat(mc->ycbcr_sampler, &sampler_params)) {
        ngpu_ycbcr_sampler_vk_unrefp(&mc->ycbcr_sampler);

        mc->ycbcr_sampler = ngpu_ycbcr_sampler_vk_create(gpu_ctx);
        if (!mc->ycbcr_sampler)
            return NGL_ERROR_MEMORY;

        VkResult res = ngpu_ycbcr_sampler_vk_init(mc->ycbcr_sampler, &sampler_params);
        if (res != VK_SUCCESS) {
            ngpu_ycbcr_sampler_vk_unrefp(&mc->ycbcr_sampler);
            return res;
        }
    }

    struct ngpu_texture_params texture_params = {
        .type       = NGPU_TEXTURE_TYPE_2D,
        .format     = NGPU_FORMAT_UNDEFINED,
        .width      = desc.width,
        .height     = desc.height,
        .min_filter = params->texture_min_filter,
        .mag_filter = params->texture_mag_filter,
        .usage      = NGPU_TEXTURE_USAGE_SAMPLED_BIT,
        .import_params = {
            .type = NGPU_IMPORT_TYPE_AHARDWARE_BUFFER,
            .ahardware_buffer = {
                .hardware_buffer  = hardware_buffer,
                .ycbcr_sampler    = mc->ycbcr_sampler,
                .acquire_fence_fd = -1,
            },
        },
    };

    mc->texture = ngpu_texture_create(gpu_ctx);
    if (!mc->texture)
        return NGL_ERROR_MEMORY;

    ret = ngpu_texture_init(mc->texture, &texture_params);
    if (ret < 0)
        return ret;

    ngli_image_color_cache_update(&mc->color_cache, NGLI_IMAGE_LAYOUT_DEFAULT, &color_info, 1.f);

    const struct ngli_image_params image_params = {
        .width                = width,
        .height               = height,
        .layout               = NGLI_IMAGE_LAYOUT_DEFAULT,
        .planes               = {mc->texture},
        .samplers             = {mc->ycbcr_sampler},
        .color_scale          = 1.f,
        .color_info           = color_info,
        .color_matrix         = mc->color_cache.color_matrix,
        .mapping_color_matrix = mc->color_cache.mapping_color_matrix,
        .coordinates_matrix   = coordinates_matrix,
        .ts                   = ts,
    };
    *imagep = ngli_image_create(&image_params);
    if (!*imagep)
        return NGL_ERROR_MEMORY;

    return 0;
}

static void mc_uninit(struct hwmap *hwmap)
{
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;

    mc_release_frame_resources(hwmap);
    ngpu_ycbcr_sampler_vk_unrefp(&mc->ycbcr_sampler);
}

const struct hwmap_class ngli_hwmap_mc_vk_class = {
    .name      = "mediacodec (hw buffer → vk image)",
    .hwformat  = NMD_PIXFMT_MEDIACODEC,
    .layouts   = (const enum ngli_image_layout[]){
        NGLI_IMAGE_LAYOUT_DEFAULT,
        NGLI_IMAGE_LAYOUT_NONE
    },
    .flags     = HWMAP_FLAG_FRAME_OWNER,
    .priv_size = sizeof(struct hwmap_mc),
    .init      = mc_init,
    .map_frame = mc_map_frame,
    .uninit    = mc_uninit,
};
