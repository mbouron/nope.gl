/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <string.h>

#include "gpu_ctx.h"
#include "log.h"
#include "memory.h"
#include "pipeline.h"
#include "buffer.h"
#include "type.h"
#include "utils.h"

int ngli_pipeline_layout_copy(struct pipeline_layout *dst, const struct pipeline_layout *src)
{
    dst->texture_descs = ngli_memdup(src->texture_descs, src->nb_texture_descs * sizeof(*src->texture_descs));
    if (!dst->texture_descs)
        return NGL_ERROR_MEMORY;
    dst->nb_texture_descs = src->nb_texture_descs;

    dst->buffer_descs = ngli_memdup(src->buffer_descs, src->nb_buffer_descs * sizeof(*src->buffer_descs));
    if (!dst->buffer_descs)
        return NGL_ERROR_MEMORY;
    dst->nb_buffer_descs = src->nb_buffer_descs;

    if (src->nb_attribute_descs) {
    dst->attribute_descs = ngli_memdup(src->attribute_descs, src->nb_attribute_descs * sizeof(*src->attribute_descs));
    if (!dst->attribute_descs)
        return NGL_ERROR_MEMORY;
    dst->nb_attribute_descs = src->nb_attribute_descs;
        }

    return 0;
}

void ngli_pipeline_layout_reset(struct pipeline_layout *layout)
{
    ngli_freep(&layout->texture_descs);
    ngli_freep(&layout->buffer_descs);
    ngli_freep(&layout->attribute_descs);
    memset(layout, 0, sizeof(*layout));
}

struct pipeline *ngli_pipeline_create(struct gpu_ctx *gpu_ctx)
{
    return gpu_ctx->cls->pipeline_create(gpu_ctx);
}

int ngli_pipeline_init(struct pipeline *s, const struct pipeline_params *params)
{
    return s->gpu_ctx->cls->pipeline_init(s, params);
}

void ngli_pipeline_freep(struct pipeline **sp)
{
    if (!*sp)
        return;
    (*sp)->gpu_ctx->cls->pipeline_freep(sp);
}
