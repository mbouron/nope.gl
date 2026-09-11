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

#include <string.h>
#include <nopemd.h>

#include "colorconv.h"
#include "image.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "utils/memory.h"
#include "utils/refcount.h"
#include "utils/utils.h"

struct color_info ngli_color_info_from_nopemd_frame(const struct nmd_frame *frame)
{
    return (struct color_info){
        .space     = frame->color_space,
        .range     = frame->color_range,
        .primaries = frame->color_primaries,
        .transfer  = frame->color_trc,
    };
}

static const size_t nb_planes_map[] = {
    [NGLI_IMAGE_LAYOUT_NONE]           = 0,
    [NGLI_IMAGE_LAYOUT_DEFAULT]        = 1,
    [NGLI_IMAGE_LAYOUT_MEDIACODEC]     = 1,
    [NGLI_IMAGE_LAYOUT_NV12]           = 2,
    [NGLI_IMAGE_LAYOUT_NV12_RECTANGLE] = 2,
    [NGLI_IMAGE_LAYOUT_YUV]            = 3,
    [NGLI_IMAGE_LAYOUT_RECTANGLE]      = 1,
};

NGLI_STATIC_ASSERT(NGLI_ARRAY_NB(nb_planes_map) == NGLI_NB_IMAGE_LAYOUTS, "nb planes map");

struct ngli_image {
    struct ngli_rc rc;
    struct ngli_image_params params;
    size_t nb_planes;
};

NGLI_RC_CHECK_STRUCT(ngli_image);

static const struct ngli_image empty_image = {
    .params = {
        .color_matrix       = {.m = NGLI_MAT4_IDENTITY},
        .coordinates_matrix = {.m = NGLI_MAT4_IDENTITY},
    },
};

bool ngli_image_color_cache_update(struct ngli_image_color_cache *s, enum ngli_image_layout layout,
                                  const struct color_info *color_info, float color_scale)
{
    if (s->initialized && s->layout == layout && s->color_scale == color_scale &&
        s->color_info.space == color_info->space &&
        s->color_info.range == color_info->range &&
        s->color_info.primaries == color_info->primaries &&
        s->color_info.transfer == color_info->transfer)
        return false;

    s->layout = layout;
    s->color_info = *color_info;
    s->color_scale = color_scale;
    s->color_matrix = empty_image.params.color_matrix;
    if (layout == NGLI_IMAGE_LAYOUT_NV12 ||
        layout == NGLI_IMAGE_LAYOUT_NV12_RECTANGLE ||
        layout == NGLI_IMAGE_LAYOUT_YUV)
        s->color_matrix = ngli_colorconv_get_ycbcr_to_rgb_color_matrix(color_info, color_scale);
    s->mapping_color_matrix = layout == NGLI_IMAGE_LAYOUT_NONE ? (struct ngli_mat4){0}
        : ngli_colorconv_get_mapping_color_matrix(color_info, NMD_COL_PRI_BT709);
    s->initialized = true;
    return true;
}

static void image_freep(void **sp)
{
    struct ngli_image *s = *sp;
    for (size_t i = 0; i < s->nb_planes; i++)
        ngpu_texture_freep(&s->params.planes[i]);
    ngli_freep(sp);
}

struct ngli_image *ngli_image_create(const struct ngli_image_params *params)
{
    ngli_assert(params->layout >= NGLI_IMAGE_LAYOUT_NONE && params->layout < NGLI_NB_IMAGE_LAYOUTS);
    struct ngli_image *s = ngli_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->rc = NGLI_RC_CREATE(image_freep);
    s->params = *params;
    s->nb_planes = nb_planes_map[params->layout];
    /*
     * Planes past the layout's plane count are dropped. Catch the caller that
     * derives its parameters from ngli_image_get_params() on a missing image:
     * the empty parameters carry NGLI_IMAGE_LAYOUT_NONE, so patching a plane
     * into them would otherwise yield a plane-less image without any hint.
     */
    for (size_t i = s->nb_planes; i < NGLI_ARRAY_NB(params->planes); i++)
        ngli_assert(!params->planes[i]);
    for (size_t i = 0; i < NGLI_ARRAY_NB(s->params.planes); i++) {
        s->params.planes[i] = i < s->nb_planes && params->planes[i] ? ngpu_texture_ref(params->planes[i]) : NULL;
        s->params.samplers[i] = i < s->nb_planes ? params->samplers[i] : NULL;
    }
    return s;
}

struct ngli_image *ngli_image_ref(const struct ngli_image *s)
{
    return s ? NGLI_RC_REF(s) : NULL;
}

void ngli_image_unrefp(struct ngli_image **sp)
{
    NGLI_RC_UNREFP(sp);
}

const struct ngli_image_params *ngli_image_get_params(const struct ngli_image *s)
{
    return &(s ? s : &empty_image)->params;
}

void ngli_image_set_coordinates_matrix(struct ngli_image *s, const struct ngli_mat4 *matrix)
{
    ngli_assert(s);
    s->params.coordinates_matrix = *matrix;
}
