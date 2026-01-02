/*
 * Copyright 2023-2024 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023 Nope Forge
 * Copyright 2019-2022 GoPro Inc.
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

#ifndef NGPU_CTX_GL_H
#define NGPU_CTX_GL_H

#include "config.h"

#if defined(TARGET_IPHONE)
#include <CoreVideo/CoreVideo.h>
#endif

#include "ngpu/ctx.h"
#include "ngpu/opengl/cmd_buffer_gl.h"
#include "ngpu/opengl/glstate.h"

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

struct ngl_ctx;
struct ngpu_rendertarget;

typedef void (*capture_func_type)(struct ngpu_ctx *s);

struct ngpu_ctx_gl {
    struct ngpu_ctx parent;
    struct glcontext *glcontext;

    struct ngpu_glstate glstate;

    struct ngpu_cmd_buffer_gl **update_cmd_buffers;
    struct ngpu_cmd_buffer_gl **draw_cmd_buffers;
    struct ngpu_cmd_buffer_gl *cur_cmd_buffer;

    /* Default rendertargets */
    struct ngpu_rendertarget_layout default_rt_layout;
    struct ngpu_rendertarget *default_rt;
    struct ngpu_rendertarget *default_rt_load;

    /* Offscreen render target resources */
    struct ngpu_texture *color;
    struct ngpu_texture *ms_color;
    struct ngpu_texture *depth_stencil;

    /* Offscreen capture callback and resources */
    capture_func_type capture_func;
    struct ngpu_rendertarget *capture_rt;
    struct ngpu_texture *capture_texture;
#if defined(TARGET_IPHONE)
    CVPixelBufferRef capture_cvbuffer;
    CVOpenGLESTextureRef capture_cvtexture;
#endif

    /* Timer */
    GLuint queries[2];
};

int ngpu_ctx_gl_make_current(struct ngpu_ctx *s);
int ngpu_ctx_gl_release_current(struct ngpu_ctx *s);
void ngpu_ctx_gl_reset_state(struct ngpu_ctx *s);
int ngpu_ctx_gl_wrap_framebuffer(struct ngpu_ctx *s, GLuint fbo);

#endif
