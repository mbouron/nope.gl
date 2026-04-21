/*
 * Copyright 2026 Matthieu Bouron <matthieu@mojo.video>
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

#include <stddef.h>
#include <string.h>

#include "darray.h"
#include "memory.h"
#include "nopegl/nopegl.h"

#define NGLI_DARRAY_INITIAL_CAPACITY 8

static bool needs_aligned(size_t alignment)
{
#ifdef TARGET_MSVC
    return alignment > _Alignof(double);
#else
    return alignment > _Alignof(max_align_t);
#endif
}

static int reserve(ngli_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment, size_t new_capacity)
{
    if (new_capacity <= *capacity)
        return 0;

    if (needs_aligned(alignment)) {
        size_t size;
        if (NGLI_CHK_MUL(&size, new_capacity, element_size))
            return NGL_ERROR_MEMORY;
        void *ptr = ngli_malloc_aligned(alignment, size);
        if (!ptr)
            return NGL_ERROR_MEMORY;
        if (*data) {
            memcpy(ptr, *data, *capacity * element_size);
            ngli_free_aligned(*data);
        }
        *data = ptr;
    } else {
        void *ptr = ngli_realloc(*data, new_capacity, element_size);
        if (!ptr)
            return NGL_ERROR_MEMORY;
        *data = ptr;
    }

    *capacity = new_capacity;
    return 0;
}

int ngli_darray_grow_(ngli_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment)
{
    const size_t new_capacity = *capacity ? *capacity * 2 : NGLI_DARRAY_INITIAL_CAPACITY;
    return reserve(data, capacity, element_size, alignment, new_capacity);
}

int ngli_darray_reserve_(ngli_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment, size_t new_capacity)
{
    return reserve(data, capacity, element_size, alignment, new_capacity);
}

void ngli_darray_free_(void *ptr, size_t alignment)
{
    if (needs_aligned(alignment))
        ngli_free_aligned(ptr);
    else
        ngli_free(ptr);
}
