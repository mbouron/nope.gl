/*
 * Copyright 2026 Matthieu Bouron <matthieu@gmail.com>
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

#ifndef NGPU_DARRAY_H
#define NGPU_DARRAY_H

#include <stddef.h>
#include <string.h>

#include "utils.h"

typedef void *ngpu_may_alias ngpu_darray_data_alias;

int ngpu_darray_grow_(ngpu_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment);
int ngpu_darray_reserve_(ngpu_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment, size_t new_capacity);
void ngpu_darray_free_(void *ptr, size_t alignment);

#define NGPU_DARRAY_ALIGNOF_(a) _Alignof(__typeof__(*(a)->data))

#define NGPU_DARRAY(T) struct {                                                  \
    T *data;                                                                     \
    size_t count;                                                                \
    size_t capacity;                                                             \
    ngpu_user_free_func_type user_free_func;                                     \
    void *user_arg;                                                              \
}

#define NGPU_DECLARE_DARRAY_WITH_NAME(name, T) struct name {                     \
    T *data;                                                                     \
    size_t count;                                                                \
    size_t capacity;                                                             \
    ngpu_user_free_func_type user_free_func;                                     \
    void *user_arg;                                                              \
}

#define ngpu_darray_is_empty(a) ((a)->count == 0)

#define ngpu_darray_get(a, i)                                                    \
    (ngpu_assert((size_t)(i) < (a)->count), &(a)->data[(i)])

#define ngpu_darray_tail(a)                                                      \
    (ngpu_assert((a)->count > 0), &(a)->data[(a)->count - 1])

#define ngpu_darray_foreach(it, a)                                               \
    for (__typeof__(*(a)->data) *it = (a)->data;                                 \
         it < (a)->data + (a)->count; it++)

#define ngpu_darray_push(a, ...)                                                 \
    (((a)->count < (a)->capacity                                                 \
      || ngpu_darray_grow_((ngpu_darray_data_alias *)&(a)->data, &(a)->capacity, \
                           sizeof(*(a)->data), NGPU_DARRAY_ALIGNOF_(a)) == 0)    \
      ? ((a)->data[(a)->count++] = (__VA_ARGS__), 0)                             \
      : NGPU_ERROR_MEMORY)

#define ngpu_darray_pop(a) \
    (ngpu_assert((a)->count > 0), &(a)->data[--(a)->count])

#define ngpu_darray_pop_unsafe(a) \
    (&(a)->data[--(a)->count])

#define ngpu_darray_reserve(a, cap)                                              \
    ngpu_darray_reserve_((ngpu_darray_data_alias *)&(a)->data, &(a)->capacity,   \
                         sizeof(*(a)->data), NGPU_DARRAY_ALIGNOF_(a), (cap))

#define ngpu_darray_clear(a) do {                                                \
    if ((a)->user_free_func)                                                     \
        for (size_t _i = 0; _i < (a)->count; _i++)                               \
            (a)->user_free_func((a)->user_arg, &(a)->data[_i]);                  \
    (a)->count = 0;                                                              \
} while (0)

#define ngpu_darray_remove_range(a, idx, n) do {                                 \
    size_t _idx = (idx);                                                         \
    size_t _n = (n);                                                             \
    ngpu_assert(_idx + _n <= (a)->count);                                        \
    if ((a)->user_free_func)                                                     \
        for (size_t _i = 0; _i < _n; _i++)                                       \
            (a)->user_free_func((a)->user_arg, &(a)->data[_idx + _i]);           \
    memmove(&(a)->data[_idx], &(a)->data[_idx + _n],                             \
            ((a)->count - _idx - _n) * sizeof(*(a)->data));                      \
    (a)->count -= _n;                                                            \
} while (0)

#define ngpu_darray_remove(a, idx) ngpu_darray_remove_range((a), (idx), 1)

#define ngpu_darray_reset(a) do {                                                \
    ngpu_darray_clear(a);                                                        \
    ngpu_darray_free_((a)->data, NGPU_DARRAY_ALIGNOF_(a));                       \
    (a)->data = NULL;                                                            \
    (a)->capacity = 0;                                                           \
} while (0)

#define ngpu_darray_set_free_func(a, fn, arg) do {                               \
    ngpu_assert((a)->count == 0);                                                \
    (a)->user_free_func = (fn);                                                  \
    (a)->user_arg = (arg);                                                       \
} while (0)

#endif
