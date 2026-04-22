/*
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

#include <stddef.h>
#include <string.h>

#include "internal.h"
#include "node2d.h"
#include "log.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "node_texture.h"
#include "node_textureview.h"
#include "nopegl/nopegl.h"
#include "rtt.h"
#include "utils/utils.h"

struct offscreencanvas2d_opts {
    struct ngl_node **children;
    size_t nb_children;
    int32_t width;
    int32_t height;
    struct ngl_node **color_textures;
    size_t nb_color_textures;
    struct ngl_node *depth_texture;
    uint32_t samples;
    float clear_color[4];
};

struct offscreencanvas2d_priv {
    struct ngli_node2d_info node2d_info;
    struct darray indices;

    uint32_t rtt_width;
    uint32_t rtt_height;
    int resizable;
    struct ngpu_rendertarget_layout layout;
    struct rtt_params rtt_params;
    struct rtt_ctx *rtt_ctx;
};

static int offscreencanvas2d_swap_children(struct ngl_node *node, size_t from, size_t to)
{
    struct offscreencanvas2d_priv *s = node->priv_data;
    size_t *indices = ngli_darray_data(&s->indices);
    NGLI_SWAP(size_t, indices[from], indices[to]);
    return 0;
}

#define OFFSET(x) offsetof(struct offscreencanvas2d_opts, x)
static const struct node_param offscreencanvas2d_params[] = {
    {
        .key        = "children",
        .type       = NGLI_PARAM_TYPE_NODELIST,
        .offset     = OFFSET(children),
        .flags      = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .node_types = NGLI_NODE2D_TYPES_LIST,
        .desc       = NGLI_DOCSTRING("2D scenes to render offscreen"),
        .swap_func  = offscreencanvas2d_swap_children,
    }, {
        .key        = "width",
        .type       = NGLI_PARAM_TYPE_I32,
        .offset     = OFFSET(width),
        .desc       = NGLI_DOCSTRING("canvas width in pixels (0 uses parent's canvas width)"),
    }, {
        .key        = "height",
        .type       = NGLI_PARAM_TYPE_I32,
        .offset     = OFFSET(height),
        .desc       = NGLI_DOCSTRING("canvas height in pixels (0 uses parent's canvas height)"),
    }, {
        .key        = "color_textures",
        .type       = NGLI_PARAM_TYPE_NODELIST,
        .offset     = OFFSET(color_textures),
        .node_types = (const uint32_t[]){
            NGL_NODE_TEXTURE2D,
            NGL_NODE_TEXTURE2DARRAY,
            NGL_NODE_TEXTURE3D,
            NGL_NODE_TEXTURECUBE,
            NGL_NODE_TEXTUREVIEW,
            NGLI_NODE_NONE
        },
        .desc       = NGLI_DOCSTRING("destination color texture"),
    }, {
        .key        = "depth_texture",
        .type       = NGLI_PARAM_TYPE_NODE,
        .offset     = OFFSET(depth_texture),
        .flags      = NGLI_PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
        .node_types = (const uint32_t[]){NGL_NODE_TEXTURE2D, NGL_NODE_TEXTUREVIEW, NGLI_NODE_NONE},
        .desc       = NGLI_DOCSTRING("destination depth (and potentially combined stencil) texture"),
    }, {
        .key        = "samples",
        .type       = NGLI_PARAM_TYPE_I32,
        .offset     = OFFSET(samples),
        .desc       = NGLI_DOCSTRING("number of samples used for multisampling anti-aliasing"),
    }, {
        .key        = "clear_color",
        .type       = NGLI_PARAM_TYPE_VEC4,
        .offset     = OFFSET(clear_color),
        .desc       = NGLI_DOCSTRING("color used to clear the color texture"),
    },
    {NULL}
};
#undef OFFSET

struct rtt_texture_info {
    struct ngl_node *node;
    struct texture_info *info;
    uint32_t layer_base;
    uint32_t layer_count;
};

static struct rtt_texture_info get_rtt_texture_info(struct ngl_node *node)
{
    if (node->cls->id == NGL_NODE_TEXTUREVIEW) {
        const struct textureview_opts *v = node->opts;
        struct texture_info *info = v->texture->priv_data;
        return (struct rtt_texture_info){node, info, v->layer, 1};
    }
    struct texture_info *info = node->priv_data;
    const uint32_t nb_layers = info->params.type == NGPU_TEXTURE_TYPE_CUBE ? 6 : 1;
    return (struct rtt_texture_info){node, info, 0, nb_layers};
}

static int offscreencanvas2d_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    const struct ngpu_limits *limits = ngpu_ctx_get_limits(gpu_ctx);
    struct offscreencanvas2d_priv *s = node->priv_data;
    const struct offscreencanvas2d_opts *o = node->opts;

    ngli_darray_init(&s->indices, sizeof(size_t), 0);

    if (!o->nb_color_textures) {
        LOG(ERROR, "at least one color texture must be specified");
        return NGL_ERROR_INVALID_ARG;
    }

    s->layout.samples = o->samples;

    size_t nb_color_attachments = 0;
    for (size_t i = 0; i < o->nb_color_textures; i++) {
        const struct rtt_texture_info texture_info = get_rtt_texture_info(o->color_textures[i]);
        nb_color_attachments += texture_info.layer_count;

        if (ngli_node_texture_has_media_data_src(texture_info.node)) {
            LOG(ERROR, "render targets cannot have a data source");
            return NGL_ERROR_INVALID_ARG;
        }

        struct ngpu_texture_params *params = &texture_info.info->params;
        if (i == 0) {
            s->rtt_width = params->width;
            s->rtt_height = params->height;
            s->resizable = (s->rtt_width == 0 && s->rtt_height == 0);
        } else if (s->rtt_width != params->width || s->rtt_height != params->height) {
            LOG(ERROR, "all color texture dimensions do not match: %ux%u != %ux%u",
                s->rtt_width, s->rtt_height, params->width, params->height);
            return NGL_ERROR_INVALID_ARG;
        }

        params->usage |= NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
        for (uint32_t j = 0; j < texture_info.layer_count; j++) {
            s->layout.colors[s->layout.nb_colors].format = params->format;
            s->layout.colors[s->layout.nb_colors].resolve = o->samples > 1;
            s->layout.nb_colors++;
        }
    }

    if (nb_color_attachments > limits->max_color_attachments) {
        LOG(ERROR, "context does not support more than %u color attachments", limits->max_color_attachments);
        return NGL_ERROR_UNSUPPORTED;
    }

    if (o->depth_texture) {
        const struct rtt_texture_info texture_info = get_rtt_texture_info(o->depth_texture);

        if (ngli_node_texture_has_media_data_src(texture_info.node)) {
            LOG(ERROR, "render targets cannot have a data source");
            return NGL_ERROR_INVALID_ARG;
        }

        struct ngpu_texture_params *params = &texture_info.info->params;
        if (s->rtt_width != params->width || s->rtt_height != params->height) {
            LOG(ERROR, "color and depth texture dimensions do not match: %ux%u != %ux%u",
                s->rtt_width, s->rtt_height, params->width, params->height);
            return NGL_ERROR_INVALID_ARG;
        }

        const uint64_t features = ngpu_ctx_get_features(gpu_ctx);
        if (!(features & NGPU_FEATURE_DEPTH_STENCIL_RESOLVE_BIT) && o->samples > 0) {
            LOG(ERROR, "context does not support resolving depth/stencil attachments");
            return NGL_ERROR_GRAPHICS_UNSUPPORTED;
        }

        params->usage |= NGPU_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        s->layout.depth_stencil.format = params->format;
        s->layout.depth_stencil.resolve = o->samples > 1;
    }

    return 0;
}

static void offscreencanvas2d_get_child_render_state(const struct ngl_node *node,
                                                      const struct ngpu_graphics_state *parent_gs,
                                                      const struct ngpu_rendertarget_layout *parent_rtl,
                                                      struct ngpu_graphics_state *child_gs,
                                                      struct ngpu_rendertarget_layout *child_rtl)
{
    const struct offscreencanvas2d_priv *s = node->priv_data;
    *child_gs = *parent_gs;
    *child_rtl = s->layout;
}

static int offscreencanvas2d_prepare(struct ngl_node *node,
                                     const struct ngpu_graphics_state *graphics_state,
                                     const struct ngpu_rendertarget_layout *rendertarget_layout)
{
    struct offscreencanvas2d_priv *s = node->priv_data;
    const struct offscreencanvas2d_opts *o = node->opts;

    for (size_t i = 0; i < o->nb_children; i++) {
        if (!ngli_darray_push(&s->indices, &i))
            return NGL_ERROR_MEMORY;
    }

    return 0;
}

static int offscreencanvas2d_prefetch(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct offscreencanvas2d_priv *s = node->priv_data;
    const struct offscreencanvas2d_opts *o = node->opts;

    s->rtt_params = (struct rtt_params) {
        .width = s->rtt_width,
        .height = s->rtt_height,
        .samples = o->samples,
    };

    for (size_t i = 0; i < o->nb_color_textures; i++) {
        const struct rtt_texture_info rti = get_rtt_texture_info(o->color_textures[i]);
        struct ngpu_texture *texture = rti.info->texture;
        const uint32_t layer_end = rti.layer_base + rti.layer_count;
        for (uint32_t j = rti.layer_base; j < layer_end; j++) {
            s->rtt_params.colors[s->rtt_params.nb_colors++] = (struct ngpu_attachment) {
                .attachment       = texture,
                .attachment_layer = j,
                .load_op          = NGPU_LOAD_OP_CLEAR,
                .clear_value      = {NGLI_ARG_VEC4(o->clear_color)},
                .store_op         = NGPU_STORE_OP_STORE,
            };
        }
        struct image *image = &rti.info->image;
        ngpu_ctx_get_rendertarget_uvcoord_matrix(gpu_ctx, image->coordinates_matrix.m);
    }

    if (o->depth_texture) {
        const struct rtt_texture_info rti = get_rtt_texture_info(o->depth_texture);
        s->rtt_params.depth_stencil = (struct ngpu_attachment) {
            .attachment       = rti.info->texture,
            .attachment_layer = rti.layer_base,
            .load_op          = NGPU_LOAD_OP_CLEAR,
            .store_op         = NGPU_STORE_OP_STORE,
        };
    }

    if (s->resizable)
        return 0;

    s->rtt_ctx = ngli_rtt_create(ctx);
    if (!s->rtt_ctx)
        return NGL_ERROR_MEMORY;

    return ngli_rtt_init(s->rtt_ctx, &s->rtt_params);
}

static int offscreencanvas2d_resize(struct ngl_node *node)
{
    int ret = 0;
    struct ngl_ctx *ctx = node->ctx;
    struct offscreencanvas2d_priv *s = node->priv_data;
    const struct offscreencanvas2d_opts *o = node->opts;

    const uint32_t width = ngpu_rendertarget_get_width(ctx->current_rendertarget);
    const uint32_t height = ngpu_rendertarget_get_height(ctx->current_rendertarget);
    if (s->rtt_ctx) {
        uint32_t current_width, current_height;
        ngli_rtt_get_dimensions(s->rtt_ctx, &current_width, &current_height);
        if (current_width == width && current_height == height)
            return 0;
    }

    struct ngpu_texture *textures[NGPU_MAX_COLOR_ATTACHMENTS] = {NULL};
    struct ngpu_texture *depth_texture = NULL;
    struct rtt_ctx *rtt_ctx = NULL;

    for (size_t i = 0; i < o->nb_color_textures; i++) {
        textures[i] = ngpu_texture_create(ctx->gpu_ctx);
        if (!textures[i]) {
            ret = NGL_ERROR_MEMORY;
            goto fail;
        }

        const struct rtt_texture_info info = get_rtt_texture_info(o->color_textures[i]);
        struct ngpu_texture_params texture_params = info.info->params;
        texture_params.width = width;
        texture_params.height = height;

        ret = ngpu_texture_init(textures[i], &texture_params);
        if (ret < 0)
            goto fail;
    }

    if (o->depth_texture) {
        depth_texture = ngpu_texture_create(ctx->gpu_ctx);
        if (!depth_texture) {
            ret = NGL_ERROR_MEMORY;
            goto fail;
        }

        const struct rtt_texture_info info = get_rtt_texture_info(o->depth_texture);
        struct ngpu_texture_params texture_params = info.info->params;
        texture_params.width = width;
        texture_params.height = height;

        ret = ngpu_texture_init(depth_texture, &texture_params);
        if (ret < 0)
            goto fail;
    }

    rtt_ctx = ngli_rtt_create(ctx);
    if (!rtt_ctx) {
        ret = NGL_ERROR_MEMORY;
        goto fail;
    }

    struct rtt_params params = s->rtt_params;
    params.width  = width;
    params.height = height;

    for (size_t i = 0; i < o->nb_color_textures; i++)
        params.colors[i].attachment = textures[i];
    params.depth_stencil.attachment = depth_texture;

    ret = ngli_rtt_init(rtt_ctx, &params);
    if (ret < 0)
        goto fail;

    ngli_rtt_freep(&s->rtt_ctx);

    s->rtt_width = width;
    s->rtt_height = height;
    s->rtt_params = params;
    s->rtt_ctx = rtt_ctx;

    for (size_t i = 0; i < o->nb_color_textures; i++) {
        const struct rtt_texture_info info = get_rtt_texture_info(o->color_textures[i]);
        struct texture_info *texture_info = info.info;
        ngpu_texture_freep(&texture_info->texture);
        texture_info->texture = textures[i];
        texture_info->image.params.width = width;
        texture_info->image.params.height = height;
        texture_info->image.planes[0] = textures[i];
        texture_info->image.rev = texture_info->image_rev++;
    }

    if (o->depth_texture) {
        const struct rtt_texture_info info = get_rtt_texture_info(o->depth_texture);
        struct texture_info *texture_info = info.info;
        ngpu_texture_freep(&texture_info->texture);
        texture_info->texture = depth_texture;
        texture_info->image.params.width = width;
        texture_info->image.params.height = height;
        texture_info->image.planes[0] = depth_texture;
        texture_info->image.rev = texture_info->image_rev++;
    }

    return 0;

fail:
    for (size_t i = 0; i < o->nb_color_textures; i++)
        ngpu_texture_freep(&textures[i]);
    ngpu_texture_freep(&depth_texture);
    ngli_rtt_freep(&rtt_ctx);

    LOG(ERROR, "failed to resize offscreen canvas: %ux%u", width, height);
    return ret;
}

static void offscreencanvas2d_pre_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct offscreencanvas2d_priv *s = node->priv_data;
    const struct offscreencanvas2d_opts *o = node->opts;

    /* Handle resizable textures */
    if (s->resizable) {
        int ret = offscreencanvas2d_resize(node);
        if (ret < 0)
            return;
    }

    /* Pre-draw children */
    for (size_t i = 0; i < o->nb_children; i++)
        ngli_node_pre_draw(o->children[i]);

    /* Save previous 2D state */
    struct ngli_mat4 prev_projection_2d = ctx->projection_2d_matrix;
    struct darray prev_transform_2d_stack = ctx->transform_2d_stack;
    struct darray prev_opacity_2d_stack = ctx->opacity_2d_stack;

    /* Initialize fresh stacks */
    ngli_darray_init(&ctx->transform_2d_stack, sizeof(struct ngli_mat4), NGLI_DARRAY_FLAG_ALIGNED);
    ngli_darray_init(&ctx->opacity_2d_stack, sizeof(float), 0);

    static const struct ngli_mat4 id_matrix = {.m = NGLI_MAT4_IDENTITY};
    const float default_opacity = 1.f;
    if (!ngli_darray_push(&ctx->transform_2d_stack, &id_matrix) ||
        !ngli_darray_push(&ctx->opacity_2d_stack, &default_opacity))
        goto restore;

    /* Begin RTT + set up orthographic projection */
    ngli_rtt_begin(s->rtt_ctx);

    const float w = o->width  > 0 ? (float)o->width  : ctx->canvas_2d_width;
    const float h = o->height > 0 ? (float)o->height : ctx->canvas_2d_height;

    struct ngli_mat4 base_projection;
    ngpu_ctx_get_projection_matrix(gpu_ctx, base_projection.m);
    ngli_mat4_orthographic(ctx->projection_2d_matrix.m, -0.5f, w - 0.5f, h - 0.5f, -0.5f, -1.f, 1.f);
    ngli_mat4_mul(ctx->projection_2d_matrix.m, base_projection.m, ctx->projection_2d_matrix.m);

    /* Draw children */
    const size_t *indices = ngli_darray_data(&s->indices);
    for (size_t i = 0; i < o->nb_children; i++) {
        const size_t index = indices[i];
        ngli_node_draw(o->children[index]);
    }

    ngli_rtt_end(s->rtt_ctx);

restore:
    /* Restore previous 2D state */
    ngli_darray_reset(&ctx->transform_2d_stack);
    ngli_darray_reset(&ctx->opacity_2d_stack);
    ctx->transform_2d_stack = prev_transform_2d_stack;
    ctx->opacity_2d_stack = prev_opacity_2d_stack;
    ctx->projection_2d_matrix = prev_projection_2d;
}

static void offscreencanvas2d_draw(struct ngl_node *node)
{
}

static void offscreencanvas2d_release(struct ngl_node *node)
{
    struct offscreencanvas2d_priv *s = node->priv_data;
    ngli_rtt_freep(&s->rtt_ctx);
}

static void offscreencanvas2d_uninit(struct ngl_node *node)
{
    struct offscreencanvas2d_priv *s = node->priv_data;
    ngli_darray_reset(&s->indices);
}

const struct node_class ngli_offscreencanvas2d_class = {
    .id        = NGL_NODE_OFFSCREENCANVAS2D,
    .name      = "OffscreenCanvas2D",
    .priv_size = sizeof(struct offscreencanvas2d_priv),
    .init      = offscreencanvas2d_init,
    .get_child_render_state = offscreencanvas2d_get_child_render_state,
    .prepare   = offscreencanvas2d_prepare,
    .prefetch  = offscreencanvas2d_prefetch,
    .update    = ngli_node_update_children,
    .pre_draw  = offscreencanvas2d_pre_draw,
    .draw      = offscreencanvas2d_draw,
    .release   = offscreencanvas2d_release,
    .uninit    = offscreencanvas2d_uninit,
    .opts_size = sizeof(struct offscreencanvas2d_opts),
    .params    = offscreencanvas2d_params,
    .flags     = NGLI_NODE_FLAG_2D,
    .file      = __FILE__,
};
