/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <stdckdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "darray.h"
#include "error.h"
#include "memory.h"

#define NPRIV_DARRAY_INITIAL_CAPACITY 8

static bool needs_aligned(size_t alignment)
{
    return alignment > _Alignof(max_align_t);
}

static int reserve(npriv_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment, size_t new_capacity)
{
    if (new_capacity <= *capacity)
        return 0;

    if (needs_aligned(alignment)) {
        size_t size;
        if (ckd_mul(&size, new_capacity, element_size))
            return NPRIV_ERROR_MEMORY;
        void *ptr = npriv_try_malloc_aligned(alignment, size);
        if (!ptr)
            return NPRIV_ERROR_MEMORY;
        if (*data) {
            memcpy(ptr, *data, *capacity * element_size);
            npriv_free_aligned(*data);
        }
        *data = ptr;
    } else {
        void *ptr = npriv_try_realloc(*data, new_capacity, element_size);
        if (!ptr)
            return NPRIV_ERROR_MEMORY;
        *data = ptr;
    }

    *capacity = new_capacity;
    return 0;
}

int npriv_darray_try_grow_(npriv_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment)
{
    const size_t new_capacity = *capacity ? *capacity * 2 : NPRIV_DARRAY_INITIAL_CAPACITY;
    return reserve(data, capacity, element_size, alignment, new_capacity);
}

int npriv_darray_try_reserve_(npriv_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment, size_t new_capacity)
{
    return reserve(data, capacity, element_size, alignment, new_capacity);
}

void npriv_darray_grow_(npriv_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment)
{
    if (npriv_darray_try_grow_(data, capacity, element_size, alignment) < 0)
        npriv_oom();
}

void npriv_darray_reserve_(npriv_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment, size_t new_capacity)
{
    if (npriv_darray_try_reserve_(data, capacity, element_size, alignment, new_capacity) < 0)
        npriv_oom();
}

void npriv_darray_free_(void *ptr, size_t alignment)
{
    if (needs_aligned(alignment))
        npriv_free_aligned(ptr);
    else
        npriv_free(ptr);
}
