/*
 * Copyright 2022 GoPro Inc.
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

#include "internal.h"
#include "log.h"
#include "ngpu/ngpu_opengl.h"

static int gl_ctx_begin(struct ngl_ctx *s)
{
    return ngpu_ctx_gl_make_current(s->gpu_ctx);
}

static void gl_ctx_end(struct ngl_ctx *s)
{
    ngpu_ctx_gl_release_current(s->gpu_ctx);
}

static int gl_configure(struct ngl_ctx *s, const struct ngl_config *config)
{
    int ret = ngli_ctx_configure(s, config);
    if (ret < 0)
        return ret;
    gl_ctx_end(s);
    return 0;
}

static int gl_resize(struct ngl_ctx *s, uint32_t width, uint32_t height)
{
    int ret = gl_ctx_begin(s);
    if (ret < 0)
        return ret;
    ret = ngli_ctx_resize(s, width, height);
    if (ret < 0)
        return ret;
    gl_ctx_end(s);
    return 0;
}

static int gl_get_viewport(struct ngl_ctx *s, int32_t *viewport)
{
    int ret = gl_ctx_begin(s);
    if (ret < 0)
        return ret;
    ret = ngli_ctx_get_viewport(s, viewport);
    gl_ctx_end(s);
    return ret;
}

static int gl_set_capture_buffer(struct ngl_ctx *s, void *capture_buffer)
{
    int ret = gl_ctx_begin(s);
    if (ret < 0)
        return ret;
    ret = ngli_ctx_set_capture_buffer(s, capture_buffer);
    if (ret < 0)
        return ret;
    gl_ctx_end(s);
    return 0;
}

static int gl_set_scene(struct ngl_ctx *s, struct ngl_scene *scene)
{
    int ret = gl_ctx_begin(s);
    if (ret < 0)
        return ret;
    ret = ngli_ctx_set_scene(s, scene);
    gl_ctx_end(s);
    return ret;
}

static int gl_prepare_draw(struct ngl_ctx *s, double t)
{
    int ret = gl_ctx_begin(s);
    if (ret < 0)
        return ret;
    ret = ngli_ctx_prepare_draw(s, t);
    gl_ctx_end(s);
    return ret;
}

static int gl_draw(struct ngl_ctx *s, double t, struct ngpu_fence *wait_fence, struct ngpu_fence **signal_fence)
{
    int ret = gl_ctx_begin(s);
    if (ret < 0)
        return ret;
    ret = ngli_ctx_draw(s, t, wait_fence, signal_fence);
    gl_ctx_end(s);
    return ret;
}

static void gl_reset(struct ngl_ctx *s, int action)
{
    if (s->gpu_ctx)
        ngpu_ctx_gl_make_current(s->gpu_ctx);
    ngli_ctx_reset(s, action);
}

static int gl_dispatch(struct ngl_ctx *s, int (*fn)(struct ngl_ctx *, void *), void *arg)
{
    int ret = gl_ctx_begin(s);
    if (ret < 0)
        return ret;
    ret = fn(s, arg);
    gl_ctx_end(s);
    return ret;
}

const struct api_impl api_gl = {
    .configure           = gl_configure,
    .resize              = gl_resize,
    .get_viewport        = gl_get_viewport,
    .set_capture_buffer  = gl_set_capture_buffer,
    .set_scene           = gl_set_scene,
    .prepare_draw        = gl_prepare_draw,
    .draw                = gl_draw,
    .reset               = gl_reset,
    .dispatch            = gl_dispatch,
};
