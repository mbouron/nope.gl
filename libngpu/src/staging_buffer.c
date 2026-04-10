/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <ngpu/ngpu.h>

#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

#define INITIAL_CAPACITY 65536U

struct ngpu_staging_buffer {
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_buffer *buffer;
    uint8_t *mapped_data;
    size_t offset;
    size_t capacity;
    struct ngpu_darray prev_buffers;
    size_t alignment;
    bool persistent;
};

static int create_buffer(struct ngpu_staging_buffer *s, size_t capacity)
{
    s->buffer = ngpu_buffer_create(s->gpu_ctx);
    if (!s->buffer)
        return NGPU_ERROR_MEMORY;

    uint32_t usage = NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                   | NGPU_BUFFER_USAGE_STORAGE_BUFFER_BIT
                   | NGPU_BUFFER_USAGE_DYNAMIC_BIT
                   | NGPU_BUFFER_USAGE_TRANSFER_DST_BIT
                   | NGPU_BUFFER_USAGE_MAP_WRITE;

    if (s->persistent)
        usage |= NGPU_BUFFER_USAGE_MAP_PERSISTENT;

    int ret = ngpu_buffer_init(s->buffer, capacity, usage);
    if (ret < 0)
        return ret;

    ret = ngpu_buffer_map(s->buffer, 0, NGPU_BUFFER_WHOLE_SIZE, (void **)&s->mapped_data);
    if (ret < 0)
        return ret;

    return 0;
}

struct ngpu_staging_buffer *ngpu_staging_buffer_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngpu_staging_buffer *s = ngpu_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->gpu_ctx = gpu_ctx;

    const struct ngpu_limits *limits = ngpu_ctx_get_limits(gpu_ctx);
    s->alignment = NGPU_MAX(limits->min_uniform_block_offset_alignment,
                            limits->min_storage_block_offset_alignment);
    if (!s->alignment)
        s->alignment = 256; /* safe default */

    const uint64_t features = ngpu_ctx_get_features(gpu_ctx);
    s->persistent = (features & NGPU_FEATURE_BUFFER_MAP_PERSISTENT_BIT) != 0;

    s->offset = 0;
    s->capacity = NGPU_ALIGN(INITIAL_CAPACITY, s->alignment);

    int ret = create_buffer(s, s->capacity);
    if (ret < 0) {
        ngpu_freep(&s);
        return NULL;
    }

    ngpu_darray_init(&s->prev_buffers, sizeof(struct ngpu_buffer *), 0);

    return s;
}

static int grow(struct ngpu_staging_buffer *s, size_t min_capacity)
{
    size_t new_capacity = s->capacity;
    while (new_capacity < min_capacity)
        new_capacity *= 2;
    new_capacity = NGPU_ALIGN(new_capacity, s->alignment);

    /*
     * Keep the old buffer alive so that buffer pointers returned by
     * get_buffer() earlier remain valid until next reset().
     */
    ngpu_buffer_unmap(s->buffer);
    if (!ngpu_darray_push(&s->prev_buffers, &s->buffer))
        return NGPU_ERROR_MEMORY;
    s->buffer = NULL;
    s->mapped_data = NULL;

    int ret = create_buffer(s, new_capacity);
    if (ret < 0)
        return ret;

    s->capacity = new_capacity;
    s->offset = 0;

    return 0;
}

void *ngpu_staging_buffer_reserve(struct ngpu_staging_buffer *s, size_t size, size_t *offsetp)
{
    size_t aligned_offset = NGPU_ALIGN(s->offset, s->alignment);
    size_t end = aligned_offset + size;

    if (end > s->capacity) {
        int ret = grow(s, end);
        ngpu_assert(ret == 0);
        aligned_offset = NGPU_ALIGN(s->offset, s->alignment);
        end = aligned_offset + size;
    }

    s->offset = end;

    *offsetp = aligned_offset;
    return s->mapped_data + aligned_offset;
}

size_t ngpu_staging_buffer_push(struct ngpu_staging_buffer *s, const void *data, size_t size)
{
    size_t offset = 0;
    void *dst = ngpu_staging_buffer_reserve(s, size, &offset);
    memcpy(dst, data, size);
    return offset;
}

int ngpu_staging_buffer_flush(struct ngpu_staging_buffer *s)
{
    if (s->persistent)
        return 0;

    if (s->mapped_data) {
        ngpu_buffer_unmap(s->buffer);
        s->mapped_data = NULL;
    }

    return 0;
}

static void ngpu_staging_buffer_free_prev_buffers(struct ngpu_staging_buffer *s)
{
    struct ngpu_buffer **prev = ngpu_darray_data(&s->prev_buffers);
    for (size_t i = 0; i < ngpu_darray_count(&s->prev_buffers); i++) {
        ngpu_buffer_wait(prev[i]);
        ngpu_buffer_freep(&prev[i]);
    }
    ngpu_darray_clear(&s->prev_buffers);
}

void ngpu_staging_buffer_reset(struct ngpu_staging_buffer *s)
{
    ngpu_staging_buffer_free_prev_buffers(s);
    ngpu_buffer_wait(s->buffer);

    s->offset = 0;

    if (!s->persistent && !s->mapped_data)
        ngpu_buffer_map(s->buffer, 0, NGPU_BUFFER_WHOLE_SIZE, (void **)&s->mapped_data);
}

struct ngpu_buffer *ngpu_staging_buffer_get_buffer(const struct ngpu_staging_buffer *s)
{
    return s->buffer;
}

void ngpu_staging_buffer_freep(struct ngpu_staging_buffer **sp)
{
    struct ngpu_staging_buffer *s = *sp;
    if (!s)
        return;

    ngpu_staging_buffer_free_prev_buffers(s);
    ngpu_buffer_wait(s->buffer);

    if (s->mapped_data) {
        ngpu_buffer_unmap(s->buffer);
        s->mapped_data = NULL;
    }
    ngpu_buffer_freep(&s->buffer);
    ngpu_darray_reset(&s->prev_buffers);

    ngpu_freep(sp);
}
