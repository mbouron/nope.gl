/*
 * Copyright 2024-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <nopegpu/nopegpu.h>

#include "config.h"
#include "internal.h"
#include "log.h"
#include "node_texture.h"
#include "params.h"

#if defined(TARGET_ANDROID)
#include <unistd.h>

#include "jni_utils.h"
#include "nopegl/nopegl_android.h"
#include "utils/darray.h"

#if defined(BACKEND_GL) || defined(BACKEND_GLES)
#include <nopegpu/nopegpu_opengl.h>
#endif

#if defined(BACKEND_VK)
#include <nopegpu/nopegpu_vulkan.h>
#endif

/* Mobile GPUs have little memory: a canvas cycles 2 buffers, up to 4 when drawn several times per frame */
#define MAX_AHB_TEXTURES 4

/*
 * Reuse textures when the producer returns the same buffers. Hold a buffer
 * reference so a different buffer cannot reuse its address and match the
 * cache entry. Discard unused entries as the producer replaces its buffers.
 */
struct ahb_texture {
    AHardwareBuffer *buffer;
    uint32_t width;
    uint32_t height;
    struct ngpu_texture *texture;
    uint64_t map_index;
};
#endif

#include "nopegl/nopegl_opengl.h"

struct customtexture_opts {
    void *user_data;
    struct ngl_node_funcs funcs;
    enum ngpu_filter min_filter;
    enum ngpu_filter mag_filter;
};

struct customtexture_priv {
    struct texture_info texture_info;
#if defined(TARGET_ANDROID)
    struct ngli_ahardware_buffer ahardware_buffer;
    AHardwareBuffer *hardware_buffer;
    NGLI_DARRAY(struct ahb_texture) ahb_textures;
    uint64_t map_index;
#endif
};

#define OFFSET(x) offsetof(struct customtexture_opts, x)
static const struct node_param customtexture_params[] = {
    {
        .key       = "min_filter",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(min_filter),
        .def_value = {.i32 = NGPU_FILTER_LINEAR},
        .choices   = &ngli_filter_choices,
        .desc      = NGLI_DOCSTRING("texture minifying function"),
    },
    {
        .key       = "mag_filter",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(mag_filter),
        .def_value = {.i32 = NGPU_FILTER_LINEAR},
        .choices   = &ngli_filter_choices,
        .desc      = NGLI_DOCSTRING("texture magnification function"),
    },
    {NULL}
};

#if defined(TARGET_ANDROID)
static void reset_ahb_texture(void *user_arg, void *data)
{
    struct android_ctx *android_ctx = user_arg;
    struct ahb_texture *ahb_texture = data;
    ngpu_texture_freep(&ahb_texture->texture);
    android_ctx->AHardwareBuffer_release(ahb_texture->buffer);
}
#endif

static int customtexture_init(struct ngl_node *node)
{
    struct customtexture_priv *s = node->priv_data;
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;
    s->texture_info.resource = ngli_image_resource_create();
    if (!s->texture_info.resource)
        return NGL_ERROR_MEMORY;

#if defined(TARGET_ANDROID)
    ngli_darray_set_free_func(&s->ahb_textures, reset_ahb_texture, &node->ctx->android_ctx);
#endif

    if (!funcs->init)
        return 0;

    int ret = funcs->init(NULL, o->user_data);
    if (ret < 0)
        return ret;

    return 0;
}

static int customtexture_prefetch(struct ngl_node *node)
{
    struct customtexture_priv *s = node->priv_data;
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

    if (funcs->prefetch) {
        int ret = funcs->prefetch(NULL, o->user_data);
        if (ret < 0)
            return ret;
    }
    ngli_image_resource_set(s->texture_info.resource, s->texture_info.image);
    return 0;
}

static int customtexture_prepare(struct ngl_node *node,
                                 const struct ngpu_rendertarget_layout *rendertarget_layout)
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

#if defined(TARGET_ANDROID)
static void release_ahardware_buffer(struct ngl_node *node)
{
    struct customtexture_priv *s = node->priv_data;
    if (s->ahardware_buffer.custom_texture.release) {
        struct ngli_ahardware_buffer buffer = s->ahardware_buffer;
        s->ahardware_buffer = (struct ngli_ahardware_buffer){0};
        if (node->ctx->config.backend == NGL_BACKEND_VULKAN)
            buffer.custom_texture.texture = s->texture_info.texture;
        ngli_ctx_release_ahardware_buffer(node->ctx, buffer);
    }
}
#endif

static void customtexture_release(struct ngl_node *node)
{
    struct customtexture_priv *s = node->priv_data;
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

#if defined(TARGET_ANDROID)
    if (s->ahardware_buffer.custom_texture.release) {
        release_ahardware_buffer(node);
        ngli_image_unrefp(&s->texture_info.image);
        ngpu_texture_freep(&s->texture_info.texture);
        s->hardware_buffer = NULL;
    }
    ngli_darray_clear(&s->ahb_textures);
#endif
    if (funcs->release)
        funcs->release(NULL, o->user_data);
    ngli_image_resource_set(s->texture_info.resource, NULL);
}

static void customtexture_unprepare(struct ngl_node *node)
{
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

    if (!funcs->unprepare)
        return;

    funcs->unprepare(NULL, o->user_data);
}

static void customtexture_uninit(struct ngl_node *node)
{
    struct customtexture_priv *s = node->priv_data;
    const struct customtexture_opts *o = node->opts;
    const struct ngl_node_funcs *funcs = &o->funcs;

#if defined(TARGET_ANDROID)
    release_ahardware_buffer(node);
    s->hardware_buffer = NULL;
    ngli_darray_reset(&s->ahb_textures);
#endif
    ngli_image_unrefp(&s->texture_info.image);
    ngpu_texture_freep(&s->texture_info.texture);

    if (funcs->uninit)
        funcs->uninit(NULL, o->user_data);
    ngli_image_resource_releasep(&s->texture_info.resource);
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

static enum ngli_image_layout target_to_layout(uint32_t target)
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
    const struct customtexture_opts *o = node->opts;
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
        .min_filter = o->min_filter,
        .mag_filter = o->mag_filter,
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

    const struct ngli_image_params image_params = {
        .width                = info->width,
        .height               = info->height,
        .layout               = target_to_layout(info->target),
        .planes               = {s->texture_info.texture},
        .color_scale          = 1.f,
        .color_info           = NGLI_COLOR_INFO_DEFAULTS,
        .color_matrix         = {.m = NGLI_MAT4_IDENTITY},
        .mapping_color_matrix = {.m = NGLI_MAT4_IDENTITY},
        .coordinates_matrix   = {.m = NGLI_MAT4_IDENTITY},
    };
    struct ngli_image *image = ngli_image_create(&image_params);
    if (!image)
        return NGL_ERROR_MEMORY;
    ngli_image_unrefp(&s->texture_info.image);
    s->texture_info.image = image;
    ngli_image_resource_set(s->texture_info.resource, image);

    return 0;
}
#else
static int import_texture_gl(struct ngl_node *node, const struct ngl_custom_texture_info_gl *info)
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

#if defined(TARGET_ANDROID)
    release_ahardware_buffer(node);
    s->hardware_buffer = NULL;
    ngli_darray_clear(&s->ahb_textures);
#endif
    /* Cleanup previous texture/image */
    ngli_image_resource_set(s->texture_info.resource, NULL);
    ngli_image_unrefp(&s->texture_info.image);
    ngpu_texture_freep(&s->texture_info.texture);
    if (!info) {
        ngli_node_invalidate_branch(node);
        return 0;
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

    ngli_node_invalidate_branch(node);
    return 0;
}

#if defined(TARGET_ANDROID)
static int create_ahb_texture(struct ngl_node *node, const struct ngl_custom_texture_info_ahb *info,
                              struct ngpu_texture **texturep)
{
    const struct customtexture_opts *o = node->opts;
    struct ngpu_ctx *gpu_ctx = node->ctx->gpu_ctx;

    struct ngpu_texture_params texture_params = {
        .type       = NGPU_TEXTURE_TYPE_2D,
        .format     = NGPU_FORMAT_R8G8B8A8_UNORM,
        .width      = info->width,
        .height     = info->height,
        .min_filter = o->min_filter,
        .mag_filter = o->mag_filter,
        .usage      = NGPU_TEXTURE_USAGE_SAMPLED_BIT,
        .import_params = {
            .type = NGPU_IMPORT_TYPE_AHARDWARE_BUFFER,
            .ahardware_buffer = {
                .hardware_buffer   = info->hardware_buffer,
                .ycbcr_sampler     = NULL,
                .acquire_fence_fd  = info->acquire_fence_fd,
            },
        },
    };

    struct ngpu_texture *texture = ngpu_texture_create(gpu_ctx);
    if (!texture)
        return NGL_ERROR_MEMORY;

    int ret = ngpu_texture_init(texture, &texture_params);
    if (ret < 0) {
        ngpu_texture_freep(&texture);
        return ret;
    }

    *texturep = texture;
    return 0;
}

static int acquire_ahb_texture(struct ngl_node *node, struct ngpu_texture *texture, int acquire_fence_fd)
{
    const enum ngpu_backend_type backend = ngpu_ctx_get_backend_type(node->ctx->gpu_ctx);
#if defined(BACKEND_GL) || defined(BACKEND_GLES)
    if (backend == NGPU_BACKEND_OPENGL || backend == NGPU_BACKEND_OPENGLES)
        return ngpu_texture_gl_acquire_external(texture, acquire_fence_fd);
#endif
#if defined(BACKEND_VK)
    if (backend == NGPU_BACKEND_VULKAN)
        return ngpu_texture_vk_acquire_external(texture, acquire_fence_fd);
#endif
    return NGL_ERROR_UNSUPPORTED;
}

static bool is_stale_ahb_texture(const struct customtexture_priv *s, const struct ahb_texture *ahb_texture)
{
    return s->map_index - ahb_texture->map_index > MAX_AHB_TEXTURES;
}

static bool has_buffer(const AHardwareBuffer *buffer, const struct ahb_texture *ahb_texture)
{
    return ahb_texture->buffer == buffer;
}

static bool has_other_size(const struct ngl_custom_texture_info_ahb *info, const struct ahb_texture *ahb_texture)
{
    return ahb_texture->width != info->width || ahb_texture->height != info->height;
}

static size_t get_lru_ahb_texture(const struct customtexture_priv *s)
{
    size_t lru = 0;
    for (size_t i = 1; i < s->ahb_textures.count; i++) {
        if (s->ahb_textures.data[i].map_index < s->ahb_textures.data[lru].map_index)
            lru = i;
    }
    return lru;
}

static int get_ahb_texture(struct ngl_node *node, const struct ngl_custom_texture_info_ahb *info,
                           struct ngpu_texture **texturep)
{
    struct customtexture_priv *s = node->priv_data;
    struct android_ctx *android_ctx = &node->ctx->android_ctx;

    s->map_index++;
    ngli_darray_remove_if(&s->ahb_textures, is_stale_ahb_texture, s);

    /* The producer allocates new buffers when it is resized */
    ngli_darray_remove_if(&s->ahb_textures, has_other_size, info);

    for (size_t i = 0; i < s->ahb_textures.count; i++) {
        struct ahb_texture *ahb_texture = &s->ahb_textures.data[i];
        if (ahb_texture->buffer != info->hardware_buffer)
            continue;

        int ret = acquire_ahb_texture(node, ahb_texture->texture, info->acquire_fence_fd);
        if (ret < 0) {
            ngli_darray_remove(&s->ahb_textures, i);
            return ret;
        }

        ahb_texture->map_index = s->map_index;
        *texturep = ngpu_texture_ref(ahb_texture->texture);
        return 0;
    }

    if (s->ahb_textures.count == MAX_AHB_TEXTURES)
        ngli_darray_remove(&s->ahb_textures, get_lru_ahb_texture(s));

    struct ngpu_texture *texture;
    int ret = create_ahb_texture(node, info, &texture);
    if (ret < 0)
        return ret;

    android_ctx->AHardwareBuffer_acquire(info->hardware_buffer);
    ngli_darray_push(&s->ahb_textures, (struct ahb_texture){
        .buffer    = info->hardware_buffer,
        .width     = info->width,
        .height    = info->height,
        .texture   = ngpu_texture_ref(texture),
        .map_index = s->map_index,
    });

    *texturep = texture;
    return 0;
}

static int import_texture_ahb(struct ngl_node *node, const struct ngl_custom_texture_info_ahb *info)
{
    struct customtexture_priv *s = node->priv_data;
    struct ngpu_ctx *gpu_ctx = node->ctx->gpu_ctx;

    const enum ngpu_backend_type backend = ngpu_ctx_get_backend_type(gpu_ctx);

    /* A buffer submitted again before its release is not acquired a second time */
    int ret;
    if (info->hardware_buffer == s->hardware_buffer)
        ret = create_ahb_texture(node, info, &s->texture_info.texture);
    else
        ret = get_ahb_texture(node, info, &s->texture_info.texture);
    if (ret < 0)
        return ret;

    enum ngli_image_layout layout = NGLI_IMAGE_LAYOUT_DEFAULT;
    if (backend == NGPU_BACKEND_OPENGL || backend == NGPU_BACKEND_OPENGLES)
        layout = NGLI_IMAGE_LAYOUT_MEDIACODEC;

    const struct ngli_image_params image_params = {
        .width                = info->width,
        .height               = info->height,
        .layout               = layout,
        .planes               = {s->texture_info.texture},
        .color_scale          = 1.f,
        .color_info           = NGLI_COLOR_INFO_DEFAULTS,
        .color_matrix         = {.m = NGLI_MAT4_IDENTITY},
        .mapping_color_matrix = {.m = NGLI_MAT4_IDENTITY},
        .coordinates_matrix   = {.m = NGLI_MAT4_IDENTITY},
    };
    struct ngli_image *image = ngli_image_create(&image_params);
    if (!image) {
        /* The texture is acquired but will not be released */
        ngli_darray_remove_if(&s->ahb_textures, has_buffer, info->hardware_buffer);
        return NGL_ERROR_MEMORY;
    }
    ngli_image_unrefp(&s->texture_info.image);
    s->texture_info.image = image;
    ngli_image_resource_set(s->texture_info.resource, image);

    return 0;
}

static int custom_texture_set_texture_info_ahb(struct ngl_node *node, const struct ngl_custom_texture_info_ahb *info)
{
    if (!node)
        return NGL_ERROR_INVALID_ARG;

    if (node->cls->id != NGL_NODE_CUSTOMTEXTURE)
        return NGL_ERROR_UNSUPPORTED;

    if (!node->ctx)
        return NGL_ERROR_UNSUPPORTED;

    struct customtexture_priv *s = node->priv_data;

    release_ahardware_buffer(node);
    /* Cleanup previous texture/image */
    ngli_image_resource_set(s->texture_info.resource, NULL);
    ngli_image_unrefp(&s->texture_info.image);
    ngpu_texture_freep(&s->texture_info.texture);
    if (!info || !info->hardware_buffer) {
        s->hardware_buffer = NULL;
        ngli_darray_clear(&s->ahb_textures);
        ngli_node_invalidate_branch(node);
        return 0;
    }

    int ret = import_texture_ahb(node, info);
    if (ret < 0) {
        s->hardware_buffer = NULL;
        return ret;
    }

    s->hardware_buffer = info->hardware_buffer;
    s->ahardware_buffer = (struct ngli_ahardware_buffer){
        .type           = NGLI_AHARDWARE_BUFFER_TYPE_CUSTOM_TEXTURE,
        .custom_texture = {.release = info->release, .arg = info->release_user_data},
    };
    ngli_node_invalidate_branch(node);
    return 0;
}

int ngl_custom_texture_set_texture_info_ahb(struct ngl_node *node, const struct ngl_custom_texture_info_ahb *info)
{
    int ret = custom_texture_set_texture_info_ahb(node, info);
    if (info && info->acquire_fence_fd >= 0)
        close(info->acquire_fence_fd);
    return ret;
}
#endif

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
    .unprepare      = customtexture_unprepare,
    .uninit         = customtexture_uninit,
    .free           = customtexture_free,
    .priv_size      = sizeof(struct customtexture_priv),
    .opts_size      = sizeof(struct customtexture_opts),
    .params         = customtexture_params,
    .flags          = NGLI_NODE_FLAG_SHAREABLE,
    .file           = __FILE__,
};
