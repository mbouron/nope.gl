/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <string.h>

#include "gpu_buffer_gl.h"
#include "gpu_ctx_gl.h"
#include "glcontext.h"
#include "glincludes.h"
#include "memory.h"

static GLbitfield get_gl_barriers(uint32_t usage)
{
    GLbitfield barriers = 0;
    if (usage & NGLI_GPU_BUFFER_USAGE_TRANSFER_SRC_BIT)
        barriers |= GL_BUFFER_UPDATE_BARRIER_BIT;
    if (usage & NGLI_GPU_BUFFER_USAGE_TRANSFER_DST_BIT)
        barriers |= GL_BUFFER_UPDATE_BARRIER_BIT;
    if (usage & NGLI_GPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
        barriers |= GL_UNIFORM_BARRIER_BIT;
    if (usage & NGLI_GPU_BUFFER_USAGE_STORAGE_BUFFER_BIT)
        barriers |= GL_SHADER_STORAGE_BARRIER_BIT;
    if (usage & NGLI_GPU_BUFFER_USAGE_INDEX_BUFFER_BIT)
        barriers |= GL_ELEMENT_ARRAY_BARRIER_BIT;
    if (usage & NGLI_GPU_BUFFER_USAGE_VERTEX_BUFFER_BIT)
        barriers |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT;
    if (usage & NGLI_GPU_BUFFER_USAGE_MAP_READ)
        barriers |= GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT;
    if (usage & NGLI_GPU_BUFFER_USAGE_MAP_WRITE)
        barriers |= GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT;
    return barriers;
}

static GLenum get_gl_usage(uint32_t usage)
{
    if (usage & NGLI_GPU_BUFFER_USAGE_DYNAMIC_BIT)
        return GL_DYNAMIC_DRAW;
    return GL_STATIC_DRAW;
}

static GLbitfield get_gl_map_flags(uint32_t usage)
{
    GLbitfield flags = 0;
    if (usage & NGLI_GPU_BUFFER_USAGE_MAP_READ)
        flags |= GL_MAP_READ_BIT;
    if (usage & NGLI_GPU_BUFFER_USAGE_MAP_WRITE)
        flags |= GL_MAP_WRITE_BIT;
    return flags;
}

struct gpu_buffer *ngli_gpu_buffer_gl_create(struct gpu_ctx *gpu_ctx)
{
    struct gpu_buffer_gl *s = ngli_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->parent.gpu_ctx = gpu_ctx;
    return (struct gpu_buffer *)s;
}

int ngli_gpu_buffer_gl_init(struct gpu_buffer *s)
{
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct gpu_buffer_gl *s_priv = (struct gpu_buffer_gl *)s;

    s_priv->map_flags = get_gl_map_flags(s->usage);
    s_priv->barriers = get_gl_barriers(s->usage);

    gl->funcs.GenBuffers(1, &s_priv->id);
    gl->funcs.BindBuffer(GL_ARRAY_BUFFER, s_priv->id);
    if (gl->features & NGLI_FEATURE_GL_BUFFER_STORAGE) {
        const GLbitfield storage_flags = GL_DYNAMIC_STORAGE_BIT;
        gl->funcs.BufferStorage(GL_ARRAY_BUFFER, s->size, NULL, storage_flags | s_priv->map_flags);
    } else {
        gl->funcs.BufferData(GL_ARRAY_BUFFER, s->size, NULL, get_gl_usage(s->usage));
    }

    return 0;
}

int ngli_gpu_buffer_gl_upload(struct gpu_buffer *s, const void *data, size_t offset, size_t size)
{
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct gpu_buffer_gl *s_priv = (struct gpu_buffer_gl *)s;
    gl->funcs.BindBuffer(GL_ARRAY_BUFFER, s_priv->id);
    gl->funcs.BufferSubData(GL_ARRAY_BUFFER, offset, size, data);
    return 0;
}

int ngli_gpu_buffer_gl_map(struct gpu_buffer *s, size_t offset, size_t size, void **datap)
{
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct gpu_buffer_gl *s_priv = (struct gpu_buffer_gl *)s;
    gl->funcs.BindBuffer(GL_ARRAY_BUFFER, s_priv->id);
    void *data = gl->funcs.MapBufferRange(GL_ARRAY_BUFFER, offset, size, s_priv->map_flags);
    if (!data)
        return NGL_ERROR_GRAPHICS_GENERIC;
    *datap = data;
    return 0;
}

void ngli_gpu_buffer_gl_unmap(struct gpu_buffer *s)
{
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct gpu_buffer_gl *s_priv = (struct gpu_buffer_gl *)s;
    gl->funcs.BindBuffer(GL_ARRAY_BUFFER, s_priv->id);
    gl->funcs.UnmapBuffer(GL_ARRAY_BUFFER);
}

void ngli_gpu_buffer_gl_freep(struct gpu_buffer **sp)
{
    if (!*sp)
        return;
    struct gpu_buffer *s = *sp;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct gpu_buffer_gl *s_priv = (struct gpu_buffer_gl *)s;
    gl->funcs.DeleteBuffers(1, &s_priv->id);
    ngli_freep(sp);
}
