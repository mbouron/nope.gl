/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include "bind_group.h"
#include "gpu_ctx.h"

struct bindgroup_layout *ngli_bindgroup_layout_create(struct gpu_ctx *gpu_ctx)
{
    return gpu_ctx->cls->bindgroup_layout_create(gpu_ctx);
}

int ngli_bindgroup_layout_init(struct bindgroup_layout *s, const struct bindgroup_layout_params *params)
{
    return s->gpu_ctx->cls->bindgroup_layout_init(s, params);
}

void ngli_bindgroup_layout_freep(struct bindgroup_layout **sp)
{
    if (!*sp)
        return;
    (*sp)->gpu_ctx->cls->bindgroup_layout_freep(sp);
}

struct bindgroup *ngli_bindgroup_create(struct gpu_ctx *gpu_ctx)
{
    return gpu_ctx->cls->bindgroup_create(gpu_ctx);
}

int ngli_bindgroup_init(struct bindgroup *s, const struct bindgroup_params *params)
{
    return s->gpu_ctx->cls->bindgroup_init(s, params);
}

int ngli_bindgroup_set_texture(struct bindgroup *s, int32_t index, const struct texture *texture)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    return s->gpu_ctx->cls->bindgroup_set_texture(s, index, texture);
}

int ngli_bindgroup_set_buffer(struct bindgroup *s, int32_t index, const struct buffer *buffer, size_t offset, size_t size)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;
    return s->gpu_ctx->cls->bindgroup_set_buffer(s, index, buffer, offset, size);
}

void ngli_bindgroup_freep(struct bindgroup **sp)
{
    if (!*sp)
        return;
    (*sp)->gpu_ctx->cls->bindgroup_freep(sp);
}
