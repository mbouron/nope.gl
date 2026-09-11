/*
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

#ifndef IMAGE_H
#define IMAGE_H

#include <stdbool.h>
#include <nopemd.h>

#include <ngpu/ngpu.h>
#include "utils/utils.h"

#define NGLI_COLOR_INFO_DEFAULTS {             \
    .space     = NMD_COL_SPC_UNSPECIFIED,      \
    .range     = NMD_COL_RNG_UNSPECIFIED,      \
    .primaries = NMD_COL_PRI_UNSPECIFIED,      \
    .transfer  = NMD_COL_TRC_UNSPECIFIED,      \
}                                              \

struct color_info {
    int space;
    int range;
    int primaries;
    int transfer;
};

struct color_info ngli_color_info_from_nopemd_frame(const struct nmd_frame *frame);

enum ngli_image_layout {
    NGLI_IMAGE_LAYOUT_NONE           = NGPU_IMAGE_LAYOUT_NONE,
    NGLI_IMAGE_LAYOUT_DEFAULT        = NGPU_IMAGE_LAYOUT_DEFAULT,
    NGLI_IMAGE_LAYOUT_MEDIACODEC     = NGPU_IMAGE_LAYOUT_MEDIACODEC,
    NGLI_IMAGE_LAYOUT_NV12           = NGPU_IMAGE_LAYOUT_NV12,
    NGLI_IMAGE_LAYOUT_NV12_RECTANGLE = NGPU_IMAGE_LAYOUT_NV12_RECTANGLE,
    NGLI_IMAGE_LAYOUT_YUV            = NGPU_IMAGE_LAYOUT_YUV,
    NGLI_IMAGE_LAYOUT_RECTANGLE      = NGPU_IMAGE_LAYOUT_RECTANGLE,
    NGLI_NB_IMAGE_LAYOUTS
};

enum {
    NGLI_IMAGE_LAYOUT_DEFAULT_BIT        = 1U << NGLI_IMAGE_LAYOUT_DEFAULT,
    NGLI_IMAGE_LAYOUT_MEDIACODEC_BIT     = 1U << NGLI_IMAGE_LAYOUT_MEDIACODEC,
    NGLI_IMAGE_LAYOUT_NV12_BIT           = 1U << NGLI_IMAGE_LAYOUT_NV12,
    NGLI_IMAGE_LAYOUT_NV12_RECTANGLE_BIT = 1U << NGLI_IMAGE_LAYOUT_NV12_RECTANGLE,
    NGLI_IMAGE_LAYOUT_YUV_BIT            = 1U << NGLI_IMAGE_LAYOUT_YUV,
    NGLI_IMAGE_LAYOUT_RECTANGLE_BIT      = 1U << NGLI_IMAGE_LAYOUT_RECTANGLE,
    NGLI_IMAGE_LAYOUT_ALL_BIT            = 0xFF,
};

struct ngli_image_params {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    enum ngli_image_layout layout;
    struct ngpu_texture *planes[4];
    void *samplers[4];
    float color_scale;
    struct color_info color_info;
    struct ngli_mat4 color_matrix;
    struct ngli_mat4 mapping_color_matrix;
    struct ngli_mat4 coordinates_matrix;
    float ts;
};

struct ngli_image_color_cache {
    bool initialized;
    enum ngli_image_layout layout;
    struct color_info color_info;
    float color_scale;
    struct ngli_mat4 color_matrix;
    struct ngli_mat4 mapping_color_matrix;
};

bool ngli_image_color_cache_update(struct ngli_image_color_cache *s, enum ngli_image_layout layout,
                                  const struct color_info *color_info, float color_scale);

struct ngli_image;

struct ngli_image *ngli_image_create(const struct ngli_image_params *params);
struct ngli_image *ngli_image_ref(const struct ngli_image *s);
void ngli_image_unrefp(struct ngli_image **sp);

const struct ngli_image_params *ngli_image_get_params(const struct ngli_image *s);

/*
 * Images are otherwise immutable once published. RTT, offscreen canvas and blur
 * producers still patch the coordinates matrix of an image their consumers
 * already hold. This is safe because a pipeline re-uploads the image metadata
 * block on every execution instead of caching it alongside the texture binding.
 */
void ngli_image_set_coordinates_matrix(struct ngli_image *s, const struct ngli_mat4 *matrix);

#endif
