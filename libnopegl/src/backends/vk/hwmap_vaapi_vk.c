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

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <nopemd.h>

#include <va/va.h>
#include <va/va_drmcommon.h>
#include <libdrm/drm_fourcc.h>

#include "hwmap.h"
#include "image.h"
#include "internal.h"
#include "log.h"
#include "math_utils.h"
#include "ngpu/ngpu.h"
#include "nopegl/nopegl.h"
#include "utils/utils.h"

struct format_desc {
    int layout;
    size_t nb_planes;
    int log2_chroma_width;
    int log2_chroma_height;
    enum ngpu_format formats[2];
};

static int vaapi_get_format_desc(uint32_t format, struct format_desc *desc)
{
    switch (format) {
    case VA_FOURCC_NV12:
        *desc = (struct format_desc) {
            .layout = NGLI_IMAGE_LAYOUT_NV12,
            .nb_planes = 2,
            .log2_chroma_width = 1,
            .log2_chroma_height = 1,
            .formats[0] = NGPU_FORMAT_R8_UNORM,
            .formats[1] = NGPU_FORMAT_R8G8_UNORM,
        };
        break;
    case VA_FOURCC_P010:
    case VA_FOURCC_P016:
        *desc = (struct format_desc) {
            .layout = NGLI_IMAGE_LAYOUT_NV12,
            .nb_planes = 2,
            .log2_chroma_width = 1,
            .log2_chroma_height = 1,
            .formats[0] = NGPU_FORMAT_R16_UNORM,
            .formats[1] = NGPU_FORMAT_R16G16_UNORM,
        };
        break;
    default:
        LOG(ERROR, "unsupported vaapi surface format %u", format);
        return NGL_ERROR_UNSUPPORTED;
    }

    return 0;
}

struct hwmap_vaapi {
    struct nmd_frame *frame;
    struct ngpu_texture *planes[2];
    VADRMPRIMESurfaceDescriptor surface_descriptor;
    bool surface_acquired;
};

static bool support_direct_rendering(struct hwmap *hwmap)
{
    const struct hwmap_params *params = &hwmap->params;

    bool direct_rendering = NGLI_HAS_ALL_FLAGS(params->image_layouts, NGLI_IMAGE_LAYOUT_NV12_BIT);

    if (direct_rendering && params->texture_mipmap_filter) {
        LOG(WARNING,
            "vaapi direct rendering does not support mipmapping: "
            "disabling direct rendering");
        direct_rendering = false;
    }

    return direct_rendering;
}

static int vaapi_init(struct hwmap *hwmap, struct nmd_frame *frame)
{
    struct hwmap_vaapi *vaapi = hwmap->hwmap_priv_data;

    const struct image_params image_params = {
        .width = (uint32_t)frame->width,
        .height = (uint32_t)frame->height,
        .layout = NGLI_IMAGE_LAYOUT_NV12,
        .color_scale = 1.f,
        .color_info = ngli_color_info_from_nopemd_frame(frame),
    };
    ngli_image_init(&hwmap->mapped_image, &image_params, vaapi->planes);

    hwmap->require_hwconv = !support_direct_rendering(hwmap);

    return 0;
}

static void vaapi_release_frame_resources(struct hwmap *hwmap)
{
    struct hwmap_vaapi *vaapi = hwmap->hwmap_priv_data;

    if (vaapi->surface_acquired) {
        for (size_t i = 0; i < 2; i++) {
            hwmap->mapped_image.planes[i] = NULL;
            ngpu_texture_freep(&vaapi->planes[i]);
        }
        for (uint32_t i = 0; i < vaapi->surface_descriptor.num_objects; i++) {
            close(vaapi->surface_descriptor.objects[i].fd);
        }
        vaapi->surface_acquired = false;
    }

    nmd_frame_releasep(&vaapi->frame);
}

static void vaapi_uninit(struct hwmap *hwmap)
{
    vaapi_release_frame_resources(hwmap);
}

static int vaapi_map_frame(struct hwmap *hwmap, struct nmd_frame *frame)
{
    struct ngl_ctx *ctx = hwmap->ctx;
    struct vaapi_ctx *vaapi_ctx = &ctx->vaapi_ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct hwmap_vaapi *vaapi = hwmap->hwmap_priv_data;

    vaapi_release_frame_resources(hwmap);
    vaapi->frame = frame;

    VASurfaceID surface_id = (VASurfaceID)(intptr_t)frame->datap[0];
    VAStatus status = vaExportSurfaceHandle(vaapi_ctx->va_display,
                                            surface_id,
                                            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                            VA_EXPORT_SURFACE_READ_ONLY |
                                            VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                            &vaapi->surface_descriptor);
    if (status != VA_STATUS_SUCCESS) {
        LOG(ERROR, "failed to export vaapi surface handle: %s", vaErrorStr(status));
        return NGL_ERROR_EXTERNAL;
    }
    vaapi->surface_acquired = true;

    status = vaSyncSurface(vaapi_ctx->va_display, surface_id);
    if (status != VA_STATUS_SUCCESS)
        LOG(WARNING, "failed to sync surface: %s", vaErrorStr(status));

    struct format_desc desc;
    int ret = vaapi_get_format_desc(vaapi->surface_descriptor.fourcc, &desc);
    if (ret < 0)
        return ret;

    const size_t nb_layers = vaapi->surface_descriptor.num_layers;
    if (nb_layers != desc.nb_planes) {
        LOG(ERROR, "surface layer count (%zu) does not match plane count (%zu)",
            nb_layers, desc.nb_planes);
        return NGL_ERROR_UNSUPPORTED;
    }

    for (size_t i = 0; i < nb_layers; i++) {
        const enum ngpu_format format = desc.formats[i];
        const uint32_t width = (uint32_t)(i == 0 ? frame->width : NGLI_CEIL_RSHIFT(frame->width, desc.log2_chroma_width));
        const uint32_t height = (uint32_t)(i == 0 ? frame->height : NGLI_CEIL_RSHIFT(frame->height, desc.log2_chroma_height));

        const uint32_t id = vaapi->surface_descriptor.layers[i].object_index[0];
        const int fd = vaapi->surface_descriptor.objects[id].fd;
        const uint32_t size = vaapi->surface_descriptor.objects[id].size;
        const uint32_t offset = vaapi->surface_descriptor.layers[i].offset[0];
        const uint32_t pitch = vaapi->surface_descriptor.layers[i].pitch[0];
        const uint64_t drm_format_modifier = vaapi->surface_descriptor.objects[id].drm_format_modifier;

        const struct ngpu_texture_params texture_params = {
            .type = NGPU_TEXTURE_TYPE_2D,
            .format = format,
            .width = (uint32_t)width,
            .height = (uint32_t)height,
            .min_filter = hwmap->params.texture_min_filter,
            .mag_filter = hwmap->params.texture_mag_filter,
            .usage = NGPU_TEXTURE_USAGE_SAMPLED_BIT,
            .import_params = {
                .type = NGPU_IMPORT_TYPE_DMA_BUF,
                .dma_buf = {
                    .fd = fd,
                    .size = size,
                    .offset = offset,
                    .pitch = pitch,
                    .drm_format = vaapi->surface_descriptor.layers[i].drm_format,
                    .drm_format_mod = drm_format_modifier,
                },
            },
        };

        vaapi->planes[i] = ngpu_texture_create(gpu_ctx);
        if (!vaapi->planes[i])
            return NGL_ERROR_MEMORY;

        ret = ngpu_texture_init(vaapi->planes[i], &texture_params);
        if (ret < 0)
            return ret;

        hwmap->mapped_image.planes[i] = vaapi->planes[i];
    }

    return 0;
}

const struct hwmap_class ngli_hwmap_vaapi_vk_class = {
    .name      = "vaapi (dma buf → vk image)",
    .hwformat  = NMD_PIXFMT_VAAPI,
    .layouts   = (const enum image_layout[]){
        NGLI_IMAGE_LAYOUT_NV12,
        NGLI_IMAGE_LAYOUT_NONE
    },
    .flags     = HWMAP_FLAG_FRAME_OWNER,
    .priv_size = sizeof(struct hwmap_vaapi),
    .init      = vaapi_init,
    .map_frame = vaapi_map_frame,
    .uninit    = vaapi_uninit,
};
