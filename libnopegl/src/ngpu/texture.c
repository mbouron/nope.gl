/*
 * Copyright 2023-2024 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include "ngpu/ctx.h"
#include "ngpu/texture.h"

static void texture_freep(void **texturep)
{
    struct ngpu_texture **sp = (struct ngpu_texture **)texturep;
    if (!*sp)
        return;

    (*sp)->gpu_ctx->cls->texture_freep(sp);
}

struct ngpu_texture *ngpu_texture_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngpu_texture *s = gpu_ctx->cls->texture_create(gpu_ctx);
    if (!s)
        return NULL;
    s->rc = NGLI_RC_CREATE(texture_freep);
    return s;
}

static const uint64_t import_feature_map[] = {
    [NGPU_IMPORT_TYPE_NONE]             = 0,
    [NGPU_IMPORT_TYPE_DMA_BUF]          = NGPU_FEATURE_IMPORT_DMA_BUF_BIT,
    [NGPU_IMPORT_TYPE_AHARDWARE_BUFFER] = NGPU_FEATURE_IMPORT_AHARDWARE_BUFFER_BIT,
    [NGPU_IMPORT_TYPE_IOSURFACE]        = NGPU_FEATURE_IMPORT_IOSURFACE_BIT,
    [NGPU_IMPORT_TYPE_COREVIDEO_BUFFER] = NGPU_FEATURE_IMPORT_COREVIDEO_BUFFER_BIT,
    [NGPU_IMPORT_TYPE_METAL_TEXTURE]    = NGPU_FEATURE_IMPORT_METAL_TEXTURE_BIT,
};

int ngpu_texture_init(struct ngpu_texture *s, const struct ngpu_texture_params *params)
{
    const enum ngpu_import_type import_type = params->import_params.type;
    if (import_type == NGPU_IMPORT_TYPE_NONE) {
        ngli_assert(NGLI_HAS_ALL_FLAGS(s->gpu_ctx->features, import_feature_map[import_type]));
        return s->gpu_ctx->cls->texture_init(s, params);
    }

    return s->gpu_ctx->cls->texture_import(s, params);
}

int ngpu_texture_upload(struct ngpu_texture *s, const uint8_t *data, uint32_t linesize)
{
    return s->gpu_ctx->cls->texture_upload(s, data, linesize);
}

int ngpu_texture_upload_with_params(struct ngpu_texture *s, const uint8_t *data, const struct ngpu_texture_transfer_params *transfer_params)
{
    return s->gpu_ctx->cls->texture_upload_with_params(s, data, transfer_params);
}

int ngpu_texture_generate_mipmap(struct ngpu_texture *s)
{
    return s->gpu_ctx->cls->texture_generate_mipmap(s);
}

void ngpu_texture_freep(struct ngpu_texture **sp)
{
    NGLI_RC_UNREFP(sp);
}

const struct ngpu_texture_params *ngpu_texture_get_params(const struct ngpu_texture *s)
{
    return &s->params;
}
