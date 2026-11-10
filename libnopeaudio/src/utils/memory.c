/*
 * Copyright 2024-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include "config.h"

#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "utils.h"

void *nau_try_malloc(size_t size)
{
    return malloc(size);
}

void *nau_try_calloc(size_t n, size_t size)
{
    return calloc(n, size);
}

void *nau_try_malloc_aligned(size_t alignment, size_t size)
{
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    /* aligned_alloc() requires size to be a multiple of alignment */
    size_t aligned_size = NAU_ALIGN(size, alignment);
    return aligned_alloc(alignment, aligned_size);
#endif
}

void *nau_try_realloc(void *ptr, size_t n, size_t size)
{
    size_t bytes;
    if (ckd_mul(&bytes, n, size))
        return NULL;
    return realloc(ptr, bytes);
}

_Noreturn void nau_oom(void)
{
    fprintf(stderr, "Out of memory\n");
    abort();
}

void *nau_malloc(size_t size)
{
    void *ptr = nau_try_malloc(size);
    if (!ptr)
        nau_oom();
    return ptr;
}

void *nau_calloc(size_t n, size_t size)
{
    void *ptr = nau_try_calloc(n, size);
    if (!ptr)
        nau_oom();
    return ptr;
}

void *nau_malloc_aligned(size_t alignment, size_t size)
{
    void *ptr = nau_try_malloc_aligned(alignment, size);
    if (!ptr)
        nau_oom();
    return ptr;
}

void *nau_realloc(void *ptr, size_t n, size_t size)
{
    void *new_ptr = nau_try_realloc(ptr, n, size);
    if (!new_ptr)
        nau_oom();
    return new_ptr;
}

void nau_free(void *ptr)
{
    free(ptr);
}

void nau_free_aligned(void *ptr)
{
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

void *nau_try_memdup(const void *src, size_t n)
{
    void *dst = nau_try_malloc(n);
    if (!dst)
        return NULL;
    memcpy(dst, src, n);
    return dst;
}
