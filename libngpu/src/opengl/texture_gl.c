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

#include "config.h"

#if defined(TARGET_LINUX)
#include <unistd.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

#include "egl.h"
#endif

#if defined(TARGET_ANDROID)
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <unistd.h>

#include "egl.h"
#endif

#if defined(TARGET_DARWIN)
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#endif

#if defined(TARGET_IPHONE)
#include <CoreVideo/CoreVideo.h>
#endif

#include <string.h>


#include "utils/log.h"
#include "opengl/ctx_gl.h"
#include "opengl/format_gl.h"
#include "opengl/glcontext.h"
#include "opengl/glincludes.h"
#include "opengl/texture_gl.h"
#include "utils/bits.h"
#include "utils/memory.h"
#include "utils/utils.h"

static const GLint gl_filter_map[NGPU_NB_FILTER][NGPU_NB_MIPMAP] = {
    [NGPU_FILTER_NEAREST] = {
        [NGPU_MIPMAP_FILTER_NONE]    = GL_NEAREST,
        [NGPU_MIPMAP_FILTER_NEAREST] = GL_NEAREST_MIPMAP_NEAREST,
        [NGPU_MIPMAP_FILTER_LINEAR]  = GL_NEAREST_MIPMAP_LINEAR,
    },
    [NGPU_FILTER_LINEAR] = {
        [NGPU_MIPMAP_FILTER_NONE]    = GL_LINEAR,
        [NGPU_MIPMAP_FILTER_NEAREST] = GL_LINEAR_MIPMAP_NEAREST,
        [NGPU_MIPMAP_FILTER_LINEAR]  = GL_LINEAR_MIPMAP_LINEAR,
    },
};

GLint ngpu_texture_get_gl_min_filter(enum ngpu_filter min_filter, enum ngpu_mipmap_filter mipmap_filter)
{
    return gl_filter_map[min_filter][mipmap_filter];
}

GLint ngpu_texture_get_gl_mag_filter(enum ngpu_filter mag_filter)
{
    return gl_filter_map[mag_filter][NGPU_MIPMAP_FILTER_NONE];
}

static const GLint gl_wrap_map[NGPU_NB_WRAP] = {
    [NGPU_WRAP_CLAMP_TO_EDGE]   = GL_CLAMP_TO_EDGE,
    [NGPU_WRAP_MIRRORED_REPEAT] = GL_MIRRORED_REPEAT,
    [NGPU_WRAP_REPEAT]          = GL_REPEAT,
};

GLint ngpu_texture_get_gl_wrap(enum ngpu_wrap wrap)
{
    return gl_wrap_map[wrap];
}

static GLbitfield get_gl_barriers(uint32_t usage)
{
    GLbitfield barriers = 0;
    if (usage & NGPU_TEXTURE_USAGE_TRANSFER_SRC_BIT)
        barriers |= GL_TEXTURE_UPDATE_BARRIER_BIT;
    if (usage & NGPU_TEXTURE_USAGE_TRANSFER_DST_BIT)
        barriers |= GL_TEXTURE_UPDATE_BARRIER_BIT;
    if (usage & NGPU_TEXTURE_USAGE_STORAGE_BIT)
        barriers |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
    if (usage & NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT)
        barriers |= GL_FRAMEBUFFER_BARRIER_BIT;
    return barriers;
}

static void texture_allocate(struct ngpu_texture *s)
{
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct ngpu_texture_params *params = &s->params;

    const GLint internal_format = (GLint)s_priv->internal_format;
    const GLint width = (GLint)params->width;
    const GLint height = (GLint)params->height;
    const GLint depth = (GLint)params->depth;
    const GLint array_layers = (GLint)s_priv->array_layers;

    switch (s_priv->target) {
    case GL_TEXTURE_2D:
        gl->funcs.TexImage2D(s_priv->target, 0, internal_format, width, height, 0, s_priv->format, s_priv->format_type, NULL);
        break;
    case GL_TEXTURE_2D_ARRAY:
        gl->funcs.TexImage3D(s_priv->target, 0, internal_format, width, height, array_layers, 0, s_priv->format, s_priv->format_type, NULL);
        break;
    case GL_TEXTURE_3D:
        gl->funcs.TexImage3D(s_priv->target, 0, internal_format, width, height, depth, 0, s_priv->format, s_priv->format_type, NULL);
        break;
    case GL_TEXTURE_CUBE_MAP: {
        for (int face = 0; face < 6; face++) {
            GLenum target = (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face);
            gl->funcs.TexImage2D(target, 0, internal_format, width, height, 0, s_priv->format, s_priv->format_type, NULL);
        }
        break;
    }
    }
}

static uint32_t get_mipmap_levels(const struct ngpu_texture *s)
{
    const struct ngpu_texture_params *params = &s->params;

    uint32_t mipmap_levels = 1;
    if (params->mipmap_filter != NGPU_MIPMAP_FILTER_NONE)
        mipmap_levels = ngpu_log2(params->width | params->height | 1);
    return mipmap_levels;
}

static void texture_allocate_storage(struct ngpu_texture *s)
{
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct ngpu_texture_params *params = &s->params;

    const GLint width = (GLint)params->width;
    const GLint height = (GLint)params->height;
    const GLint depth = (GLint)params->depth;
    const GLint array_layers = (GLint)s_priv->array_layers;
    const GLint mipmap_levels = (GLint)get_mipmap_levels(s);

    switch (s_priv->target) {
    case GL_TEXTURE_2D:
        gl->funcs.TexStorage2D(s_priv->target, mipmap_levels, s_priv->internal_format, width, height);
        break;
    case GL_TEXTURE_2D_ARRAY:
        gl->funcs.TexStorage3D(s_priv->target, mipmap_levels, s_priv->internal_format, width, height, array_layers);
        break;
    case GL_TEXTURE_3D:
        gl->funcs.TexStorage3D(s_priv->target, 1, s_priv->internal_format, width, height, depth);
        break;
    case GL_TEXTURE_CUBE_MAP:
        /* glTexStorage2D automatically accomodates for 6 faces when using the cubemap target */
        gl->funcs.TexStorage2D(s_priv->target, mipmap_levels, s_priv->internal_format, width, height);
        break;
    }
}

static void texture_upload(struct ngpu_texture *s, const uint8_t *data, const struct ngpu_texture_transfer_params *transfer_params)
{
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    const size_t pixels_per_row = (size_t)transfer_params->pixels_per_row;
    const size_t bytes_per_row = pixels_per_row * s_priv->bytes_per_pixel;
    const size_t alignment = NGPU_MIN(NGPU_ALIGNMENT(bytes_per_row), 8);
    gl->funcs.PixelStorei(GL_UNPACK_ALIGNMENT, (GLint)alignment);
    gl->funcs.PixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)transfer_params->pixels_per_row);

    const GLint x = (GLint)transfer_params->x;
    const GLint y = (GLint)transfer_params->y;
    const GLint z = (GLint)transfer_params->z;
    const GLint width = (GLint)transfer_params->width;
    const GLint height = (GLint)transfer_params->height;
    const GLint depth = (GLint)transfer_params->depth;
    const GLint base_layer = (GLint)transfer_params->base_layer;
    const GLint layer_count = (GLint)transfer_params->layer_count;

    switch (s_priv->target) {
    case GL_TEXTURE_2D:
        gl->funcs.TexSubImage2D(s_priv->target, 0, x, y, width, height,
                                s_priv->format, s_priv->format_type, data);
        break;
    case GL_TEXTURE_2D_ARRAY:
        gl->funcs.TexSubImage3D(s_priv->target, 0, x, y, base_layer,
                                width, height, layer_count,
                                s_priv->format, s_priv->format_type, data);
        break;
    case GL_TEXTURE_3D:
        gl->funcs.TexSubImage3D(s_priv->target, 0, x, y, z,
                                width, height, depth,
                                s_priv->format, s_priv->format_type, data);
        break;
    case GL_TEXTURE_CUBE_MAP: {
        const size_t layer_size = bytes_per_row * transfer_params->height;
        const uint8_t *layer_data = data + (transfer_params->base_layer * layer_size);
        for (uint32_t face = transfer_params->base_layer; face < transfer_params->layer_count; face++) {
            gl->funcs.TexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, 0, 0, width, height,
                                    s_priv->format, s_priv->format_type, layer_data);
            layer_data += layer_size;
        }
        break;
    }
    }

    gl->funcs.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->funcs.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static int renderbuffer_check_samples(struct ngpu_texture *s)
{
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct ngpu_limits *limits = &gl->limits;
    const struct ngpu_texture_params *params = &s->params;

    GLint max_samples = (GLint)limits->max_samples;
    if (gl->features & NGPU_FEATURE_GL_INTERNALFORMAT_QUERY)
        gl->funcs.GetInternalformativ(GL_RENDERBUFFER, s_priv->format, GL_SAMPLES, 1, &max_samples);

    if (params->samples > max_samples) {
        LOG(WARNING, "renderbuffer format 0x%x does not support samples %u (maximum %d)",
            s_priv->format, params->samples, max_samples);
        return NGPU_ERROR_GRAPHICS_UNSUPPORTED;
    }

    return 0;
}

static void renderbuffer_allocate_storage(struct ngpu_texture *s)
{
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct ngpu_texture_params *params = &s->params;

    const GLint width = (GLint)params->width;
    const GLint height = (GLint)params->height;
    const GLint samples = (GLint)params->samples;

    if (params->samples > 0)
        gl->funcs.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, s_priv->format, width, height);
    else
        gl->funcs.RenderbufferStorage(GL_RENDERBUFFER, s_priv->format, width, height);
}

#define COLOR_USAGE NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT
#define DEPTH_USAGE NGPU_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
#define TRANSIENT_COLOR_USAGE (COLOR_USAGE | NGPU_TEXTURE_USAGE_TRANSIENT_ATTACHMENT_BIT)
#define TRANSIENT_DEPTH_USAGE (DEPTH_USAGE | NGPU_TEXTURE_USAGE_TRANSIENT_ATTACHMENT_BIT)

static int texture_init_fields(struct ngpu_texture *s, const struct ngpu_texture_params *params)
{
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    s->params = *params;

    if (!s_priv->wrapped)
        ngpu_assert(params->width && params->height);

    uint32_t depth = 1;
    if (params->type == NGPU_TEXTURE_TYPE_3D) {
        if (!s_priv->wrapped)
            ngpu_assert(params->depth);
        depth = params->depth;
    }
    s->params.depth = depth;

#if defined(TARGET_LINUX) || defined(TARGET_ANDROID)
    s_priv->fd = -1;
#endif

    s_priv->array_layers = 1;
    if (params->type == NGPU_TEXTURE_TYPE_CUBE) {
        s_priv->array_layers = 6;
    } else if (params->type == NGPU_TEXTURE_TYPE_2D_ARRAY) {
        s_priv->array_layers = params->depth;
    }

    if (!s_priv->wrapped &&
        (params->usage == COLOR_USAGE ||
         params->usage == DEPTH_USAGE ||
         params->usage == TRANSIENT_COLOR_USAGE ||
         params->usage == TRANSIENT_DEPTH_USAGE)) {
        const struct ngpu_format_gl *format_gl = ngpu_format_get_gl_texture_format(gl, params->format);

        s_priv->target = GL_RENDERBUFFER;
        s_priv->format = format_gl->internal_format;
        s_priv->internal_format = format_gl->internal_format;

        int ret = renderbuffer_check_samples(s);
        if (ret < 0)
            return ret;
        return 0;
    }

    /* TODO: add multisample support for textures */
    ngpu_assert(!params->samples);

    if (params->type == NGPU_TEXTURE_TYPE_2D)
        s_priv->target = GL_TEXTURE_2D;
    else if (params->type == NGPU_TEXTURE_TYPE_2D_ARRAY)
        s_priv->target = GL_TEXTURE_2D_ARRAY;
    else if (params->type == NGPU_TEXTURE_TYPE_3D)
        s_priv->target = GL_TEXTURE_3D;
    else if (params->type == NGPU_TEXTURE_TYPE_CUBE)
        s_priv->target = GL_TEXTURE_CUBE_MAP;
    else
        ngpu_assert(0);

    if (params->import_params.type == NGPU_IMPORT_TYPE_AHARDWARE_BUFFER)
        s_priv->target = GL_TEXTURE_EXTERNAL_OES;
    if (params->import_params.type == NGPU_IMPORT_TYPE_IOSURFACE)
        s_priv->target = GL_TEXTURE_RECTANGLE;

    const struct ngpu_format_gl *format_gl = ngpu_format_get_gl_texture_format(gl, params->format);
    s_priv->format          = format_gl->format;
    s_priv->internal_format = format_gl->internal_format;
    s_priv->format_type     = format_gl->type;
    s_priv->bytes_per_pixel = ngpu_format_get_bytes_per_pixel(params->format);
    s_priv->barriers = get_gl_barriers(params->usage);

    return 0;
}

static int texture_init(struct ngpu_texture *s, const struct ngpu_texture_params *params)
{
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    if (s_priv->target == GL_RENDERBUFFER) {
        gl->funcs.GenRenderbuffers(1, &s_priv->id);
        gl->funcs.BindRenderbuffer(s_priv->target, s_priv->id);
        renderbuffer_allocate_storage(s);
        return 0;
    }

    gl->funcs.GenTextures(1, &s_priv->id);
    gl->funcs.BindTexture(s_priv->target, s_priv->id);
    const GLint min_filter = ngpu_texture_get_gl_min_filter(params->min_filter, s->params.mipmap_filter);
    const GLint mag_filter = ngpu_texture_get_gl_mag_filter(params->mag_filter);
    const GLint wrap_s = ngpu_texture_get_gl_wrap(params->wrap_s);
    const GLint wrap_t = ngpu_texture_get_gl_wrap(params->wrap_t);
    const GLint wrap_r = ngpu_texture_get_gl_wrap(params->wrap_r);
    gl->funcs.TexParameteri(s_priv->target, GL_TEXTURE_MIN_FILTER, min_filter);
    gl->funcs.TexParameteri(s_priv->target, GL_TEXTURE_MAG_FILTER, mag_filter);
    gl->funcs.TexParameteri(s_priv->target, GL_TEXTURE_WRAP_S, wrap_s);
    gl->funcs.TexParameteri(s_priv->target, GL_TEXTURE_WRAP_T, wrap_t);
    if (s_priv->target == GL_TEXTURE_2D_ARRAY ||
        s_priv->target == GL_TEXTURE_3D ||
        s_priv->target == GL_TEXTURE_CUBE_MAP)
        gl->funcs.TexParameteri(s_priv->target, GL_TEXTURE_WRAP_R, wrap_r);

    return 0;
}

struct ngpu_texture *ngpu_texture_gl_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngpu_texture_gl *s = ngpu_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->parent.gpu_ctx = gpu_ctx;
    return (struct ngpu_texture *)s;
}

#define ADD_ATTRIB(name, value) do {                          \
    ngpu_assert(nb_attribs + 3 < NGPU_ARRAY_NB(attribs));     \
    attribs[nb_attribs++] = (name);                           \
    attribs[nb_attribs++] = (EGLint)(value);                  \
    attribs[nb_attribs] = EGL_NONE;                           \
} while(0)

static int texture_import_dma_buf(struct ngpu_texture *s)
{
#if defined(TARGET_LINUX)
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    const struct ngpu_import_params *import_params = &s->params.import_params;
    const struct ngpu_import_dma_buf_params *dma_buf_params = &import_params->dma_buf;

    s_priv->fd = dup(dma_buf_params->fd);
    if (s_priv->fd == -1) {
        LOG(ERROR, "could not dup file descriptor (fd=%d)", dma_buf_params->fd);
        return NGPU_ERROR_EXTERNAL;
    }

    EGLint attribs[32] = {EGL_NONE};
    size_t nb_attribs = 0;

    ADD_ATTRIB(EGL_WIDTH,  s->params.width);
    ADD_ATTRIB(EGL_HEIGHT, s->params.height);
    ADD_ATTRIB(EGL_LINUX_DRM_FOURCC_EXT, import_params->dma_buf.drm_format);

    ADD_ATTRIB(EGL_DMA_BUF_PLANE0_FD_EXT, s_priv->fd);
    ADD_ATTRIB(EGL_DMA_BUF_PLANE0_OFFSET_EXT, dma_buf_params->offset);
    ADD_ATTRIB(EGL_DMA_BUF_PLANE0_PITCH_EXT, dma_buf_params->pitch);

    const bool use_drm_format_mod =
        NGPU_HAS_ALL_FLAGS(gl->features, NGPU_FEATURE_GL_EGL_EXT_IMAGE_DMA_BUF_IMPORT_MODIFIERS);
    if (use_drm_format_mod && dma_buf_params->drm_format_mod != DRM_FORMAT_MOD_INVALID) {
        ADD_ATTRIB(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (uint32_t)(dma_buf_params->drm_format_mod & 0xFFFFFFFFLU));
        ADD_ATTRIB(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (uint32_t)((dma_buf_params->drm_format_mod >> 32U) & 0xFFFFFFFFLU));
    }

    s_priv->egl_image = ngpu_eglCreateImageKHR(gl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (!s_priv->egl_image) {
        LOG(ERROR, "failed to create egl image");
        return NGPU_ERROR_EXTERNAL;
    }

    gl->funcs.BindTexture(s_priv->target, s_priv->id);
    if (gl->features & NGPU_FEATURE_GL_EXT_EGL_IMAGE_STORAGE)
        gl->funcs.EGLImageTargetTexStorageEXT(s_priv->target, s_priv->egl_image, NULL);
    else
        gl->funcs.EGLImageTargetTexture2DOES(s_priv->target, s_priv->egl_image);
    gl->funcs.BindTexture(s_priv->target, 0);

    return 0;
#else
    return NGPU_ERROR_UNSUPPORTED;
#endif
}

static int texture_import_android_hardware_buffer(struct ngpu_texture *s)
{
#if defined(TARGET_ANDROID)
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    const struct ngpu_import_params *import_params = &s->params.import_params;
    const struct ngpu_import_ahardware_buffer_params *ahb_params = &import_params->ahardware_buffer;
    AHardwareBuffer *hardware_buffer = ahb_params->hardware_buffer;

    EGLClientBuffer egl_buffer = ngpu_eglGetNativeClientBufferANDROID(gl, hardware_buffer);
    if (!egl_buffer)
        return NGPU_ERROR_EXTERNAL;

    static const EGLint attrs[] = {
        EGL_IMAGE_PRESERVED_KHR,
        EGL_TRUE,
        EGL_NONE,
    };

    s_priv->egl_image = ngpu_eglCreateImageKHR(gl, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, egl_buffer, attrs);
    if (!s_priv->egl_image) {
        LOG(ERROR, "failed to create egl image");
        return NGPU_ERROR_EXTERNAL;
    }

    gl->funcs.BindTexture(GL_TEXTURE_EXTERNAL_OES, s_priv->id);
    if (gl->features & NGPU_FEATURE_GL_EXT_EGL_IMAGE_STORAGE)
        gl->funcs.EGLImageTargetTexStorageEXT(s_priv->target, s_priv->egl_image, NULL);
    else
        gl->funcs.EGLImageTargetTexture2DOES(s_priv->target, s_priv->egl_image);
    gl->funcs.EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, s_priv->egl_image);

    return 0;
#else
    return NGPU_ERROR_UNSUPPORTED;
#endif
}

static int texture_import_iosurface(struct ngpu_texture *s)
{
#if defined(TARGET_DARWIN)
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    const struct ngpu_import_params *import_params = &s->params.import_params;
    const struct ngpu_import_iosurface_params *iosurface_params = &import_params->iosurface;
    IOSurfaceRef surface = iosurface_params->iosurface;
    size_t index = iosurface_params->plane;

    /* CGLTexImageIOSurface2D() requires GL_UNSIGNED_INT_8_8_8_8_REV instead of GL_UNSIGNED_SHORT to map BGRA IOSurface2D */
    const GLenum format_type = s_priv->format == GL_BGRA ? GL_UNSIGNED_INT_8_8_8_8_REV : s_priv->format_type;

    gl->funcs.BindTexture(GL_TEXTURE_RECTANGLE, s_priv->id);
    CGLError err = CGLTexImageIOSurface2D(CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE,
                                          s_priv->internal_format, (GLsizei)s->params.width, (GLsizei)s->params.height,
                                          s_priv->format, format_type, surface, (GLuint)index);
    if (err != kCGLNoError) {
        LOG(ERROR, "could not bind IOSurface plane %zu to texture %u: %s", index, s_priv->id, CGLErrorString(err));
        return NGPU_ERROR_EXTERNAL;
    }

    gl->funcs.BindTexture(GL_TEXTURE_RECTANGLE, 0);

    return 0;
#else
    return NGPU_ERROR_UNSUPPORTED;
#endif
}

static int texture_import_corevideo_buffer(struct ngpu_texture *s)
{
#if defined(TARGET_IPHONE)
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    const struct ngpu_import_params *import_params = &s->params.import_params;
    const struct ngpu_import_corevideo_buffer_params *cv_buffer_params = &import_params->corevideo_buffer;
    CVPixelBufferRef cv_pixel_buffer = cv_buffer_params->corevideo_buffer;
    size_t index = cv_buffer_params->plane;

    const size_t width  = CVPixelBufferGetWidthOfPlane(cv_pixel_buffer, index);
    const size_t height = CVPixelBufferGetHeightOfPlane(cv_pixel_buffer, index);
    if (width > INT_MAX || height > INT_MAX)
        return NGPU_ERROR_LIMIT_EXCEEDED;

    CVOpenGLESTextureCacheRef *cache = ngpu_glcontext_get_texture_cache(gl);
    CVReturn err = CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
                                                                *cache,
                                                                cv_pixel_buffer,
                                                                NULL,
                                                                GL_TEXTURE_2D,
                                                                (GLint)s_priv->internal_format,
                                                                (GLsizei)width,
                                                                (GLsizei)height,
                                                                s_priv->format,
                                                                s_priv->format_type,
                                                                index,
                                                                &s_priv->cv_texture);
    if (err != noErr) {
        LOG(ERROR, "could not create CoreVideo texture from image: %d", err);
        return NGPU_ERROR_EXTERNAL;
    }

    s_priv->cv_pixel_buffer = CFRetain(cv_pixel_buffer);

    /* Delete pre-allocated texture handle */
    gl->funcs.DeleteTextures(1, &s_priv->id);

    s_priv->id = CVOpenGLESTextureGetName(s_priv->cv_texture);
    s_priv->wrapped = 1;

    gl->funcs.BindTexture(GL_TEXTURE_2D, s_priv->id);
    const GLint min_filter = ngpu_texture_get_gl_min_filter(s->params.min_filter, s->params.mipmap_filter);
    const GLint mag_filter = ngpu_texture_get_gl_mag_filter(s->params.mag_filter);
    const GLint wrap_s = ngpu_texture_get_gl_wrap(s->params.wrap_s);
    const GLint wrap_t = ngpu_texture_get_gl_wrap(s->params.wrap_t);
    gl->funcs.TexParameteri(s_priv->target, GL_TEXTURE_MIN_FILTER, min_filter);
    gl->funcs.TexParameteri(s_priv->target, GL_TEXTURE_MAG_FILTER, mag_filter);
    gl->funcs.TexParameteri(s_priv->target, GL_TEXTURE_WRAP_S, wrap_s);
    gl->funcs.TexParameteri(s_priv->target, GL_TEXTURE_WRAP_T, wrap_t);
    gl->funcs.BindTexture(GL_TEXTURE_2D, 0);

    return 0;
#else
    return NGPU_ERROR_UNSUPPORTED;
#endif
}

static int texture_import_opengl_texture(struct ngpu_texture *s)
{
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    const struct ngpu_import_params *import_params = &s->params.import_params;
    const struct ngpu_import_opengl_texture_params *opengl_texture_params = &import_params->opengl_texture;

    /* Delete pre-allocated texture handle */
    gl->funcs.DeleteTextures(1, &s_priv->id);

    s_priv->id = opengl_texture_params->texture;
    s_priv->target = opengl_texture_params->target;

    s_priv->wrapped = 1;

    return 0;
}

int ngpu_texture_gl_init(struct ngpu_texture *s, const struct ngpu_texture_params *params)
{
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    int ret = texture_init_fields(s, params);
    if (ret < 0)
        return ret;

    ret = texture_init(s, params);
    if (ret < 0)
        return ret;

    if (gl->features & NGPU_FEATURE_GL_TEXTURE_STORAGE) {
        texture_allocate_storage(s);
    } else {
        texture_allocate(s);
    }

    return 0;
}

int ngpu_texture_gl_import(struct ngpu_texture *s, const struct ngpu_texture_params *params)
{
    int ret = texture_init_fields(s, params);
    if (ret < 0)
        return ret;

    ret = texture_init(s, params);
    if (ret < 0)
        return ret;

    const struct ngpu_import_params *import_params = &params->import_params;
    switch (import_params->type) {
    case NGPU_IMPORT_TYPE_DMA_BUF:
        ret = texture_import_dma_buf(s);
        break;
    case NGPU_IMPORT_TYPE_AHARDWARE_BUFFER:
        ret = texture_import_android_hardware_buffer(s);
        break;
    case NGPU_IMPORT_TYPE_IOSURFACE:
        ret = texture_import_iosurface(s);
        break;
    case NGPU_IMPORT_TYPE_COREVIDEO_BUFFER:
        ret = texture_import_corevideo_buffer(s);
        break;
    case NGPU_IMPORT_TYPE_OPENGL_TEXTURE:
        ret = texture_import_opengl_texture(s);
        break;
    default:
        ngpu_assert(0);
    }
    if (ret < 0)
        return ret;

    return 0;
}

int ngpu_texture_gl_upload(struct ngpu_texture *s, const uint8_t *data, uint32_t linesize)
{
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    const struct ngpu_texture_params *params = &s->params;
    const struct ngpu_texture_transfer_params transfer_params = {
        .width = params->width,
        .height = params->height,
        .depth = params->depth,
        .base_layer = 0,
        .layer_count = s_priv->array_layers,
        .pixels_per_row = linesize ? linesize : params->width,
    };

    return ngpu_texture_gl_upload_with_params(s, data, &transfer_params);
}

int ngpu_texture_gl_upload_with_params(struct ngpu_texture *s, const uint8_t *data, const struct ngpu_texture_transfer_params *transfer_params)
{
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct ngpu_texture_params *params = &s->params;

    /* Wrapped textures and render buffers cannot update their content with
     * this function */
    ngpu_assert(!s_priv->wrapped);
    ngpu_assert(params->usage & NGPU_TEXTURE_USAGE_TRANSFER_DST_BIT);

    gl->funcs.BindTexture(s_priv->target, s_priv->id);
    if (data) {
        texture_upload(s, data, transfer_params);
        if (params->mipmap_filter != NGPU_MIPMAP_FILTER_NONE)
            gl->funcs.GenerateMipmap(s_priv->target);
    }
    gl->funcs.BindTexture(s_priv->target, 0);

    return 0;
}

int ngpu_texture_gl_generate_mipmap(struct ngpu_texture *s)
{
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct ngpu_texture_params *params = &s->params;

    ngpu_assert(params->usage & NGPU_TEXTURE_USAGE_TRANSFER_SRC_BIT);
    ngpu_assert(params->usage & NGPU_TEXTURE_USAGE_TRANSFER_DST_BIT);

    gl->funcs.BindTexture(s_priv->target, s_priv->id);
    gl->funcs.GenerateMipmap(s_priv->target);
    return 0;
}

void ngpu_texture_gl_freep(struct ngpu_texture **sp)
{
    if (!*sp)
        return;

    struct ngpu_texture *s = *sp;
    struct ngpu_texture_gl *s_priv = (struct ngpu_texture_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    if (!s_priv->wrapped) {
        if (s_priv->target == GL_RENDERBUFFER)
            gl->funcs.DeleteRenderbuffers(1, &s_priv->id);
        else
            gl->funcs.DeleteTextures(1, &s_priv->id);
    }

#if defined(TARGET_LINUX) || defined(TARGET_ANDROID)
    if (s_priv->egl_image)
        ngpu_eglDestroyImageKHR(gl, s_priv->egl_image);

    if (s_priv->fd != -1)
        close(s_priv->fd);
#endif

#if defined(TARGET_IPHONE)
    if (s_priv->cv_pixel_buffer)
        CFRelease(s_priv->cv_pixel_buffer);
    if (s_priv->cv_texture)
        CFRelease(s_priv->cv_texture);
#endif

    ngpu_freep(sp);
}
