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

#include "ctx.h"
#include "texture.h"
#include "utils/bits.h"
#include "utils/utils.h"

static void texture_freep(void **texturep)
{
    struct ngpu_texture **sp = (struct ngpu_texture **)texturep;
    if (!*sp)
        return;

    struct ngpu_texture *s = *sp;
    if (s->memory_size_accounted) {
        struct ngpu_memory_stats *stats = &s->gpu_ctx->memory_stats;
        stats->texture_count -= NGPU_MIN(stats->texture_count, 1);
        stats->texture_bytes -= NGPU_MIN(stats->texture_bytes, s->memory_size);
    }

    s->gpu_ctx->cls->texture_freep(sp);
}

struct ngpu_texture *ngpu_texture_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngpu_texture *s = gpu_ctx->cls->texture_create(gpu_ctx);
    if (!s)
        return NULL;
    s->rc = NGPU_RC_CREATE(texture_freep);
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

static uint64_t get_memory_size(const struct ngpu_texture_params *params)
{
    uint32_t depth = params->type == NGPU_TEXTURE_TYPE_3D ? params->depth : 1;
    uint32_t layers = 1;
    if (params->type == NGPU_TEXTURE_TYPE_2D_ARRAY)
        layers = params->depth;
    else if (params->type == NGPU_TEXTURE_TYPE_CUBE)
        layers = 6;

    const uint32_t samples = params->samples ? params->samples : 1;
    const uint64_t bytes_per_pixel = ngpu_format_get_bytes_per_pixel(params->format);

    uint32_t levels = 1;
    if (params->mipmap_filter != NGPU_MIPMAP_FILTER_NONE)
        levels = ngpu_log2(params->width | params->height | 1);

    uint32_t width = params->width;
    uint32_t height = params->height;
    uint64_t size = 0;
    for (uint32_t i = 0; i < levels; i++) {
        size += (uint64_t)width
              * (uint64_t)height
              * (uint64_t)depth
              * (uint64_t)layers
              * (uint64_t)samples
              * bytes_per_pixel;

        width = NGPU_MAX(width >> 1, 1);
        height = NGPU_MAX(height >> 1, 1);
        if (params->type == NGPU_TEXTURE_TYPE_3D)
            depth = NGPU_MAX(depth >> 1, 1);
    }
    return size;
}

int ngpu_texture_init(struct ngpu_texture *s, const struct ngpu_texture_params *params)
{
    const enum ngpu_import_type import_type = params->import_params.type;
    if (import_type == NGPU_IMPORT_TYPE_NONE) {
        ngpu_assert(NGPU_HAS_ALL_FLAGS(s->gpu_ctx->features, import_feature_map[import_type]));
        int ret = s->gpu_ctx->cls->texture_init(s, params);
        if (ret < 0)
            return ret;

        s->memory_size = get_memory_size(params);
        struct ngpu_memory_stats *stats = &s->gpu_ctx->memory_stats;
        stats->texture_count++;
        stats->texture_bytes += s->memory_size;
        s->memory_size_accounted = true;

        return 0;
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

int ngpu_texture_read_pixels(struct ngpu_texture *s, uint8_t *data)
{
    ngpu_assert(s->params.type == NGPU_TEXTURE_TYPE_2D);
    return s->gpu_ctx->cls->texture_read_pixels(s, data);
}

int ngpu_texture_generate_mipmap(struct ngpu_texture *s)
{
    return s->gpu_ctx->cls->texture_generate_mipmap(s);
}

struct ngpu_texture *ngpu_texture_ref(struct ngpu_texture *s)
{
    return NGPU_RC_REF(s);
}

void ngpu_texture_unrefp(struct ngpu_texture **sp)
{
    NGPU_RC_UNREFP(sp);
}

void ngpu_texture_freep(struct ngpu_texture **sp)
{
    NGPU_RC_UNREFP(sp);
}

const struct ngpu_texture_params *ngpu_texture_get_params(const struct ngpu_texture *s)
{
    return &s->params;
}
