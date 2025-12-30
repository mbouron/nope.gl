/*
 * Copyright 2023-2024 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2016-2022 GoPro Inc.
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

#ifndef GLCONTEXT_H
#define GLCONTEXT_H

#include <stdlib.h>

#include "format_gl.h"
#include "glfunctions.h"
#include "ngpu/ngpu.h"

#define NGPU_FEATURE_GL_TEXTURE_STORAGE                            (1ULL << 2)
#define NGPU_FEATURE_GL_COMPUTE_SHADER                             (1ULL << 3)
#define NGPU_FEATURE_GL_PROGRAM_INTERFACE_QUERY                    (1ULL << 4)
#define NGPU_FEATURE_GL_SHADER_IMAGE_LOAD_STORE                    (1ULL << 5)
#define NGPU_FEATURE_GL_SHADER_STORAGE_BUFFER_OBJECT               (1ULL << 6)
#define NGPU_FEATURE_GL_INTERNALFORMAT_QUERY                       (1ULL << 8)
#define NGPU_FEATURE_GL_TIMER_QUERY                                (1ULL << 10)
#define NGPU_FEATURE_GL_EXT_DISJOINT_TIMER_QUERY                   (1ULL << 11)
#define NGPU_FEATURE_GL_INVALIDATE_SUBDATA                         (1ULL << 15)
#define NGPU_FEATURE_GL_OES_EGL_EXTERNAL_IMAGE                     (1ULL << 16)
#define NGPU_FEATURE_GL_EXT_EGL_IMAGE_STORAGE                      (1ULL << 17)
#define NGPU_FEATURE_GL_OES_EGL_IMAGE                              (1ULL << 19)
#define NGPU_FEATURE_GL_EGL_IMAGE_BASE_KHR                         (1ULL << 20)
#define NGPU_FEATURE_GL_EGL_EXT_IMAGE_DMA_BUF_IMPORT               (1ULL << 21)
#define NGPU_FEATURE_GL_YUV_TARGET                                 (1ULL << 23)
#define NGPU_FEATURE_GL_SOFTWARE                                   (1ULL << 28)
#define NGPU_FEATURE_GL_EGL_ANDROID_GET_IMAGE_NATIVE_CLIENT_BUFFER (1ULL << 30)
#define NGPU_FEATURE_GL_KHR_DEBUG                                  (1ULL << 31)
#define NGPU_FEATURE_GL_SHADER_IMAGE_SIZE                          (1ULL << 33)
#define NGPU_FEATURE_GL_SHADING_LANGUAGE_420PACK                   (1ULL << 34)
#define NGPU_FEATURE_GL_COLOR_BUFFER_FLOAT                         (1ULL << 36)
#define NGPU_FEATURE_GL_COLOR_BUFFER_HALF_FLOAT                    (1ULL << 37)
#define NGPU_FEATURE_GL_BUFFER_STORAGE                             (1ULL << 39)
#define NGPU_FEATURE_GL_EXT_BUFFER_STORAGE                         (1ULL << 40)
#define NGPU_FEATURE_GL_EGL_MESA_QUERY_DRIVER                      (1ULL << 41)
#define NGPU_FEATURE_GL_TEXTURE_NORM16                             (1ULL << 42)
#define NGPU_FEATURE_GL_TEXTURE_FLOAT_LINEAR                       (1ULL << 43)
#define NGPU_FEATURE_GL_FLOAT_BLEND                                (1ULL << 44)
#define NGPU_FEATURE_GL_EGL_EXT_IMAGE_DMA_BUF_IMPORT_MODIFIERS     (1ULL << 45)
#define NGPU_FEATURE_GL_VIEWPORT_ARRAY                             (1ULL << 46)

#define NGPU_FEATURE_GL_COMPUTE_SHADER_ALL (NGPU_FEATURE_GL_COMPUTE_SHADER | \
                                            NGPU_FEATURE_GL_PROGRAM_INTERFACE_QUERY | \
                                            NGPU_FEATURE_GL_SHADER_IMAGE_LOAD_STORE | \
                                            NGPU_FEATURE_GL_SHADER_IMAGE_SIZE | \
                                            NGPU_FEATURE_GL_SHADER_STORAGE_BUFFER_OBJECT)

struct glcontext_class;

struct glcontext_params {
    enum ngpu_platform_type platform;
    enum ngpu_backend_type backend;
    int external;
    uintptr_t display;
    uintptr_t window;
    uintptr_t shared_ctx;
    int swap_interval;
    int offscreen;
    uint32_t width;
    uint32_t height;
    uint32_t samples;
    int debug;
};

struct glcontext {
    /* GL context */
    const struct glcontext_class *cls;
    void *priv_data;

    /* User options */
    enum ngpu_platform_type platform;
    enum ngpu_backend_type backend;
    int external;
    int offscreen;
    uint32_t width;
    uint32_t height;
    uint32_t samples;
    int debug;

    /* GL api */
    int version;

    /* GLSL version */
    int glsl_version;

    /* GL features */
    uint64_t features;

    /* GL limits */
    struct ngpu_limits limits;

    /* GL functions */
    struct glfunctions funcs;

    /* GL timer functions */
    struct {
        void (NGLI_GL_APIENTRY *GenQueries)(GLsizei n, GLuint * ids);
        void (NGLI_GL_APIENTRY *DeleteQueries)(GLsizei n, const GLuint *ids);
        void (NGLI_GL_APIENTRY *BeginQuery)(GLenum target, GLuint id);
        void (NGLI_GL_APIENTRY *EndQuery)(GLenum target);
        void (NGLI_GL_APIENTRY *QueryCounter)(GLuint id, GLenum target);
        void (NGLI_GL_APIENTRY *GetQueryObjectui64v)(GLuint id, GLenum pname, GLuint64 *params);
    } timer_funcs;

    /* GL formats */
    struct ngpu_format_gl formats[NGPU_FORMAT_NB];

    /*
     * Workaround a radeonsi sync issue between fbo writes and compute reads
     * using 2D samplers.
     *
     * See: https://gitlab.freedesktop.org/mesa/mesa/-/issues/8906
     */
    int workaround_radeonsi_sync;
};

struct glcontext_class {
    int (*init)(struct glcontext *glcontext, uintptr_t display, uintptr_t window, uintptr_t handle);
    int (*resize)(struct glcontext *glcontext, uint32_t width, uint32_t height);
    int (*make_current)(struct glcontext *glcontext, int current);
    void (*swap_buffers)(struct glcontext *glcontext);
    int (*set_swap_interval)(struct glcontext *glcontext, int interval);
    void (*set_surface_pts)(struct glcontext *glcontext, double t);
    void* (*get_texture_cache)(struct glcontext *glcontext);
    void* (*get_proc_address)(struct glcontext *glcontext, const char *name);
    uintptr_t (*get_display)(struct glcontext *glcontext);
    uintptr_t (*get_handle)(struct glcontext *glcontext);
    GLuint (*get_default_framebuffer)(struct glcontext *glcontext);
    void (*uninit)(struct glcontext *glcontext);
    size_t priv_size;
};

struct glcontext *ngpu_glcontext_create(const struct glcontext_params *params);
int ngpu_glcontext_make_current(struct glcontext *glcontext, int current);
void ngpu_glcontext_swap_buffers(struct glcontext *glcontext);
int ngpu_glcontext_set_swap_interval(struct glcontext *glcontext, int interval);
void ngpu_glcontext_set_surface_pts(struct glcontext *glcontext, double t);
int ngpu_glcontext_resize(struct glcontext *glcontext, uint32_t width, uint32_t height);
void *ngpu_glcontext_get_proc_address(struct glcontext *glcontext, const char *name);
void *ngpu_glcontext_get_texture_cache(struct glcontext *glcontext);
uintptr_t ngpu_glcontext_get_display(struct glcontext *glcontext);
uintptr_t ngpu_glcontext_get_handle(struct glcontext *glcontext);
GLuint ngpu_glcontext_get_default_framebuffer(struct glcontext *glcontext);
void ngpu_glcontext_freep(struct glcontext **glcontext);
int ngpu_glcontext_check_extension(const char *extension, const char *extensions);
int ngpu_glcontext_check_gl_error(const struct glcontext *glcontext, const char *context);

#endif /* GLCONTEXT_H */
