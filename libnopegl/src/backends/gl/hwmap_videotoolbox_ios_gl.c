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

#include <CoreVideo/CoreVideo.h>

#include "hwmap.h"
#include "image.h"
#include "internal.h"
#include "log.h"
#include <ngpu/ngpu.h>
#include "nopegl/nopegl.h"
#include "utils/memory.h"

struct format_desc {
    enum image_layout layout;
    size_t nb_planes;
    struct {
        enum ngpu_format format;
    } planes[2];
};

static int vt_get_format_desc(OSType format, struct format_desc *desc)
{
    switch (format) {
    case kCVPixelFormatType_32BGRA:
        desc->layout = NGLI_IMAGE_LAYOUT_DEFAULT;
        desc->nb_planes = 1;
        desc->planes[0].format = NGPU_FORMAT_B8G8R8A8_UNORM;
        break;
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        desc->layout = NGLI_IMAGE_LAYOUT_NV12;
        desc->nb_planes = 2;
        desc->planes[0].format = NGPU_FORMAT_R8_UNORM;
        desc->planes[1].format = NGPU_FORMAT_R8G8_UNORM;
        break;
    default:
        LOG(ERROR, "unsupported pixel format %u", (uint32_t)format);
        return NGL_ERROR_UNSUPPORTED;
    }

    return 0;
}

struct hwmap_vt_ios {
    struct ngpu_texture *planes[2];
    OSType format;
    struct format_desc format_desc;
};

static int vt_ios_map_plane(struct hwmap *hwmap, CVPixelBufferRef cvpixbuf, size_t index)
{
    struct ngl_ctx *ctx = hwmap->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct hwmap_vt_ios *vt = hwmap->hwmap_priv_data;

    size_t width  = CVPixelBufferGetWidthOfPlane(cvpixbuf, index);
    size_t height = CVPixelBufferGetHeightOfPlane(cvpixbuf, index);
    if (width > INT_MAX || height > INT_MAX)
        return NGL_ERROR_LIMIT_EXCEEDED;

    struct ngpu_texture_params texture_params = {
        .type   = NGPU_TEXTURE_TYPE_2D,
        .format = vt->format_desc.planes[index].format,
        .width  = (uint32_t)width,
        .height = (uint32_t)height,
        .min_filter = hwmap->params.texture_min_filter,
        .mag_filter = hwmap->params.texture_mag_filter,
        .usage  = NGPU_TEXTURE_USAGE_SAMPLED_BIT,
        .import_params = {
            .corevideo_buffer = {
                .corevideo_buffer = cvpixbuf,
                .plane = (uint32_t)index,
            },
        },
    };

    vt->planes[index] = ngpu_texture_create(gpu_ctx);
    if (!vt->planes[index])
        return NGL_ERROR_MEMORY;

    int ret = ngpu_texture_init(vt->planes[index], &texture_params);
    if (ret < 0)
        return ret;

    hwmap->mapped_image.planes[index] = vt->planes[index];

    return 0;
}

static int vt_ios_map_frame(struct hwmap *hwmap, struct nmd_frame *frame)
{
    struct hwmap_vt_ios *vt = hwmap->hwmap_priv_data;

    CVPixelBufferRef cvpixbuf = (CVPixelBufferRef)frame->datap[0];
    OSType cvformat = CVPixelBufferGetPixelFormatType(cvpixbuf);
    ngli_assert(vt->format == cvformat);

    for (size_t i = 0; i < NGLI_ARRAY_NB(vt->planes); i++) {
        ngpu_texture_freep(&vt->planes[i]);
    }

    for (size_t i = 0; i < vt->format_desc.nb_planes; i++) {
        int ret = vt_ios_map_plane(hwmap, cvpixbuf, i);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static void vt_ios_uninit(struct hwmap *hwmap)
{
    struct hwmap_vt_ios *vt = hwmap->hwmap_priv_data;

    for (size_t i = 0; i < NGLI_ARRAY_NB(vt->planes); i++) {
        ngpu_texture_freep(&vt->planes[i]);
    }
}

static int support_direct_rendering(struct hwmap *hwmap, struct nmd_frame *frame)
{
    const struct hwmap_params *params = &hwmap->params;

    CVPixelBufferRef cvpixbuf = (CVPixelBufferRef)frame->datap[0];
    OSType cvformat = CVPixelBufferGetPixelFormatType(cvpixbuf);
    int direct_rendering = 1;

    switch (cvformat) {
    case kCVPixelFormatType_32BGRA:
        break;
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        direct_rendering = NGLI_HAS_ALL_FLAGS(params->image_layouts, NGLI_IMAGE_LAYOUT_NV12_BIT);
        break;
    default:
        ngli_assert(0);
    }

    if (direct_rendering && params->texture_mipmap_filter) {
        LOG(WARNING, "Videotoolbox textures do not support mipmapping: "
            "disabling direct rendering");
        direct_rendering = 0;
    }

    return direct_rendering;
}

static int vt_ios_init(struct hwmap *hwmap, struct nmd_frame *frame)
{
    struct hwmap_vt_ios *vt = hwmap->hwmap_priv_data;

    CVPixelBufferRef cvpixbuf = (CVPixelBufferRef)frame->datap[0];
    vt->format = CVPixelBufferGetPixelFormatType(cvpixbuf);

    int ret = vt_get_format_desc(vt->format, &vt->format_desc);
    if (ret < 0)
        return ret;

    const struct image_params image_params = {
        .width = (uint32_t)frame->width,
        .height = (uint32_t)frame->height,
        .layout = vt->format_desc.layout,
        .color_scale = 1.f,
        .color_info = ngli_color_info_from_nopemd_frame(frame),
    };
    ngli_image_init(&hwmap->mapped_image, &image_params, vt->planes);

    hwmap->require_hwconv = !support_direct_rendering(hwmap, frame);

    return 0;
}

const struct hwmap_class ngli_hwmap_vt_ios_gl_class = {
    .name      = "videotoolbox (zero-copy)",
    .hwformat  = NMD_PIXFMT_VT,
    .layouts   = (const enum image_layout[]){
        NGLI_IMAGE_LAYOUT_DEFAULT,
        NGLI_IMAGE_LAYOUT_NV12,
        NGLI_IMAGE_LAYOUT_NONE
    },
    .priv_size = sizeof(struct hwmap_vt_ios),
    .init      = vt_ios_init,
    .map_frame = vt_ios_map_frame,
    .uninit    = vt_ios_uninit,
};
