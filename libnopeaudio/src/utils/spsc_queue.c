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

#include "spsc_queue.h"
#include "libnopeutil/memory.h"
#include "nopeaudio/nopeaudio.h"

int nau_spsc_queue_init_(struct spsc_queue *q, nau_spsc_queue_data_alias *data,
                         size_t capacity, size_t element_size, size_t alignment)
{
    if (*data)
        return NAU_ERROR_INVALID;

    if (!capacity)
        return NAU_ERROR_INVALID;

    if (!element_size)
        return NAU_ERROR_INVALID;

    size_t size = 1;
    while (size < capacity) {
        if (size > SIZE_MAX / 2)
            return NAU_ERROR_UNSUPPORTED;
        size <<= 1;
    }
    if (size > SIZE_MAX / element_size)
        return NAU_ERROR_UNSUPPORTED;

    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    if (!atomic_is_lock_free(&q->head) || !atomic_is_lock_free(&q->tail))
        return NAU_ERROR_UNSUPPORTED;

    const size_t bytes = size * element_size;
    void *buffer = alignment > _Alignof(max_align_t)
                 ? npriv_try_malloc_aligned(alignment, bytes) : npriv_try_malloc(bytes);
    if (!buffer)
        return NAU_ERROR_MEMORY;
    q->mask = size - 1;
    *data = buffer;
    return NAU_OK;
}

void nau_spsc_queue_uninit_(struct spsc_queue *q, nau_spsc_queue_data_alias *data, size_t alignment)
{
    if (alignment > _Alignof(max_align_t))
        npriv_free_aligned(*data);
    else
        npriv_free(*data);
    *data = NULL;
    q->mask = 0;
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
}
