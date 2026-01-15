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

struct hwmap_mc {
    struct android_image *android_image;
    struct ngpu_texture *texture;
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

static int mc_init(struct hwmap *hwmap, struct nmd_frame *frame)
{
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;

    const struct image_params image_params = {
        .width = (uint32_t)frame->width,
        .height = (uint32_t)frame->height,
        .layout = NGLI_IMAGE_LAYOUT_MEDIACODEC,
        .color_scale = 1.f,
        .color_info = ngli_color_info_from_nopemd_frame(frame),
    };
    ngli_image_init(&hwmap->mapped_image, &image_params, &mc->texture);

    hwmap->require_hwconv = !support_direct_rendering(hwmap);

    return 0;
}

static int mc_map_frame(struct hwmap *hwmap, struct nmd_frame *frame)
{
    const struct hwmap_params *params = &hwmap->params;
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;
    struct ngl_ctx *ctx = hwmap->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct android_ctx *android_ctx = &ctx->android_ctx;

    int ret = nmd_mc_frame_render_and_releasep(&frame);
    if (ret < 0)
        return ret;

    struct android_image *android_image;
    ret = ngli_android_imagereader_acquire_next_image(params->android_imagereader, &android_image);
    if (ret < 0)
        return ret;

    ngpu_texture_freep(&mc->texture);
    ngli_android_image_freep(&mc->android_image);

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

    float *matrix = hwmap->mapped_image.coordinates_matrix;
    const int filtering = params->texture_min_filter || params->texture_mag_filter;
    ngli_android_get_crop_matrix(matrix, &desc, &crop_rect, filtering);

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
                .hardware_buffer = hardware_buffer,
            },
        },
    };

    mc->texture = ngpu_texture_create(gpu_ctx);
    if (!mc->texture)
        return NGL_ERROR_MEMORY;

    ret = ngpu_texture_init(mc->texture, &texture_params);
    if (ret < 0)
        return ret;

    hwmap->mapped_image.planes[0] = mc->texture;

    return 0;
}

static void mc_uninit(struct hwmap *hwmap)
{
    struct hwmap_mc *mc = hwmap->hwmap_priv_data;

    ngpu_texture_freep(&mc->texture);
    ngli_android_image_freep(&mc->android_image);
}

const struct hwmap_class ngli_hwmap_mc_gl_class = {
    .name      = "mediacodec (oes zero-copy)",
    .hwformat  = NMD_PIXFMT_MEDIACODEC,
    .layouts   = (const enum image_layout[]){
        NGLI_IMAGE_LAYOUT_MEDIACODEC,
        NGLI_IMAGE_LAYOUT_NONE
    },
    .flags     = HWMAP_FLAG_FRAME_OWNER,
    .priv_size = sizeof(struct hwmap_mc),
    .init      = mc_init,
    .map_frame = mc_map_frame,
    .uninit    = mc_uninit,
};
