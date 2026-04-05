/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023 Nope Forge
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

#include "internal.h"
#include "log.h"
#include <ngpu/ngpu.h>
#include "node_texture.h"
#include "node_uniform.h"
#include "nopegl/nopegl.h"
#include "pipeline_compat.h"
#include "rtt.h"
#include <ngpu/ngpu.h>
#include "staging_buffer.h"
#include "utils/memory.h"
#include "utils/utils.h"

/* GLSL shaders */
#include "blur_gaussian_vert.h"
#include "blur_gaussian_frag.h"

#define _CONSTANT_TO_STR(v) #v
#define CONSTANT_TO_STR(v) _CONSTANT_TO_STR(v)

#define MAX_KERNEL_SIZE 127
NGLI_STATIC_ASSERT(MAX_KERNEL_SIZE & 0x1, "kernel size is odd");

#define MAX_RADIUS_SIZE 126
NGLI_STATIC_ASSERT(MAX_RADIUS_SIZE == (MAX_KERNEL_SIZE - 1), "radius size");

struct direction_block {
    float direction[2];
    float _pad[2];
};

struct kernel_block {
    float weights[2 * MAX_KERNEL_SIZE];
    int32_t nb_weights;
};

struct gblur_opts {
    struct ngl_node *source;
    struct ngl_node *destination;
    struct ngl_node *blurriness_node;
    float blurriness;
};

struct gblur_priv {
    uint32_t width;
    uint32_t height;
    float blurriness;

    /* Source image */
    struct image *image;
    size_t image_rev;

    /* Render the horizontal pass to a temporary destination */
    struct ngpu_rendertarget_layout tmp_layout;
    struct rtt_ctx *tmp;

    /* Render the vertical pass to the destination */
    int dst_is_resizable;
    struct ngpu_rendertarget_layout dst_layout;
    struct rtt_ctx *dst_rtt_ctx;

    struct ngpu_block_desc direction_block_desc;
    size_t direction_block_size;
    size_t direction_block_aligned_size;
    int32_t direction_block_index;

    struct ngpu_block_desc kernel_block_desc;
    size_t kernel_block_size;
    int32_t kernel_block_index;
    uint8_t *kernel_staging_cache;

    struct ngpu_pgcraft *crafter;
    struct pipeline_compat *pl_blur_h;
    struct pipeline_compat *pl_blur_v;
};

#define OFFSET(x) offsetof(struct gblur_opts, x)
static const struct node_param gblur_params[] = {
    {"source",            NGLI_PARAM_TYPE_NODE, OFFSET(source),
                          .node_types=(const uint32_t[]){NGL_NODE_TEXTURE2D, NGLI_NODE_NONE},
                          .flags=NGLI_PARAM_FLAG_NON_NULL | NGLI_PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                          .desc=NGLI_DOCSTRING("source to use for the blur")},
    {"destination",       NGLI_PARAM_TYPE_NODE, OFFSET(destination),
                          .node_types=(const uint32_t[]){NGL_NODE_TEXTURE2D, NGLI_NODE_NONE},
                          .flags=NGLI_PARAM_FLAG_NON_NULL | NGLI_PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                          .desc=NGLI_DOCSTRING("destination to use for the blur")},
    {"blurriness",        NGLI_PARAM_TYPE_F32, OFFSET(blurriness_node), {.f32=0.03f},
                          .flags=NGLI_PARAM_FLAG_ALLOW_NODE,
                          .desc=NGLI_DOCSTRING("amount of blurriness in the range [0,1] "
                                               "where 1 is equivalent of a blur radius of " CONSTANT_TO_STR(MAX_RADIUS_SIZE) "px")},
    {NULL}
};

#define O(i) (2 * (i))
#define W(i) (2 * (i) + 1)

static int update_kernel(struct ngl_node *node)
{
    struct gblur_priv *s = node->priv_data;
    const struct gblur_opts *o = node->opts;

    const float blurriness = *(float *)ngli_node_get_data_ptr(o->blurriness_node, &o->blurriness);
    if (blurriness < 0.0)
        return NGL_ERROR_INVALID_ARG;

    if (s->blurriness == blurriness)
        return 0;

    s->blurriness = blurriness;

    const float radius_f = NGLI_CLAMP(blurriness, 0.f, 1.f) * (float)MAX_RADIUS_SIZE;
    const int32_t radius_i = (int32_t)ceil(radius_f);
    const int32_t radius = NGLI_MIN(radius_i, MAX_RADIUS_SIZE);

    /*
     * Compute sigma for a given precision (1e-3f should be fine for up to
     * 10-bit image formats).
     * See:
     * - https://en.wikipedia.org/wiki/Talk%3AGaussian_blur#Radius_again
     * - https://en.wikipedia.org/wiki/68%E2%80%9395%E2%80%9399.7_rule
     */
    const float sigma = (radius_f + 1.f) / sqrtf(-2.f * logf(1e-3f));

    /*
     * Compute the weights for the interval [-radius, radius].
     *
     * Instead of evaluating the gaussian function to compute the weights, we
     * use an approximation of its integral based on the error function. This
     * avoids errors and undersampling for small sigma (< 0.8).
     * See:
     * - https://en.wikipedia.org/wiki/Error_function#Applications
     * - https://bartwronski.com/2021/10/31/practical-gaussian-filter-binomial-filter-and-small-sigma-gaussians
     */
    float weights[2 * MAX_KERNEL_SIZE];
    size_t nb_weights = 0;

    float sum = 0.0f;
    const float sig = sigma * sqrtf(2.f);
    for (int i = -radius; i <= radius; i++) {
        const float p1 = erff(((float)i - 0.5f) / sig);
        const float p2 = erff(((float)i + 0.5f) / sig);
        const float w = (p2 - p1) / 2.f;
        weights[nb_weights++] = w;
        sum += w;
    }
    for (size_t i = 0; i < nb_weights; i++)
        weights[i] /= (float)sum;

    /*
     * Compute offsets and weights to take advantage of hw filtering to reduce
     * the number of texture fetches from (2*radius + 1) to (radius + 1). The
     * resulting offsets and weights are stored in a vec2.
     */
    struct kernel_block kernel_data = {0};
    for (int i = -radius; i < radius; i += 2) {
        const float w0 = weights[i + radius + 0];
        const float w1 = weights[i + radius + 1];
        const float w = w0 + w1;
        kernel_data.weights[O(kernel_data.nb_weights)] = w > 0 ? (float)i + w1 / w : (float)i;
        kernel_data.weights[W(kernel_data.nb_weights)] = w;
        kernel_data.nb_weights++;
    }
    kernel_data.weights[O(kernel_data.nb_weights)] = (float)radius;
    kernel_data.weights[W(kernel_data.nb_weights)] = weights[radius + radius];
    kernel_data.nb_weights++;

    if (!s->kernel_staging_cache) {
        s->kernel_staging_cache = ngli_calloc(1, s->kernel_block_size);
        if (!s->kernel_staging_cache)
            return NGL_ERROR_MEMORY;
    }
    memset(s->kernel_staging_cache, 0, s->kernel_block_size);
    const struct ngpu_block_field *fields = s->kernel_block_desc.fields;
    ngpu_block_field_copy_count(&fields[0], s->kernel_staging_cache + fields[0].offset,
                                (const uint8_t *)kernel_data.weights, 0);
    ngpu_block_field_copy(&fields[1], s->kernel_staging_cache + fields[1].offset,
                          (const uint8_t *)&kernel_data.nb_weights);

    return 0;
}

static void push_kernel_block(struct ngl_node *node)
{
    struct gblur_priv *s = node->priv_data;
    struct ngl_ctx *ctx = node->ctx;

    if (!s->kernel_staging_cache)
        return;

    const size_t kernel_offset = ngli_staging_buffer_push(ctx->current_staging_buffer, s->kernel_staging_cache, s->kernel_block_size);
    struct ngpu_buffer *buffer = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);
    ngli_pipeline_compat_update_buffer(s->pl_blur_h, s->kernel_block_index,
                                       buffer, kernel_offset, s->kernel_block_size);
    ngli_pipeline_compat_update_buffer(s->pl_blur_v, s->kernel_block_index,
                                       buffer, kernel_offset, s->kernel_block_size);
}

static int setup_pipeline(struct ngl_ctx *ctx, struct ngpu_pgcraft *crafter, struct pipeline_compat *pipeline, const struct ngpu_rendertarget_layout *layout)
{
    const struct pipeline_compat_params params = {
        .type         = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics     = {
            .topology = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state    = NGPU_GRAPHICS_STATE_DEFAULTS,
            .rt_layout    = *layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(crafter),
        },
        .program          = ngpu_pgcraft_get_program(crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(crafter),
        .resources        = ngpu_pgcraft_get_bindgroup_resources(crafter),
        .vertex_resources = ngpu_pgcraft_get_vertex_resources(crafter),
        .texture_infos    = ngpu_pgcraft_get_texture_infos(crafter),
    };

    int ret = ngli_pipeline_compat_init(pipeline, &params);
    if (ret < 0)
        return ret;

    return 0;
}

static int gblur_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct gblur_priv *s = node->priv_data;
    const struct gblur_opts *o = node->opts;

    struct texture_info *src_info = o->source->priv_data;
    s->image = &src_info->image;
    s->image_rev = SIZE_MAX;

    /* Disable direct rendering */
    src_info->supported_image_layouts = NGLI_IMAGE_LAYOUT_DEFAULT_BIT;

    /* Override texture params */
    src_info->params.min_filter = NGPU_FILTER_LINEAR;
    src_info->params.mag_filter = NGPU_FILTER_LINEAR;
    src_info->params.wrap_s     = NGPU_WRAP_MIRRORED_REPEAT,
    src_info->params.wrap_t     = NGPU_WRAP_MIRRORED_REPEAT,

    s->tmp_layout.colors[s->tmp_layout.nb_colors].format = src_info->params.format;
    s->tmp_layout.nb_colors++;

    struct texture_info *dst_info = o->destination->priv_data;
    dst_info->params.usage |= NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;

    s->dst_is_resizable = (dst_info->params.width == 0 && dst_info->params.height == 0);
    s->dst_layout.colors[0].format = dst_info->params.format;
    s->dst_layout.nb_colors = 1;

    ngpu_block_desc_init(gpu_ctx, &s->direction_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_add_field(&s->direction_block_desc, "direction", NGPU_TYPE_VEC2, 0);
    s->direction_block_size = ngpu_block_desc_get_size(&s->direction_block_desc, 0);
    ngli_assert(s->direction_block_size == sizeof(struct direction_block));
    s->direction_block_aligned_size = ngpu_block_desc_get_aligned_size(&s->direction_block_desc, 0);

    ngpu_block_desc_init(gpu_ctx, &s->kernel_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_add_field(&s->kernel_block_desc, "weights", NGPU_TYPE_VEC2, MAX_KERNEL_SIZE);
    ngpu_block_desc_add_field(&s->kernel_block_desc, "nb_weights", NGPU_TYPE_I32, 0);
    s->kernel_block_size = ngpu_block_desc_get_size(&s->kernel_block_desc, 0);

    int ret;

    const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_texture textures[] = {
        {
            .name        = "tex",
            .type        = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .precision   = NGPU_PRECISION_HIGH,
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
        },
    };

    struct ngpu_buffer *staging_buf = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);

    const struct ngpu_pgcraft_block crafter_blocks[] = {
        {
            .name          = "direction",
            .type          = NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &s->direction_block_desc,
            .buffer        = {
                .buffer = staging_buf,
                .size   = s->direction_block_size,
            },
        }, {
            .name          = "kernel",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &s->kernel_block_desc,
            .buffer        = {
                .buffer = staging_buf,
                .size   = s->kernel_block_size,
            },
        },
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/gaussian-blur",
        .vert_base        = blur_gaussian_vert,
        .frag_base        = blur_gaussian_frag,
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .blocks           = crafter_blocks,
        .nb_blocks        = NGLI_ARRAY_NB(crafter_blocks),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };
    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter)
        return NGL_ERROR_MEMORY;

    ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    if (ret < 0)
        return ret;

    s->direction_block_index = ngpu_pgcraft_get_block_index(s->crafter, "direction", NGPU_PROGRAM_STAGE_FRAG);
    s->kernel_block_index = ngpu_pgcraft_get_block_index(s->crafter, "kernel", NGPU_PROGRAM_STAGE_FRAG);

    s->pl_blur_h = ngli_pipeline_compat_create(gpu_ctx);
    s->pl_blur_v = ngli_pipeline_compat_create(gpu_ctx);
    if (!s->pl_blur_h || !s->pl_blur_v)
        return NGL_ERROR_MEMORY;

    if ((ret = setup_pipeline(ctx, s->crafter, s->pl_blur_h, &s->tmp_layout)) < 0 ||
        (ret = setup_pipeline(ctx, s->crafter, s->pl_blur_v, &s->dst_layout)) < 0)
        return ret;

    return 0;
}

static int resize(struct ngl_node *node)
{
    int ret = 0;
    struct ngl_ctx *ctx = node->ctx;
    struct gblur_priv *s = node->priv_data;
    const struct gblur_opts *o = node->opts;

    ngli_node_pre_draw(o->source);
    ngli_node_draw(o->source);

    struct texture_info *src_info = o->source->priv_data;
    const uint32_t width = src_info->image.params.width;
    const uint32_t height = src_info->image.params.height;
    if (s->width == width && s->height == height)
        return 0;

    /* Assert that the source texture format does not change */
    ngli_assert(src_info->params.format == s->tmp_layout.colors[0].format);

    /* Assert that the destination texture format does not change */
    struct texture_info *dst_info = o->destination->priv_data;
    ngli_assert(dst_info->params.format == s->dst_layout.colors[0].format);

    struct rtt_ctx *tmp = NULL;

    struct ngpu_texture *dst = NULL;
    struct rtt_ctx *dst_rtt_ctx = NULL;

    const struct ngpu_texture_params texture_params = {
        .type          = NGPU_TEXTURE_TYPE_2D,
        .format        = src_info->params.format,
        .width         = width,
        .height        = height,
        .min_filter    = NGPU_FILTER_LINEAR,
        .mag_filter    = NGPU_FILTER_LINEAR,
        .wrap_s        = NGPU_WRAP_MIRRORED_REPEAT,
        .wrap_t        = NGPU_WRAP_MIRRORED_REPEAT,
        .usage         = NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
                         NGPU_TEXTURE_USAGE_SAMPLED_BIT,
    };

    tmp = ngli_rtt_create(ctx);
    if (!tmp) {
        ret = NGL_ERROR_MEMORY;
        goto fail;
    }

    ret = ngli_rtt_from_texture_params(tmp, &texture_params);
    if (ret < 0)
        goto fail;

    dst = dst_info->texture;
    if (s->dst_is_resizable) {
        dst = ngpu_texture_create(ctx->gpu_ctx);
        if (!dst) {
            ret = NGL_ERROR_MEMORY;
            goto fail;
        }

        struct ngpu_texture_params params = dst_info->params;
        params.width = width;
        params.height = height;
        ret = ngpu_texture_init(dst, &params);
        if (ret < 0)
            goto fail;
    }

    ngli_rtt_freep(&s->tmp);
    s->tmp = tmp;

    if (s->dst_is_resizable) {
        ngpu_texture_freep(&dst_info->texture);
        dst_info->texture = dst;
        dst_info->image.params.width = ngpu_texture_get_params(dst)->width;
        dst_info->image.params.height = ngpu_texture_get_params(dst)->height;
        dst_info->image.planes[0] = dst;
        dst_info->image.rev = dst_info->image_rev++;
    }

    dst_rtt_ctx = ngli_rtt_create(ctx);
    if (!dst_rtt_ctx) {
        ret = NGL_ERROR_MEMORY;
        goto fail;
    }

    const struct rtt_params rtt_params = (struct rtt_params) {
        .width  = ngpu_texture_get_params(dst)->width,
        .height = ngpu_texture_get_params(dst)->height,
        .nb_colors = 1,
        .colors[0] = {
            .attachment = dst,
            .load_op = NGPU_LOAD_OP_CLEAR,
            .store_op = NGPU_STORE_OP_STORE,
        },
    };

    ret = ngli_rtt_init(dst_rtt_ctx, &rtt_params);
    if (ret < 0)
        goto fail;

    ngli_rtt_freep(&s->dst_rtt_ctx);
    s->dst_rtt_ctx = dst_rtt_ctx;

    s->width = width;
    s->height = height;

    /* Trigger a kernel update on resolution change */
    s->blurriness = -1.f;

    return 0;

fail:
    ngli_rtt_freep(&tmp);

    ngli_rtt_freep(&dst_rtt_ctx);
    if (s->dst_is_resizable)
        ngpu_texture_freep(&dst);

    LOG(ERROR, "failed to resize blur: %ux%u", width, height);
    return ret;
}

static void gblur_pre_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct gblur_priv *s = node->priv_data;

    int ret = resize(node);
    if (ret < 0)
        return;

    ret = update_kernel(node);
    if (ret < 0)
        return;

    push_kernel_block(node);

    const struct direction_block dir_h = {.direction = {1.f, 0.f}};
    const struct direction_block dir_v = {.direction = {0.f, 1.f}};
    const size_t dir_h_offset = ngli_staging_buffer_push(ctx->current_staging_buffer, &dir_h, sizeof(dir_h));
    const size_t dir_v_offset = ngli_staging_buffer_push(ctx->current_staging_buffer, &dir_v, sizeof(dir_v));
    struct ngpu_buffer *buffer = ngli_staging_buffer_get_buffer(ctx->current_staging_buffer);
    ngli_pipeline_compat_update_buffer(s->pl_blur_h, s->direction_block_index,
                                       buffer, dir_h_offset, s->direction_block_size);
    ngli_pipeline_compat_update_buffer(s->pl_blur_v, s->direction_block_index,
                                       buffer, dir_h_offset, s->direction_block_size);

    ngli_rtt_begin(s->tmp);
    ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);
    uint32_t offset = 0;
    ngli_pipeline_compat_update_dynamic_offsets(s->pl_blur_h, &offset, 1);
    ngli_pipeline_compat_update_image(s->pl_blur_h, 0, s->image, ctx->current_staging_buffer);
    ngli_pipeline_compat_draw(s->pl_blur_h, 3, 1, 0);
    ngli_rtt_end(s->tmp);

    ngli_rtt_begin(s->dst_rtt_ctx);
    ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);
    offset = (uint32_t)(dir_v_offset - dir_h_offset);
    ngli_pipeline_compat_update_dynamic_offsets(s->pl_blur_v, &offset, 1);
    ngli_pipeline_compat_update_image(s->pl_blur_v, 0, ngli_rtt_get_image(s->tmp, 0), ctx->current_staging_buffer);
    ngli_pipeline_compat_draw(s->pl_blur_v, 3, 1, 0);
    ngli_rtt_end(s->dst_rtt_ctx);
}

static void gblur_release(struct ngl_node *node)
{
    struct gblur_priv *s = node->priv_data;

    ngli_rtt_freep(&s->tmp);
    ngli_rtt_freep(&s->dst_rtt_ctx);
}

static void gblur_uninit(struct ngl_node *node)
{
    struct gblur_priv *s = node->priv_data;

    ngli_freep(&s->kernel_staging_cache);
    ngpu_block_desc_reset(&s->direction_block_desc);
    ngpu_block_desc_reset(&s->kernel_block_desc);
    ngli_pipeline_compat_freep(&s->pl_blur_h);
    ngli_pipeline_compat_freep(&s->pl_blur_v);
    ngpu_pgcraft_freep(&s->crafter);
}

const struct node_class ngli_gblur_class = {
    .id        = NGL_NODE_GAUSSIANBLUR,
    .name      = "GaussianBlur",
    .init      = gblur_init,
    .update    = ngli_node_update_children,
    .pre_draw  = gblur_pre_draw,
    .release   = gblur_release,
    .uninit    = gblur_uninit,
    .opts_size = sizeof(struct gblur_opts),
    .priv_size = sizeof(struct gblur_priv),
    .params    = gblur_params,
    .file      = __FILE__,
};
