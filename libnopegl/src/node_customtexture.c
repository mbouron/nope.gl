/*
 * Copyright 2024 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <ngpu/ngpu.h>

#include "config.h"
#include "internal.h"
#include "log.h"
#include "node_texture.h"
#include "params.h"

#if defined(TARGET_ANDROID)
#include "jni_utils.h"
#endif

#if defined(BACKEND_GL) || defined(BACKEND_GLES)
#include "nopegl/nopegl_opengl.h"
#endif

struct customtexture_opts {
    void *user_data;
    struct ngl_node_funcs funcs;
};

struct customtexture_priv {
    struct texture_info texture_info;
};

#define OFFSET(x) offsetof(struct customtexture_opts, x)
static const struct node_param customtexture_params[] = {
    {NULL}
};

static int customtexture_init(struct ngl_node *node)
{
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

    if (!funcs->init)
        return 0;

    int ret = funcs->init(NULL, o->user_data);
    if (ret < 0)
        return ret;

    return 0;
}

static int customtexture_prefetch(struct ngl_node *node)
{
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

    if (!funcs->prefetch)
        return 0;

    int ret = funcs->prefetch(NULL, o->user_data);
    if (ret < 0)
        return ret;

    return 0;
}

static int customtexture_prepare(struct ngl_node *node)
{
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

    if (!funcs->prepare)
        return 0;

    int ret = funcs->prepare(NULL, o->user_data);
    if (ret < 0)
        return ret;

    return 0;
}

static int customtexture_update(struct ngl_node *node, double t)
{
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

    if (!funcs->update)
        return 0;

    int ret = funcs->update(NULL, o->user_data, t);
    if (ret < 0)
        return ret;

    return 0;
}

static void customtexture_draw(struct ngl_node *node)
{
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

    if (!funcs->draw)
        return;

    funcs->draw(NULL, o->user_data);
}

static void customtexture_release(struct ngl_node *node)
{
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

    if (!funcs->release)
        return;

    funcs->release(NULL, o->user_data);
}

static void customtexture_uninit(struct ngl_node *node)
{
    struct customtexture_priv *s = node->priv_data;
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

    ngpu_texture_freep(&s->texture_info.texture);
    ngli_image_reset(&s->texture_info.image);

    if (!funcs->uninit)
        return;

    funcs->uninit(NULL, o->user_data);
}

static void customtexture_free(struct ngl_node *node)
{
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

    if (!funcs->free)
        return;

    funcs->free(NULL, o->user_data);
}

int ngl_node_set_funcs(struct ngl_node *node, void *user_data, struct ngl_node_funcs *funcs)
{
    if (!node)
        return NGL_ERROR_INVALID_ARG;

    if (node->cls->id != NGL_NODE_CUSTOMTEXTURE)
        return NGL_ERROR_UNSUPPORTED;

    if (node->ctx) {
        LOG(ERROR, "%s can not be live changed", node->label);
        return NGL_ERROR_INVALID_ARG;
    }

    struct customtexture_opts *o = node->opts;

    o->user_data = user_data;
    o->funcs = *funcs;

    return 0;
}

#if defined(BACKEND_GL) || defined(BACKEND_GLES)
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_EXTERNAL_OES 0x8D65 

static enum image_layout target_to_layout(uint32_t target)
{
    switch (target) {
        case GL_TEXTURE_2D:
            return NGLI_IMAGE_LAYOUT_DEFAULT;
        case GL_TEXTURE_EXTERNAL_OES:
            return NGLI_IMAGE_LAYOUT_MEDIACODEC;
        default:
            ngli_assert(0);
    }
}

static int import_texture_gl(struct ngl_node *node, const struct ngl_custom_texture_info_gl *info)
{
    struct customtexture_priv *s = node->priv_data;
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    const enum ngpu_backend_type backend = ngpu_ctx_get_backend_type(gpu_ctx);
    if (backend != NGPU_BACKEND_OPENGL && backend != NGPU_BACKEND_OPENGLES)
        return NGL_ERROR_GRAPHICS_UNSUPPORTED;

    if (info->target != GL_TEXTURE_2D &&
        info->target != GL_TEXTURE_EXTERNAL_OES) {
        LOG(ERROR, "only 2D and external OES textures are supported");
        return NGL_ERROR_GRAPHICS_UNSUPPORTED;
    }

    struct ngpu_texture_params texture_params = {
        .type       = NGPU_TEXTURE_TYPE_2D,
        .format     = NGPU_FORMAT_R8G8B8A8_UNORM,
        .width      = info->width,
        .height     = info->height,
        .min_filter = NGPU_FILTER_LINEAR,
        .mag_filter = NGPU_FILTER_LINEAR,
        .usage      = NGPU_TEXTURE_USAGE_SAMPLED_BIT,
        .import_params = {
            .type = NGPU_IMPORT_TYPE_OPENGL_TEXTURE,
            .opengl_texture = {
                .texture = info->texture,
                .target = info->target,
            },
        },
    };

    s->texture_info.texture = ngpu_texture_create(gpu_ctx);
    if (!s->texture_info.texture)
        return NGL_ERROR_MEMORY;

    int ret = ngpu_texture_init(s->texture_info.texture, &texture_params);
    if (ret < 0)
        return ret;
    
    const struct image_params image_params = {
        .width       = info->width,
        .height      = info->height,
        .layout      = target_to_layout(info->target),
        .color_scale = 1.f,
        .color_info  = NGLI_COLOR_INFO_DEFAULTS,
    };
    ngli_image_init(&s->texture_info.image, &image_params, &s->texture_info.texture);

    s->texture_info.image.rev = s->texture_info.image_rev++;

    return 0;
}
#else
static int import_texture_gl(struct ngl_node *node, struct ngl_custom_texture_info *info)
{
    return NGL_ERROR_UNSUPPORTED;
}
#endif

int ngl_custom_texture_set_texture_info_gl(struct ngl_node *node, const struct ngl_custom_texture_info_gl *info)
{
    if (!node)
        return NGL_ERROR_INVALID_ARG;

    if (node->cls->id != NGL_NODE_CUSTOMTEXTURE)
        return NGL_ERROR_UNSUPPORTED;

    if (!node->ctx)
        return NGL_ERROR_UNSUPPORTED;

    struct customtexture_priv *s = node->priv_data;

    /* Cleanup previous texture/image */
    ngpu_texture_freep(&s->texture_info.texture);
    ngli_image_reset(&s->texture_info.image);
    s->texture_info.image.rev = s->texture_info.image_rev++;
    if (!info) {
        return ngli_node_invalidate_branch(node);
    }

    struct ngl_ctx *ctx = node->ctx;
    struct ngl_config *config = &ctx->config;

    int ret;
    if (config->backend == NGL_BACKEND_OPENGL ||
        config->backend == NGL_BACKEND_OPENGLES) {
        ret = import_texture_gl(node, info);
    } else {
        ret = NGL_ERROR_UNSUPPORTED;
    }
    if (ret < 0)
        return ret;

    return ngli_node_invalidate_branch(node);
}

const struct node_class ngli_customtexture_class = {
    .id             = NGL_NODE_CUSTOMTEXTURE,
    .category       = NGLI_NODE_CATEGORY_TEXTURE,
    .name           = "CustomTexture",
    .init           = customtexture_init,
    .prepare        = customtexture_prepare,
    .prefetch       = customtexture_prefetch,
    .update         = customtexture_update,
    .draw           = customtexture_draw,
    .release        = customtexture_release,
    .uninit         = customtexture_uninit,
    .free           = customtexture_free,
    .priv_size      = sizeof(struct customtexture_priv),
    .opts_size      = sizeof(struct customtexture_opts),
    .params         = customtexture_params,
    .file           = __FILE__,
};
