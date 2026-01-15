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
     * Whether the OpenGL context is external or not. If the OpenGL context is
     * external, it is the user responsibility to manage the OpenGL context and
     * make sure it is current before calling any of the ngl_* functions.
     */
    int external;
    /*
     * External OpenGL framebuffer used for rendering. The framebuffer must
     * have a color attachment composed of 4 color components (R, G, B, A) and
     * a combined depth and stencil buffer attached to it.
     */
    uint32_t external_framebuffer;
};

NGPU_API int ngpu_ctx_gl_make_current(struct ngpu_ctx *s);
NGPU_API int ngpu_ctx_gl_release_current(struct ngpu_ctx *s);
NGPU_API void ngpu_ctx_gl_reset_state(struct ngpu_ctx *s);
NGPU_API int ngpu_ctx_gl_wrap_framebuffer(struct ngpu_ctx *s, uint32_t fbo);

#endif
