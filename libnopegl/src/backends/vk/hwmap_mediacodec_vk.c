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
#include <media/NdkImage.h>

#include "android_imagereader.h"
#include "hwmap.h"
#include "image.h"
#include "internal.h"
#include "log.h"
#include <ngpu/ngpu.h>
#include "ngpu/ngpu_vulkan.h"
#include "nopegl/nopegl.h"
#include "utils/darray.h"

/* Match the Android BufferQueue limit so every decoder buffer can be cached */
#define MAX_AHB_TEXTURES 64

/*
 * Reuse textures and YCbCr samplers when the decoder returns the same buffers.
 * Hold a buffer reference so a different buffer cannot reuse its address and
 * match the cache entry. Discard unused entries as the decoder replaces its buffers.
 */
struct ahb_texture {
    AHardwareBuffer *buffer;
    struct ngpu_texture *texture;
    struct ngpu_ycbcr_sampler_vk *ycbcr_sampler;
    uint64_t map_index;
};

struct hwmap_mc {
    struct ngli_image_color_cache color_cache;
    struct nmd_frame *frame;
    struct ngpu_texture *texture; // current image's texture, owned by the cache
    struct ngpu_ycbcr_sampler_vk *ycbcr_sampler;
    NGLI_DARRAY(struct ahb_texture) textures;
    uint64_t map_index;
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

static void reset_ahb_texture(void *user_arg, void *data)
{
    struct android_ctx *android_ctx = user_arg;
    struct ahb_texture *ahb_texture = data;
    ngpu_texture_freep(&ahb_texture->texture);
    ngpu_ycbcr_sampler_vk_unrefp(&ahb_texture->ycbcr_sampler);
    android_ctx->AHardwareBuffer_release(ahb_texture->buffer);
}

static int is_stale_ahb_texture(const struct hwmap_mc *mc, const struct ahb_texture *ahb_texture)
{
    return mc->map_index - ahb_texture->map_index > MAX_AHB_TEXTURES;
}

static int get_ycbcr_sampler(struct hwmap *hwmap, AHardwareBuffer *buffer)
{
    const struct hwmap_params *params = &hwmap->params;
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;
    struct ngpu_ctx *gpu_ctx = hwmap->ctx->gpu_ctx;

    struct ngpu_ycbcr_sampler_vk_params sampler_params;
    int ret = ngpu_ycbcr_sampler_vk_params_from_ahb(gpu_ctx, buffer, params->texture_min_filter, &sampler_params);
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

    return 0;
}

static int get_ahb_texture(struct hwmap *hwmap, AHardwareBuffer *buffer, const AHardwareBuffer_Desc *desc,
                           struct ahb_texture *dst)
{
    const struct hwmap_params *params = &hwmap->params;
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;
    struct ngl_ctx *ctx = hwmap->ctx;
    struct android_ctx *android_ctx = &ctx->android_ctx;

    mc->map_index++;
    ngli_darray_remove_if(&mc->textures, is_stale_ahb_texture, mc);

    ngli_darray_foreach(ahb_texture, &mc->textures) {
        if (ahb_texture->buffer == buffer) {
            ahb_texture->map_index = mc->map_index;
            *dst = *ahb_texture;
            return ngpu_texture_vk_acquire_external(ahb_texture->texture);
        }
    }

    if (mc->textures.count == MAX_AHB_TEXTURES) {
        LOG(WARNING, "more than %d buffers in the image reader queue", MAX_AHB_TEXTURES);
        ngli_darray_clear(&mc->textures);
    }

    int ret = get_ycbcr_sampler(hwmap, buffer);
    if (ret < 0)
        return ret;

    struct ngpu_texture_params texture_params = {
        .type       = NGPU_TEXTURE_TYPE_2D,
        .format     = NGPU_FORMAT_UNDEFINED,
        .width      = desc->width,
        .height     = desc->height,
        .min_filter = params->texture_min_filter,
        .mag_filter = params->texture_mag_filter,
        .usage      = NGPU_TEXTURE_USAGE_SAMPLED_BIT,
        .import_params = {
            .type = NGPU_IMPORT_TYPE_AHARDWARE_BUFFER,
            .ahardware_buffer = {
                .hardware_buffer  = buffer,
                .ycbcr_sampler    = mc->ycbcr_sampler,
                .acquire_fence_fd = -1,
            },
        },
    };

    struct ngpu_texture *texture = ngpu_texture_create(ctx->gpu_ctx);
    if (!texture)
        return NGL_ERROR_MEMORY;

    ret = ngpu_texture_init(texture, &texture_params);
    if (ret < 0) {
        ngpu_texture_freep(&texture);
        return ret;
    }

    android_ctx->AHardwareBuffer_acquire(buffer);
    struct ahb_texture ahb_texture = {
        .buffer        = buffer,
        .texture       = texture,
        .ycbcr_sampler = ngpu_ycbcr_sampler_vk_ref(mc->ycbcr_sampler),
        .map_index     = mc->map_index,
    };
    if (ngli_darray_try_push(&mc->textures, ahb_texture) < 0) {
        reset_ahb_texture(android_ctx, &ahb_texture);
        return NGL_ERROR_MEMORY;
    }

    *dst = ahb_texture;
    return 0;
}

static int mc_init(struct hwmap *hwmap, struct nmd_frame *frame)
{
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;
    struct ngl_ctx *ctx = hwmap->ctx;

    ngli_darray_set_free_func(&mc->textures, reset_ahb_texture, &ctx->android_ctx);
    hwmap->require_hwconv = !support_direct_rendering(hwmap);

    return 0;
}

static int release_image(struct hwmap *hwmap)
{
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;

    int ret = 0;
    if (mc->texture) {
        ret = ngpu_texture_vk_release_external(mc->texture);
        mc->texture = NULL;
    }
    ngli_ctx_release_mc_frame(hwmap->ctx, &mc->frame);
    return ret;
}

static int mc_map_frame(struct hwmap *hwmap, struct nmd_frame *frame, struct ngli_image **imagep)
{
    const struct hwmap_params *params = &hwmap->params;
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;
    struct ngl_ctx *ctx = hwmap->ctx;
    struct android_ctx *android_ctx = &ctx->android_ctx;

    const float ts = (float)frame->ts;
    const uint32_t width = (uint32_t)frame->width;
    const uint32_t height = (uint32_t)frame->height;
    const struct color_info color_info = ngli_color_info_from_nopemd_frame(frame);
    const AImage *image = (const AImage *)frame->datap[0];

    int ret = release_image(hwmap);
    mc->frame = frame;
    if (ret < 0)
        return ret;

    AHardwareBuffer *hardware_buffer = NULL;
    if (android_ctx->AImage_getHardwareBuffer(image, &hardware_buffer) != AMEDIA_OK || !hardware_buffer)
        return NGL_ERROR_EXTERNAL;

    AHardwareBuffer_Desc desc;
    android_ctx->AHardwareBuffer_describe(hardware_buffer, &desc);

    AImageCropRect crop_rect;
    if (android_ctx->AImage_getCropRect(image, &crop_rect) != AMEDIA_OK)
        return NGL_ERROR_EXTERNAL;

    struct ngli_mat4 coordinates_matrix = {.m = NGLI_MAT4_IDENTITY};
    const int filtering = params->texture_min_filter || params->texture_mag_filter;
    ngli_android_get_crop_matrix(coordinates_matrix.m, &desc, &crop_rect, filtering);

    struct ahb_texture ahb_texture;
    ret = get_ahb_texture(hwmap, hardware_buffer, &desc, &ahb_texture);
    if (ret < 0)
        return ret;
    mc->texture = ahb_texture.texture;

    ngli_image_color_cache_update(&mc->color_cache, NGLI_IMAGE_LAYOUT_DEFAULT, &color_info, 1.f);

    const struct ngli_image_params image_params = {
        .width                = width,
        .height               = height,
        .layout               = NGLI_IMAGE_LAYOUT_DEFAULT,
        .planes               = {ahb_texture.texture},
        .samplers             = {ahb_texture.ycbcr_sampler},
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

    ngli_darray_reset(&mc->textures);
    ngli_ctx_release_mc_frame(hwmap->ctx, &mc->frame);
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
