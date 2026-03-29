/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023 Nope Forge
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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
#include <string.h>

#include "fgblur.h"
#include "internal.h"
#include "log.h"
#include <ngpu/ngpu.h>
#include "pipeline_compat.h"
#include "rtt.h"
#include "utils/bits.h"
#include "utils/utils.h"

/* GLSL shaders */
#include "blur_common_vert.h"
#include "blur_downsample_frag.h"
#include "blur_upsample_frag.h"
#include "blur_interpolate_frag.h"

struct down_up_data_block {
    float offset;
};

struct interpolate_block {
    float lod;
};

static int setup_down_up_pipeline(struct ngpu_pgcraft *crafter,
                                  const char *name,
                                  const char *frag_base,
                                  struct pipeline_compat *pipeline,
                                  const struct ngpu_rendertarget_layout *layout,
                                  struct ngpu_block *block)
{
    const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_texture textures[] = {
        {
            .name      = "tex",
            .type      = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .precision = NGPU_PRECISION_HIGH,
            .stage     = NGPU_PROGRAM_STAGE_FRAG,
        },
    };

    const struct ngpu_pgcraft_block blocks[] = {
        {
            .name          = "data",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &block->block_desc,
            .buffer        = {
                .buffer    = block->buffer,
                .size      = block->block_size,
            },
        }
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = name,
        .vert_base        = blur_common_vert,
        .frag_base        = frag_base,
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .blocks           = blocks,
        .nb_blocks        = NGLI_ARRAY_NB(blocks),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    int ret = ngpu_pgcraft_craft(crafter, &crafter_params);
    if (ret < 0)
        return ret;

    const struct pipeline_compat_params params = {
        .type         = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics     = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state        = NGPU_GRAPHICS_STATE_DEFAULTS,
            .rt_layout    = *layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(crafter),
        },
        .program          = ngpu_pgcraft_get_program(crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(crafter),
        .resources        = ngpu_pgcraft_get_bindgroup_resources(crafter),
        .vertex_resources = ngpu_pgcraft_get_vertex_resources(crafter),
        .compat_info      = ngpu_pgcraft_get_compat_info(crafter),
    };

    return ngli_pipeline_compat_init(pipeline, &params);
}

static int setup_interpolate_pipeline(struct fgblur_ctx *s, struct ngpu_ctx *gpu_ctx,
                                      const struct ngpu_rendertarget_layout *dst_layout)
{
    const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
    };

    struct ngpu_pgcraft_texture textures[] = {
        {.name = "tex0", .type = NGPU_PGCRAFT_TEXTURE_TYPE_2D, .precision = NGPU_PRECISION_HIGH, .stage = NGPU_PROGRAM_STAGE_FRAG},
        {.name = "tex1", .type = NGPU_PGCRAFT_TEXTURE_TYPE_2D, .precision = NGPU_PRECISION_HIGH, .stage = NGPU_PROGRAM_STAGE_FRAG},
    };

    const struct ngpu_block_entry fields[] = {
        NGPU_BLOCK_FIELD(struct interpolate_block, lod, NGPU_TYPE_F32, 0),
    };
    const struct ngpu_block_params block_params = {
        .entries    = fields,
        .nb_entries = NGLI_ARRAY_NB(fields),
    };
    int ret = ngpu_block_init(gpu_ctx, &s->interpolate.block, &block_params);
    if (ret < 0)
        return ret;

    const struct ngpu_pgcraft_block crafter_blocks[] = {
        {
            .name          = "interpolate",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &s->interpolate.block.block_desc,
            .buffer        = {
                .buffer    = s->interpolate.block.buffer,
                .size      = s->interpolate.block.block_size,
            },
        }
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/fast-gaussian-blur-interpolate",
        .vert_base        = blur_common_vert,
        .frag_base        = blur_interpolate_frag,
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .blocks           = crafter_blocks,
        .nb_blocks        = NGLI_ARRAY_NB(crafter_blocks),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    ret = ngpu_pgcraft_craft(s->interpolate.crafter, &crafter_params);
    if (ret < 0)
        return ret;

    const struct pipeline_compat_params params = {
        .type         = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics     = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state        = NGPU_GRAPHICS_STATE_DEFAULTS,
            .rt_layout    = *dst_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(s->interpolate.crafter),
        },
        .program          = ngpu_pgcraft_get_program(s->interpolate.crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(s->interpolate.crafter),
        .resources        = ngpu_pgcraft_get_bindgroup_resources(s->interpolate.crafter),
        .vertex_resources = ngpu_pgcraft_get_vertex_resources(s->interpolate.crafter),
        .compat_info      = ngpu_pgcraft_get_compat_info(s->interpolate.crafter),
    };

    return ngli_pipeline_compat_init(s->interpolate.pl, &params);
}

int ngli_fgblur_init(struct fgblur_ctx *s, struct ngpu_ctx *gpu_ctx,
                     const struct ngpu_rendertarget_layout *mip_layout,
                     const struct ngpu_rendertarget_layout *dst_layout)
{
    memset(s, 0, sizeof(*s));
    s->mip_layout = *mip_layout;

    const struct ngpu_block_entry down_up_fields[] = {
        NGPU_BLOCK_FIELD(struct down_up_data_block, offset, NGPU_TYPE_F32, 0),
    };
    const struct ngpu_block_params down_up_params = {
        .entries    = down_up_fields,
        .nb_entries = NGLI_ARRAY_NB(down_up_fields),
    };
    int ret = ngpu_block_init(gpu_ctx, &s->down_up_block, &down_up_params);
    if (ret < 0)
        return ret;

    ret = ngpu_block_update(&s->down_up_block, 0, &(struct down_up_data_block){.offset=1.f});
    if (ret < 0)
        return ret;

    s->dws.crafter = ngpu_pgcraft_create(gpu_ctx);
    s->ups.crafter = ngpu_pgcraft_create(gpu_ctx);
    s->interpolate.crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->dws.crafter || !s->ups.crafter || !s->interpolate.crafter)
        return NGL_ERROR_MEMORY;

    s->dws.pl = ngli_pipeline_compat_create(gpu_ctx);
    s->ups.pl = ngli_pipeline_compat_create(gpu_ctx);
    s->interpolate.pl = ngli_pipeline_compat_create(gpu_ctx);
    if (!s->dws.pl || !s->ups.pl || !s->interpolate.pl)
        return NGL_ERROR_MEMORY;

    if ((ret = setup_down_up_pipeline(s->dws.crafter, "nopegl/fast-gaussian-blur-dws",
                                      blur_downsample_frag, s->dws.pl, mip_layout, &s->down_up_block)) < 0 ||
        (ret = setup_down_up_pipeline(s->ups.crafter, "nopegl/fast-gaussian-blur-ups",
                                      blur_upsample_frag, s->ups.pl, mip_layout, &s->down_up_block)) < 0)
        return ret;

    return setup_interpolate_pipeline(s, gpu_ctx, dst_layout);
}

int ngli_fgblur_resize(struct fgblur_ctx *s, struct ngl_ctx *ctx,
                       uint32_t width, uint32_t height,
                       enum ngpu_format format)
{
    if (s->width == width && s->height == height && s->mips[0])
        return 0;

    const struct ngpu_texture_params mip_tex_params = {
        .type       = NGPU_TEXTURE_TYPE_2D,
        .format     = format,
        .width      = width,
        .height     = height,
        .min_filter = NGPU_FILTER_LINEAR,
        .mag_filter = NGPU_FILTER_LINEAR,
        .wrap_s     = NGPU_WRAP_MIRRORED_REPEAT,
        .wrap_t     = NGPU_WRAP_MIRRORED_REPEAT,
        .usage      = NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
                      NGPU_TEXTURE_USAGE_SAMPLED_BIT,
    };

    int ret;
    struct ngpu_texture_params p = mip_tex_params;

    /* Create full-res mip for upsample result */
    ngli_rtt_freep(&s->mip);
    s->mip = ngli_rtt_create(ctx);
    if (!s->mip)
        return NGL_ERROR_MEMORY;
    ret = ngli_rtt_from_texture_params(s->mip, &p);
    if (ret < 0)
        return ret;

    /* Create mip pyramid */
    uint32_t mip_w = width;
    uint32_t mip_h = height;
    for (size_t i = 0; i < NGLI_FGBLUR_MAX_MIP_LEVELS; i++) {
        ngli_rtt_freep(&s->mips[i]);
        s->mips[i] = ngli_rtt_create(ctx);
        if (!s->mips[i])
            return NGL_ERROR_MEMORY;

        p.width = mip_w;
        p.height = mip_h;
        ret = ngli_rtt_from_texture_params(s->mips[i], &p);
        if (ret < 0)
            return ret;

        mip_w = NGLI_MAX(mip_w >> 1, 1);
        mip_h = NGLI_MAX(mip_h >> 1, 1);
    }

    /* Create destination RTT (interpolation output) */
    ngli_rtt_freep(&s->dst_rtt);
    s->dst_rtt = ngli_rtt_create(ctx);
    if (!s->dst_rtt)
        return NGL_ERROR_MEMORY;

    p = mip_tex_params;
    p.width = width;
    p.height = height;
    ret = ngli_rtt_from_texture_params(s->dst_rtt, &p);
    if (ret < 0)
        return ret;

    s->width = width;
    s->height = height;

    const uint32_t nb_mips = ngli_log2(NGLI_MAX(width, height)) + 1;
    s->max_lod = NGLI_MIN(nb_mips - 1, NGLI_FGBLUR_MAX_MIP_LEVELS - 2);

    return 0;
}

/*
 * Compute the lod level from the radius.
 *
 * The formula used below is the result of a logarithmic fit to a serie of
 * points (x, y) where x represents the blur radius and y the associated lod
 * level of each generated mip.
 *
 * To generate the serie of points, we measured for each lod level the blur
 * radius by comparing visually the corresponding mip and the output of a
 * gaussian blur performed by GIMP at different radii. For reference here are
 * the list of points:
 *   (4.45, 1), (12.92, 2), (22.97, 3), (50, 4), (100, 5)
 * which can be approximated by:
 *   1.34508f * logf(0.406057f * radius) for x > 5.17925
 *   radius / 5.17925 for x <= 5.17925
 *
 * While far from perfect, this approximation is considered good enough for now
 * as it provides close enough results to a regular gaussian blur.
 */
static float compute_lod(float radius)
{
    const float k = 5.17925f;
    if (radius <= k)
        return radius / k;
    return 1.34508f * logf(0.406057f * radius);
}

static void execute_pass(struct ngl_ctx *ctx,
                         struct rtt_ctx *rtt_ctx,
                         struct pipeline_compat *pipeline,
                         const struct image *image)
{
    ngli_rtt_begin(rtt_ctx);
    ngpu_ctx_begin_render_pass(ctx->gpu_ctx, ctx->current_rendertarget);
    ngli_pipeline_compat_update_image(pipeline, 0, image);
    ngli_pipeline_compat_draw(pipeline, 3, 1, 0);
    ngli_rtt_end(rtt_ctx);
}

const struct image *ngli_fgblur_execute(struct fgblur_ctx *s, struct ngl_ctx *ctx,
                                        const struct image *src_image,
                                        float blurriness,
                                        uint32_t width, uint32_t height)
{
    const float diagonal = hypotf((float)width, (float)height);
    const float radius = blurriness * diagonal / 2.f;
    const float lod = NGLI_MIN(compute_lod(radius), (float)s->max_lod);
    const int32_t lod_i = (int32_t)lod;
    const float lod_f = lod - (float)lod_i;

    /* Downsample source to mips[1] */
    const struct image *mip = src_image;
    execute_pass(ctx, s->mips[1], s->dws.pl, mip);

    /* Downsample successively until mips[lod_i+1] is generated */
    for (int32_t i = 2; i <= lod_i + 1; i++)
        execute_pass(ctx, s->mips[i], s->dws.pl, ngli_rtt_get_image(s->mips[i - 1], 0));

    /*
     * Upsample successively from mips[lod_i] back to full resolution.
     * If lod == 0, we simply use the source.
     */
    if (lod_i > 0) {
        for (int32_t i = lod_i - 1; i > 0; i--)
            execute_pass(ctx, s->mips[i], s->ups.pl, ngli_rtt_get_image(s->mips[i + 1], 0));
        execute_pass(ctx, s->mip, s->ups.pl, ngli_rtt_get_image(s->mips[1], 0));
        mip = ngli_rtt_get_image(s->mip, 0);
    }

    /* Upsample from mips[lod_i+1] back to full resolution in mips[0] */
    for (int32_t i = lod_i; i >= 0; i--)
        execute_pass(ctx, s->mips[i], s->ups.pl, ngli_rtt_get_image(s->mips[i + 1], 0));

    /* Interpolate the two blurred layers */
    const struct interpolate_block ib = {.lod = lod_f};
    ngpu_block_update(&s->interpolate.block, 0, &ib);

    ngli_rtt_begin(s->dst_rtt);
    ngpu_ctx_begin_render_pass(ctx->gpu_ctx, ctx->current_rendertarget);
    ngli_pipeline_compat_update_image(s->interpolate.pl, 0, mip);
    ngli_pipeline_compat_update_image(s->interpolate.pl, 1, ngli_rtt_get_image(s->mips[0], 0));
    ngli_pipeline_compat_draw(s->interpolate.pl, 3, 1, 0);
    ngli_rtt_end(s->dst_rtt);

    return ngli_rtt_get_image(s->dst_rtt, 0);
}

void ngli_fgblur_release(struct fgblur_ctx *s)
{
    ngli_rtt_freep(&s->mip);
    for (size_t i = 0; i < NGLI_FGBLUR_MAX_MIP_LEVELS; i++)
        ngli_rtt_freep(&s->mips[i]);
    ngli_rtt_freep(&s->dst_rtt);
    s->width = 0;
    s->height = 0;
}

void ngli_fgblur_uninit(struct fgblur_ctx *s)
{
    ngli_fgblur_release(s);

    ngli_pipeline_compat_freep(&s->dws.pl);
    ngpu_pgcraft_freep(&s->dws.crafter);
    ngli_pipeline_compat_freep(&s->ups.pl);
    ngpu_pgcraft_freep(&s->ups.crafter);
    ngpu_block_reset(&s->down_up_block);

    ngli_pipeline_compat_freep(&s->interpolate.pl);
    ngpu_pgcraft_freep(&s->interpolate.crafter);
    ngpu_block_reset(&s->interpolate.block);
}
