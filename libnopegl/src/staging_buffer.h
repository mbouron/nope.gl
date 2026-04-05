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

#ifndef STAGING_BUFFER_H
#define STAGING_BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "utils/darray.h"

struct ngpu_buffer;
struct ngpu_ctx;

/*
 * Staging buffer for per-frame uniform data.
 *
 * All draw nodes push their uniform block data to this buffer during the draw
 * phase, getting back an offset for binding. The buffer is reset each frame.
 */
struct staging_buffer {
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_buffer *buffer;      /* GPU buffer */
    uint8_t *mapped_data;            /* Mapped GPU memory */
    size_t offset;                   /* Current buffer position */
    size_t capacity;                 /* Current buffer capacity */
    struct darray prev_buffers;      /* struct ngpu_buffer *: old GPU buffers kept alive until reset */
    size_t alignment;                /* UBO offset alignment */
    bool persistent;                 /* Whether persistent mapping is used */
};

int ngli_staging_buffer_init(struct staging_buffer *s, struct ngpu_ctx *gpu_ctx);

/*
 * Reserve space in the staging buffer.
 *
 * Return a writable pointer to the reserved area and its offset within the buffer.
 */
void *ngli_staging_buffer_reserve(struct staging_buffer *s, size_t size, size_t *offsetp);

/*
 * Push data to the staging buffer.
 *
 * Returns the aligned offset within the buffer.
 */
size_t ngli_staging_buffer_push(struct staging_buffer *s, const void *data, size_t size);

/*
 * Flush all staged data to the GPU buffer.
 *
 * Must be called before any draw that references the staging buffer.
 */
int ngli_staging_buffer_flush(struct staging_buffer *s);

/*
 * Reset the staging buffer.
 */
void ngli_staging_buffer_reset(struct staging_buffer *s);

/*
 * Get the GPU buffer handle.
 */
struct ngpu_buffer *ngli_staging_buffer_get_buffer(const struct staging_buffer *s);

/*
 * Free the staging buffer resources.
 */
void ngli_staging_buffer_freep(struct staging_buffer *s);

#endif
