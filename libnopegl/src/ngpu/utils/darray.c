/*
 * Copyright 2024-2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include "darray.h"
#include "memory.h"
#include "utils.h"

static int reserve_non_aligned(struct ngpu_darray *darray, size_t capacity)
{
    if (capacity < darray->capacity)
        return 0;

    void *ptr = ngpu_realloc(darray->data, capacity, darray->element_size);
    if (!ptr)
        return NGPU_ERROR_MEMORY;
    darray->data = ptr;
    darray->capacity = capacity;
    return 0;
}

static int reserve_aligned(struct ngpu_darray *darray, size_t capacity)
{
    if (capacity < darray->capacity)
        return 0;

    size_t bytes;
    if (NGPU_CHK_MUL(&bytes, capacity, darray->element_size))
        return NGPU_ERROR_MEMORY;

    void *ptr = ngpu_malloc_aligned(bytes);
    if (!ptr)
        return NGPU_ERROR_MEMORY;
    if (darray->data) {
        memcpy(ptr, darray->data, darray->capacity * darray->element_size);
        ngpu_free_aligned(darray->data);
    }
    darray->data = ptr;
    darray->capacity = capacity;
    return 0;
}

void ngpu_darray_init(struct ngpu_darray *darray, size_t element_size, uint32_t flags)
{
    darray->data = NULL;
    darray->count = 0;
    darray->capacity = 0;
    darray->element_size = element_size;
    darray->user_free_func = NULL;
    darray->user_arg = NULL;
    if (flags & NGPU_DARRAY_FLAG_ALIGNED) {
        /*
         * If this is not true only the first element would be aligned. We
         * unfortunately can't force an aligned element size because the user
         * wouldn't be able to step by the sizeof of the element anymore.
         */
        ngpu_assert(element_size == NGPU_ALIGN(element_size, NGPU_ALIGN_VAL));

        darray->reserve = reserve_aligned;
        darray->release = ngpu_free_aligned;
    } else {
        darray->reserve = reserve_non_aligned;
        darray->release = ngpu_free;
    }
}

void ngpu_darray_set_free_func(struct ngpu_darray *darray, ngpu_user_free_func_type user_free_func, void *user_arg)
{
    ngpu_assert(!darray->count);
    darray->user_free_func = user_free_func;
    darray->user_arg = user_arg;
}

void *ngpu_darray_push(struct ngpu_darray *darray, const void *element)
{
    if (darray->count >= darray->capacity) {
#if HAVE_BUILTIN_OVERFLOW
        size_t new_capacity;
        if (__builtin_mul_overflow(darray->capacity, 2, &new_capacity))
            return NULL;
#else
        /* Also includes the realloc overflow check */
        if (darray->capacity >= 1ULL << (sizeof(darray->capacity)*8 - 2))
            return NULL;
        size_t new_capacity = darray->capacity * 2;
#endif
        int ret = darray->reserve(darray, darray->capacity ? new_capacity : 8);
        if (ret < 0)
            return NULL;
    }
    void *new_element = darray->data + darray->count * darray->element_size;
    darray->count++;
    if (element)
        memcpy(new_element, element, darray->element_size);
    else
        memset(new_element, 0, darray->element_size);
    return new_element;
}

void *ngpu_darray_tail(const struct ngpu_darray *darray)
{
    if (darray->count <= 0)
        return NULL;
    return darray->data + (darray->count - 1) * darray->element_size;
}

void *ngpu_darray_pop(struct ngpu_darray *darray)
{
    void *element = ngpu_darray_tail(darray);
    darray->count = darray->count > 0 ? darray->count - 1 : 0;
    return element;
}

void *ngpu_darray_pop_unsafe(struct ngpu_darray *darray)
{
    darray->count--;
    return darray->data + darray->count * darray->element_size;
}

void *ngpu_darray_get(const struct ngpu_darray *darray, size_t index)
{
    ngpu_assert(index < darray->count);
    return darray->data + index * darray->element_size;
}

static void invalidate_range(struct ngpu_darray *darray, size_t index, size_t count)
{
    if (!darray->user_free_func)
        return;
    const size_t end = index + count;
    ngpu_assert(end <= darray->count);
    for (size_t i = index; i < end; i++) {
        uint8_t *element = darray->data + i * darray->element_size;
        darray->user_free_func(darray->user_arg, element);
    }
}

void ngpu_darray_remove(struct ngpu_darray *darray, size_t index)
{
    ngpu_darray_remove_range(darray, index, 1);
}

void ngpu_darray_remove_range(struct ngpu_darray *darray, size_t index, size_t count)
{
    invalidate_range(darray, index, count);
    const size_t end = index + count;
    ngpu_assert(end <= darray->count);
    uint8_t *dst = darray->data + index * darray->element_size;
    const uint8_t *src = darray->data + end * darray->element_size;
    const size_t n = (darray->count - index - count) * darray->element_size;
    memmove(dst, src, n);
    darray->count -= count;
}

void ngpu_darray_clear(struct ngpu_darray *darray)
{
    invalidate_range(darray, 0, darray->count);
    darray->count = 0;
}

void ngpu_darray_reset(struct ngpu_darray *darray)
{
    invalidate_range(darray, 0, darray->count);
    if (darray->release)
        darray->release(darray->data);
    memset(darray, 0, sizeof(*darray));
}
