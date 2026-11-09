/*
 * Copyright 2025-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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
#include <nopemedia/nopemedia.h>

#include "hwmap.h"
#include "image.h"
#include "internal.h"
#include "log.h"
#include "math_utils.h"
#include <nopegpu/nopegpu.h>
#include "nopegl/nopegl.h"

struct hwmap_common {
    struct ngli_image_color_cache color_cache;
    const struct format_desc *desc;
    int32_t width;
    int32_t height;
    size_t nb_planes;
    struct ngpu_texture *planes[4];
    bool uint_planes;
};

static const struct format_desc {
    enum ngli_image_layout layout;
    int depth;
    int shift;
    size_t nb_planes;
    int log2_chroma_width;
    int log2_chroma_height;
    int format_depth;
    enum ngpu_format formats[4];
    enum ngpu_format uint_formats[4];
} format_descs[] = {
    [NMD_PIXFMT_RGBA] = {
        .layout = NGLI_IMAGE_LAYOUT_DEFAULT,
        .depth = 8,
        .nb_planes = 1,
        .format_depth = 8,
        .formats[0] = NGPU_FORMAT_R8G8B8A8_UNORM,
    },
    [NMD_PIXFMT_BGRA] = {
        .layout = NGLI_IMAGE_LAYOUT_DEFAULT,
        .depth = 8,
        .nb_planes = 1,
        .format_depth = 8,
        .formats[0] = NGPU_FORMAT_B8G8R8A8_UNORM,
    },
    [NMD_SMPFMT_FLT] = {
        .layout = NGLI_IMAGE_LAYOUT_DEFAULT,
        .nb_planes = 1,
        .formats[0] = NGPU_FORMAT_R32_SFLOAT,
    },
    [NMD_PIXFMT_NV12] = {
        .layout = NGLI_IMAGE_LAYOUT_NV12,
        .depth = 8,
        .nb_planes = 2,
        .log2_chroma_width = 1,
        .log2_chroma_height = 1,
        .format_depth = 8,
        .formats[0] = NGPU_FORMAT_R8_UNORM,
        .formats[1] = NGPU_FORMAT_R8G8_UNORM,
    },
    [NMD_PIXFMT_YUV420P] = {
        .layout = NGLI_IMAGE_LAYOUT_YUV,
        .depth = 8,
        .nb_planes = 3,
        .log2_chroma_width = 1,
        .log2_chroma_height = 1,
        .format_depth = 8,
        .formats[0] = NGPU_FORMAT_R8_UNORM,
        .formats[1] = NGPU_FORMAT_R8_UNORM,
        .formats[2] = NGPU_FORMAT_R8_UNORM,
    },
    [NMD_PIXFMT_YUV422P] = {
        .layout = NGLI_IMAGE_LAYOUT_YUV,
        .depth = 8,
        .nb_planes = 3,
        .log2_chroma_width = 1,
        .log2_chroma_height = 0,
        .format_depth = 8,
        .formats[0] = NGPU_FORMAT_R8_UNORM,
        .formats[1] = NGPU_FORMAT_R8_UNORM,
        .formats[2] = NGPU_FORMAT_R8_UNORM,
    },
    [NMD_PIXFMT_YUV444P] = {
        .layout = NGLI_IMAGE_LAYOUT_YUV,
        .depth = 8,
        .nb_planes = 3,
        .log2_chroma_width = 0,
        .log2_chroma_height = 0,
        .format_depth = 8,
        .formats[0] = NGPU_FORMAT_R8_UNORM,
        .formats[1] = NGPU_FORMAT_R8_UNORM,
        .formats[2] = NGPU_FORMAT_R8_UNORM,
    },
    [NMD_PIXFMT_P010LE] = {
        .layout = NGLI_IMAGE_LAYOUT_NV12,
        .depth = 10,
        .shift = 6,
        .nb_planes = 2,
        .log2_chroma_width = 1,
        .log2_chroma_height = 1,
        .format_depth = 16,
        .formats[0] = NGPU_FORMAT_R16_UNORM,
        .formats[1] = NGPU_FORMAT_R16G16_UNORM,
        .uint_formats[0] = NGPU_FORMAT_R16_UINT,
        .uint_formats[1] = NGPU_FORMAT_R16G16_UINT,
    },
    [NMD_PIXFMT_YUV420P10LE] = {
        .layout = NGLI_IMAGE_LAYOUT_YUV,
        .depth = 10,
        .nb_planes = 3,
        .log2_chroma_width = 1,
        .log2_chroma_height = 1,
        .format_depth = 16,
        .formats[0] = NGPU_FORMAT_R16_UNORM,
        .formats[1] = NGPU_FORMAT_R16_UNORM,
        .formats[2] = NGPU_FORMAT_R16_UNORM,
        .uint_formats[0] = NGPU_FORMAT_R16_UINT,
        .uint_formats[1] = NGPU_FORMAT_R16_UINT,
        .uint_formats[2] = NGPU_FORMAT_R16_UINT,
    },
    [NMD_PIXFMT_YUV422P10LE] = {
        .layout = NGLI_IMAGE_LAYOUT_YUV,
        .depth = 10,
        .nb_planes = 3,
        .log2_chroma_width = 1,
        .log2_chroma_height = 0,
        .format_depth = 16,
        .formats[0] = NGPU_FORMAT_R16_UNORM,
        .formats[1] = NGPU_FORMAT_R16_UNORM,
        .formats[2] = NGPU_FORMAT_R16_UNORM,
        .uint_formats[0] = NGPU_FORMAT_R16_UINT,
        .uint_formats[1] = NGPU_FORMAT_R16_UINT,
        .uint_formats[2] = NGPU_FORMAT_R16_UINT,
    },
    [NMD_PIXFMT_YUV444P10LE] = {
        .layout = NGLI_IMAGE_LAYOUT_YUV,
        .depth = 10,
        .nb_planes = 3,
        .log2_chroma_width = 0,
        .log2_chroma_height = 0,
        .format_depth = 16,
        .formats[0] = NGPU_FORMAT_R16_UNORM,
        .formats[1] = NGPU_FORMAT_R16_UNORM,
        .formats[2] = NGPU_FORMAT_R16_UNORM,
        .uint_formats[0] = NGPU_FORMAT_R16_UINT,
        .uint_formats[1] = NGPU_FORMAT_R16_UINT,
        .uint_formats[2] = NGPU_FORMAT_R16_UINT,
    },
};

static const struct format_desc *common_get_format_desc(int pix_fmt)
{
    if (pix_fmt < 0 || pix_fmt >= NGLI_ARRAY_NB(format_descs))
        return NULL;

    const struct format_desc *desc = &format_descs[pix_fmt];
    if (desc->layout == NGLI_IMAGE_LAYOUT_NONE)
        return NULL;

    return desc;
}

static bool support_direct_rendering(struct hwmap *hwmap, const struct format_desc *desc)
{
    const struct hwmap_params *params = &hwmap->params;

    bool direct_rendering = true;
    if (desc->layout != NGLI_IMAGE_LAYOUT_DEFAULT) {
        direct_rendering = NGLI_HAS_ALL_FLAGS(params->image_layouts, (1 << desc->layout));
        if (params->texture_mipmap_filter != NGPU_MIPMAP_FILTER_NONE)
            direct_rendering = false;
    }

    return direct_rendering;
}

static bool are_plane_formats_supported(struct hwmap *hwmap, const struct format_desc *desc)
{
    struct ngpu_ctx *gpu_ctx = hwmap->ctx->gpu_ctx;
    const struct hwmap_params *params = &hwmap->params;

    uint32_t required = NGPU_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (params->texture_min_filter != NGPU_FILTER_NEAREST ||
        params->texture_mag_filter != NGPU_FILTER_NEAREST)
        required |= NGPU_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

    for (size_t i = 0; i < desc->nb_planes; i++) {
        const uint32_t features = ngpu_ctx_get_format_features(gpu_ctx, desc->formats[i]);
        if (!NGLI_HAS_ALL_FLAGS(features, required))
            return false;
    }
    return true;
}

static int common_init(struct hwmap *hwmap, struct nmd_frame *frame)
{
    struct ngl_ctx *ctx = hwmap->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    const struct hwmap_params *params = &hwmap->params;
    struct hwmap_common *common = hwmap->hwmap_priv_data;

    const struct format_desc *desc = common_get_format_desc(frame->pix_fmt);
    if (!desc) {
        LOG(ERROR, "unsupported nope.media pixel format (%d)", frame->pix_fmt);
        return NGL_ERROR_UNSUPPORTED;
    }

    common->desc = desc;
    common->width = frame->width;
    common->height = frame->height;
    common->nb_planes = desc->nb_planes;

    /*
     * If 16-bit normalized textures are not supported (e.g. OpenGL ES 3.0,
     * WebGL 2), fallback on unsigned integer textures.
     */
    if (!are_plane_formats_supported(hwmap, desc))
        common->uint_planes = desc->uint_formats[0] != NGPU_FORMAT_UNDEFINED;

    const enum ngpu_format *formats = common->uint_planes ? desc->uint_formats : desc->formats;
    const enum ngpu_filter min_filter = common->uint_planes ? NGPU_FILTER_NEAREST : params->texture_min_filter;
    const enum ngpu_filter mag_filter = common->uint_planes ? NGPU_FILTER_NEAREST : params->texture_mag_filter;

    for (size_t i = 0; i < common->nb_planes; i++) {
        const uint32_t width =  (uint32_t)(i == 0 ? frame->width : NGLI_CEIL_RSHIFT(frame->width, desc->log2_chroma_width));
        const uint32_t height = (uint32_t)(i == 0 ? frame->height : NGLI_CEIL_RSHIFT(frame->height, desc->log2_chroma_height));

        const struct ngpu_texture_params plane_params = {
            .type          = NGPU_TEXTURE_TYPE_2D,
            .format        = formats[i],
            .width         = width,
            .height        = height,
            .min_filter    = min_filter,
            .mag_filter    = mag_filter,
            .mipmap_filter = desc->layout == NGLI_IMAGE_LAYOUT_DEFAULT ? params->texture_mipmap_filter : NGPU_MIPMAP_FILTER_NONE,
            .wrap_s        = params->texture_wrap_s,
            .wrap_t        = params->texture_wrap_t,
            .usage         = params->texture_usage,
        };

        common->planes[i] = ngpu_texture_create(gpu_ctx);
        if (!common->planes[i])
            return NGL_ERROR_MEMORY;

        int ret = ngpu_texture_init(common->planes[i], &plane_params);
        if (ret < 0)
            return ret;
    }

    hwmap->require_hwconv = common->uint_planes || !support_direct_rendering(hwmap, desc);

    return 0;
}

static void common_uninit(struct hwmap *hwmap)
{
    struct hwmap_common *common = hwmap->hwmap_priv_data;

    for (size_t i = 0; i < NGLI_ARRAY_NB(common->planes); i++)
        ngpu_texture_freep(&common->planes[i]);
}

static int common_map_frame(struct hwmap *hwmap, struct nmd_frame *frame, struct ngli_image **imagep)
{
    struct hwmap_common *common = hwmap->hwmap_priv_data;

    for (size_t i = 0; i < common->nb_planes; i++) {
        struct ngpu_texture *plane = common->planes[i];
        const struct ngpu_texture_params *params = ngpu_texture_get_params(plane);
        if (frame->linesizep[i] < 0) {
            LOG(ERROR, "invalid linesize (%d) for plane %zu", frame->linesizep[i], i);
            return NGL_ERROR_UNSUPPORTED;
        }
        const uint32_t linesize = (uint32_t)frame->linesizep[i] / (uint32_t)ngpu_format_get_bytes_per_pixel(params->format);
        int ret = ngpu_texture_upload(plane, frame->datap[i], linesize);
        if (ret < 0)
            return ret;
    }

    const struct format_desc *desc = common->desc;
    const int src_max = ((1 << desc->depth) - 1) << desc->shift;
    const int dst_max = common->uint_planes ? 1 : (1 << desc->format_depth) - 1;
    const float color_scale = (float)dst_max / (float)src_max;

    const struct color_info color_info = ngli_color_info_from_nmd_frame(frame);
    ngli_image_color_cache_update(&common->color_cache, desc->layout, &color_info, desc->depth, color_scale);

    const struct ngli_image_params image_params = {
        .width                = (uint32_t)frame->width,
        .height               = (uint32_t)frame->height,
        .layout               = desc->layout,
        .planes               = {common->planes[0], common->planes[1], common->planes[2], common->planes[3]},
        .color_scale          = color_scale,
        .color_info           = color_info,
        .color_matrix         = common->color_cache.color_matrix,
        .mapping_color_matrix = common->color_cache.mapping_color_matrix,
        .coordinates_matrix   = {.m = NGLI_MAT4_IDENTITY},
        .ts                   = (float)frame->ts,
    };
    *imagep = ngli_image_create(&image_params);
    if (!*imagep)
        return NGL_ERROR_MEMORY;

    return 0;
}

const struct hwmap_class ngli_hwmap_common_class = {
    .name      = "default",
    .hwformat  = -1, /* TODO: replace with NMD_PIXFMT_NONE */
    .layouts   = (const enum ngli_image_layout[]){
        NGLI_IMAGE_LAYOUT_DEFAULT,
        NGLI_IMAGE_LAYOUT_NV12,
        NGLI_IMAGE_LAYOUT_YUV,
        NGLI_IMAGE_LAYOUT_NONE
    },
    .priv_size = sizeof(struct hwmap_common),
    .init      = common_init,
    .map_frame = common_map_frame,
    .uninit    = common_uninit,
};
