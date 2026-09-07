/*
 * Copyright 2023-2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef NGPU_OPENGL_H
#define NGPU_OPENGL_H

#include <ngpu/ngpu.h>

struct ngpu_ctx_params_gl {
    /*
     * Native OpenGL context handle (EGLContext, HGLRC, NSOpenGLContext, or
     * EAGLContext, depending on the platform), or zero.
     *
     * If nonzero, the newly created context shares all shareable data, as
     * defined by the client API, with the specified context and the contexts
     * in its share group. The specified context must be valid and must have
     * been created on the same display and for the same client API (OpenGL or
     * OpenGL ES) as the newly created context.
     *
     * The application must synchronize access to shared objects. See
     * ngpu_texture_gl_get_name() and ngpu_fence_gl_get_sync().
     *
     * If shared_context is nonzero, ngpu_ctx_params.shared_ctx must be NULL.
     * Use ngpu_ctx_params.shared_ctx to share with another ngpu_ctx.
     */
    uintptr_t shared_context;
};

NGPU_API int ngpu_ctx_gl_make_current(struct ngpu_ctx *s);
NGPU_API int ngpu_ctx_gl_release_current(struct ngpu_ctx *s);

NGPU_API uint32_t ngpu_texture_gl_get_name(const struct ngpu_texture *s);
NGPU_API uint32_t ngpu_texture_gl_get_target(const struct ngpu_texture *s);

NGPU_API void *ngpu_fence_gl_get_sync(const struct ngpu_fence *s);
NGPU_API struct ngpu_fence *ngpu_fence_gl_create_from_sync(struct ngpu_ctx *ctx, void *sync);

#endif
