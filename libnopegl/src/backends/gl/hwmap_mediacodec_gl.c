/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <nopemd.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>

#include "android_imagereader.h"
#include "hwmap.h"
#include "image.h"
#include "internal.h"
#include "log.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "nopegl/nopegl.h"
#include "utils/darray.h"

/* Maximum number of buffers in an Android BufferQueue. */
#define MAX_AHB_TEXTURES 64

/*
 * The image reader reuses its buffers. Cache one imported texture per buffer
 * to avoid recreating the EGLImage and GL texture for each frame.
 *
 * Keep a reference to `buffer` so its address cannot be reused for another
 * buffer while it is used as a cache key.
 */
struct ahb_texture {
    AHardwareBuffer *buffer;
    struct ngpu_texture *texture;
};

struct hwmap_mc {
    struct ngli_image_color_cache color_cache;
    struct android_image *android_image;
    NGLI_DARRAY(struct ahb_texture) textures;
};

static bool support_direct_rendering(struct hwmap *hwmap)
{
    const struct hwmap_params *params = &hwmap->params;

    bool direct_rendering = NGLI_HAS_ALL_FLAGS(params->image_layouts, NGLI_IMAGE_LAYOUT_MEDIACODEC_BIT);

    if (direct_rendering) {
        if (params->texture_mipmap_filter) {
            LOG(WARNING, "external textures do not support mipmapping: "
                "disabling direct rendering");
            direct_rendering = false;
        } else if (params->texture_wrap_s != NGPU_WRAP_CLAMP_TO_EDGE ||
                   params->texture_wrap_t != NGPU_WRAP_CLAMP_TO_EDGE) {
            LOG(WARNING, "external textures only support clamp to edge wrapping: "
                "disabling direct rendering");
            direct_rendering = false;
        }
    }

    return direct_rendering;
}

static void reset_ahb_texture(void *user_arg, void *data)
{
    struct android_ctx *android_ctx = user_arg;
    struct ahb_texture *ahb_texture = data;
    ngpu_texture_freep(&ahb_texture->texture);
    android_ctx->AHardwareBuffer_release(ahb_texture->buffer);
}

static int owns_ahb_buffer(const AHardwareBuffer *buffer, const struct ahb_texture *ahb_texture)
{
    return ahb_texture->buffer == buffer;
}

static void remove_ahb_texture(void *arg, const AHardwareBuffer *buffer)
{
    struct hwmap_mc *mc = arg;
    ngli_darray_remove_if(&mc->textures, owns_ahb_buffer, buffer);
}

static int get_ahb_texture(struct hwmap *hwmap, AHardwareBuffer *buffer, const AHardwareBuffer_Desc *desc,
                           struct ngpu_texture **texturep)
{
    const struct hwmap_params *params = &hwmap->params;
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;
    struct ngl_ctx *ctx = hwmap->ctx;
    struct android_ctx *android_ctx = &ctx->android_ctx;

    if (!ngli_android_imagereader_flush_removed_buffers(params->android_imagereader, remove_ahb_texture, mc))
        ngli_darray_clear(&mc->textures);

    ngli_darray_foreach(ahb_texture, &mc->textures) {
        if (ahb_texture->buffer == buffer) {
            *texturep = ahb_texture->texture;
            return 0;
        }
    }

    if (mc->textures.count == MAX_AHB_TEXTURES) {
        LOG(WARNING, "more than %d buffers in the image reader queue", MAX_AHB_TEXTURES);
        ngli_darray_clear(&mc->textures);
    }

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
                .acquire_fence_fd = -1,
            },
        },
    };

    struct ngpu_texture *texture = ngpu_texture_create(ctx->gpu_ctx);
    if (!texture)
        return NGL_ERROR_MEMORY;

    int ret = ngpu_texture_init(texture, &texture_params);
    if (ret < 0) {
        ngpu_texture_freep(&texture);
        return ret;
    }

    android_ctx->AHardwareBuffer_acquire(buffer);
    struct ahb_texture ahb_texture = {.buffer = buffer, .texture = texture};
    if (ngli_darray_try_push(&mc->textures, ahb_texture) < 0) {
        reset_ahb_texture(android_ctx, &ahb_texture);
        return NGL_ERROR_MEMORY;
    }

    *texturep = texture;
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

    int ret = nmd_mc_frame_render_and_releasep(&frame);
    if (ret < 0)
        return ret;

    struct android_image *android_image;
    ret = ngli_android_imagereader_acquire_next_image(params->android_imagereader, &android_image);
    if (ret < 0)
        return ret;

    ngli_ctx_release_android_image(ctx, &mc->android_image);

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

    struct ngpu_texture *texture;
    ret = get_ahb_texture(hwmap, hardware_buffer, &desc, &texture);
    if (ret < 0)
        return ret;

    ngli_image_color_cache_update(&mc->color_cache, NGLI_IMAGE_LAYOUT_MEDIACODEC, &color_info, 1.f);

    const struct ngli_image_params image_params = {
        .width                = width,
        .height               = height,
        .layout               = NGLI_IMAGE_LAYOUT_MEDIACODEC,
        .planes               = {texture},
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
    ngli_ctx_release_android_image(hwmap->ctx, &mc->android_image);
}

const struct hwmap_class ngli_hwmap_mc_gl_class = {
    .name      = "mediacodec (oes zero-copy)",
    .hwformat  = NMD_PIXFMT_MEDIACODEC,
    .layouts   = (const enum ngli_image_layout[]){
        NGLI_IMAGE_LAYOUT_MEDIACODEC,
        NGLI_IMAGE_LAYOUT_NONE
    },
    .flags     = HWMAP_FLAG_FRAME_OWNER,
    .priv_size = sizeof(struct hwmap_mc),
    .init      = mc_init,
    .map_frame = mc_map_frame,
    .uninit    = mc_uninit,
};
