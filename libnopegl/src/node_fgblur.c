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
#include <string.h>

#include "fgblur.h"
#include "internal.h"
#include "log.h"
#include <ngpu/ngpu.h>
#include "node_texture.h"
#include "node_uniform.h"
#include "nopegl/nopegl.h"
#include "rtt.h"
#include "utils/utils.h"

struct fgblur_opts {
    struct ngl_node *source;
    struct ngl_node *destination;
    struct ngl_node *blurriness_node;
    float blurriness;
};

struct fgblur_priv {
    struct fgblur_ctx blur;

    int dst_is_resizable;
    struct ngpu_rendertarget_layout dst_layout;
};

#define OFFSET(x) offsetof(struct fgblur_opts, x)
static const struct node_param fgblur_params[] = {
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
                          .desc=NGLI_DOCSTRING("amount of blurriness in the range [0,1]")},
    {NULL}
};

static int fgblur_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct fgblur_priv *s = node->priv_data;
    const struct fgblur_opts *o = node->opts;

    /* Disable direct rendering */
    struct texture_info *src_info = o->source->priv_data;
    src_info->supported_image_layouts = NGLI_IMAGE_LAYOUT_DEFAULT_BIT;

    /* Override texture params */
    src_info->params.min_filter = NGPU_FILTER_LINEAR;
    src_info->params.mag_filter = NGPU_FILTER_LINEAR;
    src_info->params.wrap_s     = NGPU_WRAP_MIRRORED_REPEAT;
    src_info->params.wrap_t     = NGPU_WRAP_MIRRORED_REPEAT;

    struct texture_info *dst_info = o->destination->priv_data;
    dst_info->params.usage |= NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;

    s->dst_is_resizable = (dst_info->params.width == 0 && dst_info->params.height == 0);

    struct ngpu_rendertarget_layout mip_layout = {0};
    mip_layout.nb_colors = 1;
    mip_layout.colors[0].format = src_info->params.format;

    s->dst_layout.nb_colors = 1;
    s->dst_layout.colors[0].format = dst_info->params.format;

    return ngli_fgblur_init(&s->blur, gpu_ctx, &mip_layout, &s->dst_layout);
}

static int resize(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct fgblur_priv *s = node->priv_data;
    const struct fgblur_opts *o = node->opts;

    ngli_node_draw(o->source);

    struct texture_info *src_info = o->source->priv_data;
    const uint32_t width = src_info->image.params.width;
    const uint32_t height = src_info->image.params.height;
    if (s->blur.width == width && s->blur.height == height)
        return 0;

    int ret = ngli_fgblur_resize(&s->blur, ctx, width, height, src_info->params.format);
    if (ret < 0)
        return ret;

    /* Handle resizable destination texture */
    struct texture_info *dst_info = o->destination->priv_data;
    if (s->dst_is_resizable) {
        struct ngpu_texture *dst = ngpu_texture_create(ctx->gpu_ctx);
        if (!dst)
            return NGL_ERROR_MEMORY;

        struct ngpu_texture_params params = dst_info->params;
        params.width = width;
        params.height = height;
        ret = ngpu_texture_init(dst, &params);
        if (ret < 0) {
            ngpu_texture_freep(&dst);
            return ret;
        }

        ngpu_texture_freep(&dst_info->texture);
        dst_info->texture = dst;
        dst_info->image.params.width = width;
        dst_info->image.params.height = height;
        dst_info->image.planes[0] = dst;
        dst_info->image.rev = dst_info->image_rev++;
    }

    return 0;
}

static void fgblur_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct fgblur_priv *s = node->priv_data;
    const struct fgblur_opts *o = node->opts;

    int ret = resize(node);
    if (ret < 0)
        return;

    const float blurriness_raw = *(float *)ngli_node_get_data_ptr(o->blurriness_node, &o->blurriness);
    const float blurriness = NGLI_CLAMP(blurriness_raw, 0.f, 1.f);

    struct texture_info *src_info = o->source->priv_data;
    const struct image *src_image = &src_info->image;

    const struct image *result = ngli_fgblur_execute(&s->blur, ctx, src_image,
                                                      blurriness,
                                                      s->blur.width, s->blur.height);

    /*
     * The downsample, upsample and interpolate passes do not deal with the
     * texture coordinates at all, thus we need to forward the source
     * coordinates matrix to the destination.
     */
    struct texture_info *dst_info = o->destination->priv_data;
    struct image *dst_image = &dst_info->image;
    memcpy(dst_image->coordinates_matrix, src_image->coordinates_matrix, sizeof(src_image->coordinates_matrix));
    (void)result;
}

static void fgblur_release(struct ngl_node *node)
{
    struct fgblur_priv *s = node->priv_data;
    ngli_fgblur_release(&s->blur);
}

static void fgblur_uninit(struct ngl_node *node)
{
    struct fgblur_priv *s = node->priv_data;
    ngli_fgblur_uninit(&s->blur);
}

const struct node_class ngli_fgblur_class = {
    .id        = NGL_NODE_FASTGAUSSIANBLUR,
    .name      = "FastGaussianBlur",
    .init      = fgblur_init,
    .prepare   = ngli_node_prepare_children,
    .update    = ngli_node_update_children,
    .draw      = fgblur_draw,
    .release   = fgblur_release,
    .uninit    = fgblur_uninit,
    .opts_size = sizeof(struct fgblur_opts),
    .priv_size = sizeof(struct fgblur_priv),
    .params    = fgblur_params,
    .file      = __FILE__,
};
