/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023 Nope Forge
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

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "colorconv.h"
#include "hwconv.h"
#include "image.h"
#include "internal.h"
#include "log.h"
#include <nopegpu/nopegpu.h>
#include "pipeline.h"
#include <nopegpu/nopegpu.h>
#include "utils/utils.h"

/* GLSL fragments as string */
#include "hdr_hlg2sdr_frag.h"
#include "hdr_pq2sdr_frag.h"
#include "hwconv_frag.h"
#include "hwconv_vert.h"

static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
    {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
};

/* PQ signal ranges of the EETF, see hdr_pq2sdr.frag */
struct eetf_block {
    float src_black;
    float src_peak;
    float dst_black;
    float dst_peak;
};

/*
 * Parameters of the HDR->SDR conversion, see:
 * - hdr_hlg2sdr.frag
 * - hdr_pq2sdr.frag
 */
struct hdr_block {
    struct ngli_mat4 primaries;
    struct eetf_block eetf;
};

/* PQ inverse EOTF (luminance in cd/m² → non-linear signal), ITU-R BT.2100 */
static float pq_oetf(float luminance)
{
    const double m1 = 0.1593017578125;
    const double m2 = 78.84375;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;
    const double ym = pow(luminance / 10000.0, m1);
    return (float)pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
}

/*
 * The source range is the one of the mastering display and content light
 * level (the entire PQ range if unknown), the target range is the HLG one
 */
static struct eetf_block get_eetf_params(const struct color_info *info)
{
    const bool has_range = info->max_luminance > info->min_luminance && info->min_luminance >= 0.f;
    const float src_black = pq_oetf(has_range ? info->min_luminance : 0.f);
    const float src_peak = pq_oetf(has_range ? info->max_luminance : 10000.f);
    return (struct eetf_block){
        .src_black = src_black,
        .src_peak  = src_peak,
        .dst_black = (pq_oetf(0.f) - src_black) / (src_peak - src_black),
        .dst_peak  = (pq_oetf(1000.f) - src_black) / (src_peak - src_black),
    };
}

/* The HDR frames without primaries are assumed to be BT.2020 */
static int get_hdr_primaries(int primaries)
{
    if (primaries == NMD_COL_PRI_UNSPECIFIED ||
        primaries == NMD_COL_PRI_RESERVED0 ||
        primaries == NMD_COL_PRI_RESERVED)
        return NMD_COL_PRI_BT2020;
    return primaries;
}

static struct hdr_block get_hdr_params(const struct hwconv *hwconv, const struct color_info *info)
{
    const int primaries = get_hdr_primaries(info->primaries);
    struct hdr_block params = {
        .primaries = ngli_colorconv_get_primaries_color_matrix(primaries, NMD_COL_PRI_BT2020),
    };
    if (hwconv->type == HWCONV_TYPE_PQ)
        params.eetf = get_eetf_params(info);
    return params;
}

/* Size of the parameters of the conversion: the EETF ranges are PQ only */
static size_t get_hdr_params_size(const struct hwconv *hwconv)
{
    return hwconv->type == HWCONV_TYPE_PQ ? sizeof(struct hdr_block) : offsetof(struct hdr_block, eetf);
}

/* Unsigned integer planes are mapped without 16-bit normalized textures, see hwmap_common.c */
static bool has_uint_planes(const struct ngli_image_params *params)
{
    if (params->layout != NGLI_IMAGE_LAYOUT_NV12 &&
        params->layout != NGLI_IMAGE_LAYOUT_YUV)
        return false;
    const struct ngpu_texture_params *plane_params = ngpu_texture_get_params(params->planes[0]);
    return plane_params->format == NGPU_FORMAT_R16_UINT;
}

int ngli_hwconv_init(struct hwconv *hwconv, struct ngl_ctx *ctx,
                     const struct ngli_image *dst_image,
                     const struct ngli_image_params *src_params)
{
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    hwconv->ctx = ctx;
    hwconv->src_params = *src_params;
    hwconv->block_index = -1;
    hwconv->input_image = ngli_image_resource_create();
    if (!hwconv->input_image)
        return NGL_ERROR_MEMORY;

    const struct ngli_image_params *dst_params = ngli_image_get_params(dst_image);
    if (dst_params->layout != NGLI_IMAGE_LAYOUT_DEFAULT) {
        LOG(ERROR, "unsupported output image layout: 0x%x", dst_params->layout);
        return NGL_ERROR_UNSUPPORTED;
    }

    struct ngpu_texture *texture = dst_params->planes[0];
    const struct ngpu_texture_params *texture_params = ngpu_texture_get_params(texture);

    const struct ngpu_rendertarget_layout rt_layout = {
        .nb_colors = 1,
        .colors[0].format = texture_params->format,
    };
    const struct ngpu_rendertarget_params rt_params = {
        .width = dst_params->width,
        .height = dst_params->height,
        .nb_colors = 1,
        .colors[0] = {
            .attachment = texture,
            .load_op    = NGPU_LOAD_OP_CLEAR,
            .store_op   = NGPU_STORE_OP_STORE,
        }
    };
    hwconv->rt = ngpu_rendertarget_create(gpu_ctx);
    if (!hwconv->rt)
        return NGL_ERROR_MEMORY;
    int ret = ngpu_rendertarget_init(hwconv->rt, &rt_params);
    if (ret < 0)
        return ret;

    const enum ngli_image_layout src_layout = src_params->layout;
    if (src_layout != NGLI_IMAGE_LAYOUT_DEFAULT &&
        src_layout != NGLI_IMAGE_LAYOUT_NV12 &&
        src_layout != NGLI_IMAGE_LAYOUT_YUV &&
        src_layout != NGLI_IMAGE_LAYOUT_MEDIACODEC) {
        LOG(ERROR, "unsupported texture layout: 0x%x", src_layout);
        return NGL_ERROR_UNSUPPORTED;
    }

    /* Integer planes are filtered like the converted texture */
    struct ngpu_pgcraft_texture textures[] = {
        {
            .name               = "tex",
            .type               = NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO,
            .stage              = NGPU_PROGRAM_STAGE_FRAG,
            .uint_planes        = has_uint_planes(src_params),
            .uint_planes_filter = texture_params->mag_filter,
        },
    };

    const int transfer = src_params->color_info.transfer;
    const char *vert_base = hwconv_vert;
    const char *frag_base = hwconv_frag;
    hwconv->type = HWCONV_TYPE_DEFAULT;
    if (transfer == NMD_COL_TRC_ARIB_STD_B67) {
        frag_base = hdr_hlg2sdr_frag;
        hwconv->type = HWCONV_TYPE_HLG;
    } else if (transfer == NMD_COL_TRC_SMPTE2084) {
        frag_base = hdr_pq2sdr_frag;
        hwconv->type = HWCONV_TYPE_PQ;
    }

    const char *block_name = NULL;
    ngpu_block_desc_init(gpu_ctx, &hwconv->block_desc, NGPU_BLOCK_LAYOUT_STD140);
    if (hwconv->type == HWCONV_TYPE_HLG || hwconv->type == HWCONV_TYPE_PQ) {
        ret = ngpu_block_desc_add_field(&hwconv->block_desc, "primaries", NGPU_TYPE_MAT4, 0);
        if (ret < 0)
            return ret;
        if (hwconv->type == HWCONV_TYPE_PQ) {
            static const struct ngpu_block_field fields[] = {
                {.name = "src_black", .type = NGPU_TYPE_F32},
                {.name = "src_peak",  .type = NGPU_TYPE_F32},
                {.name = "dst_black", .type = NGPU_TYPE_F32},
                {.name = "dst_peak",  .type = NGPU_TYPE_F32},
            };
            ret = ngpu_block_desc_add_fields(&hwconv->block_desc, fields, NGLI_ARRAY_NB(fields));
            if (ret < 0)
                return ret;
        }
        ngli_assert(ngpu_block_desc_get_size(&hwconv->block_desc, 0) == get_hdr_params_size(hwconv));
        block_name = "hdr_params";
    }

    struct ngpu_pgcraft_block block = {
        .type  = NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .stage = NGPU_PROGRAM_STAGE_FRAG,
        .block = &hwconv->block_desc,
    };
    if (block_name)
        snprintf(block.name, sizeof(block.name), "%s", block_name);

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/hwconv",
        .vert_base        = vert_base,
        .frag_base        = frag_base,
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .blocks           = &block,
        .nb_blocks        = block_name ? 1 : 0,
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    hwconv->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!hwconv->crafter)
        return NGL_ERROR_MEMORY;

    ret = ngpu_pgcraft_craft(hwconv->crafter, &crafter_params);
    if (ret < 0)
        return ret;

    if (block_name)
        hwconv->block_index = ngpu_pgcraft_get_block_index(hwconv->crafter, block_name, NGPU_PROGRAM_STAGE_FRAG);

    hwconv->pipeline = ngli_pipeline_create(gpu_ctx);
    if (!hwconv->pipeline)
        return NGL_ERROR_MEMORY;

    const struct ngli_pipeline_params params = {
        .type         = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics     = {
            .topology = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state    = NGPU_GRAPHICS_STATE_DEFAULTS,
            .rt_layout    = rt_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(hwconv->crafter),
        },
        .program          = ngpu_pgcraft_get_program(hwconv->crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(hwconv->crafter),
        .texture_infos    = ngpu_pgcraft_get_texture_infos(hwconv->crafter),
    };

    ret = ngli_pipeline_init(hwconv->pipeline, &params);
    if (ret < 0)
        return ret;

    ret = ngli_pipeline_set_image_source(hwconv->pipeline, 0, hwconv->input_image);
    if (ret < 0 && ret != NGL_ERROR_NOT_FOUND)
        return ret;
    return 0;
}

static void push_params(struct hwconv *hwconv, const struct ngli_image *image)
{
    struct ngl_ctx *ctx = hwconv->ctx;
    const struct hdr_block params = get_hdr_params(hwconv, &ngli_image_get_params(image)->color_info);
    const size_t size = get_hdr_params_size(hwconv);
    const size_t offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &params, size);
    struct ngpu_buffer *buffer = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
    ngli_pipeline_update_buffer(hwconv->pipeline, hwconv->block_index, buffer, offset, size);
}

int ngli_hwconv_convert_image(struct hwconv *hwconv, const struct ngli_image *image)
{
    const struct pipeline_execution execution = {.staging = hwconv->ctx->current_staging_buffer};

    struct ngl_ctx *ctx = hwconv->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    ngli_assert(hwconv->src_params.layout == ngli_image_get_params(image)->layout);

    struct ngpu_rendertarget *rt = hwconv->rt;
    struct ngli_pipeline *pipeline = hwconv->pipeline;

    if (hwconv->block_index >= 0)
        push_params(hwconv, image);

    ngpu_ctx_begin_render_pass(gpu_ctx, rt);

    ngli_image_resource_set(hwconv->input_image, image);
    int ret = ngli_pipeline_draw(pipeline, &execution, 3, 1, 0);
    ngli_image_resource_set(hwconv->input_image, NULL);

    ngpu_ctx_end_render_pass(gpu_ctx);

    return ret;
}

void ngli_hwconv_reset(struct hwconv *hwconv)
{
    struct ngl_ctx *ctx = hwconv->ctx;
    if (!ctx)
        return;

    ngli_pipeline_freep(&hwconv->pipeline);
    ngpu_pgcraft_freep(&hwconv->crafter);
    ngpu_rendertarget_freep(&hwconv->rt);
    ngpu_block_desc_reset(&hwconv->block_desc);

    ngli_image_resource_unrefp(&hwconv->input_image);
    memset(hwconv, 0, sizeof(*hwconv));
}
