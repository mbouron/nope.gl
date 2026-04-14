/*
 * Copyright 2025-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include "utils/log.h"
#include "opengl/ctx_gl.h"
#include "opengl/fence_gl.h"
#include "utils/memory.h"

static void fence_gl_release(void **sp)
{
    struct ngpu_fence *s = *sp;
    if (!s)
        return;

    ngpu_fence_reset(s);

    ngpu_freep(sp);
}

struct ngpu_fence *ngpu_fence_gl_create(struct ngpu_ctx *ctx)
{
    struct ngpu_fence_gl *s = ngpu_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->parent.rc = NGPU_RC_CREATE(fence_gl_release);
    s->parent.gpu_ctx = ctx;

    return (struct ngpu_fence *)s;
}

int ngpu_fence_gl_reset(struct ngpu_fence *s)
{
    struct ngpu_fence_gl *s_priv = (struct ngpu_fence_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    if (s_priv->fence) {
        gl->funcs.DeleteSync(s_priv->fence);
        s_priv->fence = 0;
    }

    return 0;
}

int ngpu_fence_gl_insert(struct ngpu_fence *s)
{
    struct ngpu_fence_gl *s_priv = (struct ngpu_fence_gl *)s;
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    ngpu_assert(s_priv->fence == 0);
    s_priv->fence = gl->funcs.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    return 0;
}

int ngpu_fence_gl_wait(struct ngpu_fence *s)
{
    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct ngpu_fence_gl *s_priv = (struct ngpu_fence_gl *)s;

    if (s_priv->fence == 0)
        return 0;

    GLenum ret = gl->funcs.ClientWaitSync(s_priv->fence, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX);
    if (ret == GL_TIMEOUT_EXPIRED) {
        LOG(ERROR, "fence timeout expired");
        return NGPU_ERROR_GRAPHICS_GENERIC;
    } else if (ret == GL_WAIT_FAILED) {
        LOG(ERROR, "fence wait failed");
        return NGPU_ERROR_GRAPHICS_GENERIC;
    }

    return 0;
}

int ngpu_fence_gl_is_signaled(struct ngpu_fence *s)
{
    struct ngpu_fence_gl *s_priv = (struct ngpu_fence_gl *)s;

    if (s_priv->fence == 0)
        return 0;

    struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    GLenum ret = gl->funcs.ClientWaitSync(s_priv->fence, 0, 0);
    return ret == GL_ALREADY_SIGNALED || ret == GL_CONDITION_SATISFIED;
}

void ngpu_fence_gl_freep(struct ngpu_fence **sp) { NGPU_RC_UNREFP(sp); }
