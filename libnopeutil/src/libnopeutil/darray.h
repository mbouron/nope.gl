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

#ifndef NPRIV_DARRAY_H
#define NPRIV_DARRAY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "utils.h"

typedef void *npriv_may_alias npriv_darray_data_alias;

int npriv_darray_try_grow_(npriv_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment);
int npriv_darray_try_reserve_(npriv_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment, size_t new_capacity);
void npriv_darray_grow_(npriv_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment);
void npriv_darray_reserve_(npriv_darray_data_alias *data, size_t *capacity, size_t element_size, size_t alignment, size_t new_capacity);
void npriv_darray_free_(void *ptr, size_t alignment);

#define NPRIV_DARRAY_ALIGNOF_(a) _Alignof(__typeof__(*(a)->data))

#define NPRIV_DARRAY(T) struct {                                                 \
    T *data;                                                                     \
    size_t count;                                                                \
    size_t capacity;                                                             \
    npriv_user_free_func_type user_free_func;                                    \
    void *user_arg;                                                              \
}

#define NPRIV_DECLARE_DARRAY_WITH_NAME(name, T) struct name {                    \
    T *data;                                                                     \
    size_t count;                                                                \
    size_t capacity;                                                             \
    npriv_user_free_func_type user_free_func;                                    \
    void *user_arg;                                                              \
}

#define NPRIV_DEFINE_DARRAY_FIND(name)                                          \
static inline size_t name##_find(                                               \
    const struct name *array, __typeof__(((struct name *)0)->data[0]) value)    \
{                                                                               \
    for (size_t i = 0; i < array->count; i++)                                   \
        if (array->data[i] == value)                                            \
            return i;                                                           \
    return SIZE_MAX;                                                            \
}

#define npriv_darray_is_empty(a) ((a)->count == 0)

#define npriv_darray_get(a, i)                                                    \
    (npriv_assert((size_t)(i) < (a)->count), &(a)->data[(i)])

#define npriv_darray_tail(a)                                                      \
    (npriv_assert((a)->count > 0), &(a)->data[(a)->count - 1])

#define npriv_darray_foreach(it, a)                                              \
    for (__typeof__(*(a)->data) *it = (a)->data;                                 \
         it < (a)->data + (a)->count; it++)

#define npriv_darray_try_push(a, ...)                                                  \
    (((a)->count < (a)->capacity                                                       \
      || npriv_darray_try_grow_((npriv_darray_data_alias *)&(a)->data, &(a)->capacity, \
                                sizeof(*(a)->data), NPRIV_DARRAY_ALIGNOF_(a)) == 0)    \
      ? ((a)->data[(a)->count++] = (__VA_ARGS__), 0)                                   \
      : NPRIV_ERROR_MEMORY)

#define npriv_darray_push(a, ...) do {                                            \
    if ((a)->count >= (a)->capacity)                                              \
        npriv_darray_grow_((npriv_darray_data_alias *)&(a)->data, &(a)->capacity, \
                           sizeof(*(a)->data), NPRIV_DARRAY_ALIGNOF_(a));         \
    (a)->data[(a)->count++] = (__VA_ARGS__);                                      \
} while (0)

#define npriv_darray_insert(a, idx, ...) do {                                     \
    const size_t _idx = (idx);                                                    \
    npriv_assert(_idx <= (a)->count);                                             \
    if ((a)->count >= (a)->capacity)                                              \
        npriv_darray_grow_((npriv_darray_data_alias *)&(a)->data, &(a)->capacity, \
                           sizeof(*(a)->data), NPRIV_DARRAY_ALIGNOF_(a));         \
    memmove(&(a)->data[_idx + 1], &(a)->data[_idx],                               \
            ((a)->count - _idx) * sizeof(*(a)->data));                            \
    (a)->data[_idx] = (__VA_ARGS__);                                              \
    (a)->count++;                                                                 \
} while (0)

#define npriv_darray_pop(a) \
    (npriv_assert((a)->count > 0), &(a)->data[--(a)->count])

#define npriv_darray_pop_unsafe(a) \
    (&(a)->data[--(a)->count])

#define npriv_darray_try_reserve(a, cap)                                             \
    npriv_darray_try_reserve_((npriv_darray_data_alias *)&(a)->data, &(a)->capacity, \
                              sizeof(*(a)->data), NPRIV_DARRAY_ALIGNOF_(a), (cap))

#define npriv_darray_reserve(a, cap)                                             \
    npriv_darray_reserve_((npriv_darray_data_alias *)&(a)->data, &(a)->capacity, \
                          sizeof(*(a)->data), NPRIV_DARRAY_ALIGNOF_(a), (cap))

#define npriv_darray_clear(a) do {                                               \
    if ((a)->user_free_func)                                                     \
        for (size_t _i = 0; _i < (a)->count; _i++)                               \
            (a)->user_free_func((a)->user_arg, &(a)->data[_i]);                  \
    (a)->count = 0;                                                              \
} while (0)

#define npriv_darray_remove_range(a, idx, n) do {                                \
    size_t _idx = (idx);                                                         \
    size_t _n = (n);                                                             \
    npriv_assert(_idx + _n <= (a)->count);                                       \
    if ((a)->user_free_func)                                                     \
        for (size_t _i = 0; _i < _n; _i++)                                       \
            (a)->user_free_func((a)->user_arg, &(a)->data[_idx + _i]);           \
    memmove(&(a)->data[_idx], &(a)->data[_idx + _n],                             \
            ((a)->count - _idx - _n) * sizeof(*(a)->data));                      \
    (a)->count -= _n;                                                            \
} while (0)

#define npriv_darray_remove(a, idx) npriv_darray_remove_range((a), (idx), 1)

#define npriv_darray_move(a, from, to) do {                                      \
    __typeof__(a) _npriv_darray_move_a = (a);                                    \
    const size_t _npriv_darray_move_from = (from);                               \
    const size_t _npriv_darray_move_to = (to);                                   \
    npriv_assert(_npriv_darray_move_from < _npriv_darray_move_a->count);         \
    npriv_assert(_npriv_darray_move_to < _npriv_darray_move_a->count);           \
    __typeof__(*_npriv_darray_move_a->data) _npriv_darray_move_elem =            \
        _npriv_darray_move_a->data[_npriv_darray_move_from];                     \
    const size_t _npriv_darray_move_count =                                      \
        _npriv_darray_move_from < _npriv_darray_move_to                          \
        ? _npriv_darray_move_to - _npriv_darray_move_from                        \
        : _npriv_darray_move_from - _npriv_darray_move_to;                       \
    if (_npriv_darray_move_from < _npriv_darray_move_to)                         \
        memmove(&_npriv_darray_move_a->data[_npriv_darray_move_from],            \
                &_npriv_darray_move_a->data[_npriv_darray_move_from + 1],        \
                _npriv_darray_move_count * sizeof(*_npriv_darray_move_a->data)); \
    else if (_npriv_darray_move_from > _npriv_darray_move_to)                    \
        memmove(&_npriv_darray_move_a->data[_npriv_darray_move_to + 1],          \
                &_npriv_darray_move_a->data[_npriv_darray_move_to],              \
                _npriv_darray_move_count * sizeof(*_npriv_darray_move_a->data)); \
    _npriv_darray_move_a->data[_npriv_darray_move_to] = _npriv_darray_move_elem; \
} while (0)

#define npriv_darray_remove_if(a, predicate, arg) do {                           \
    __typeof__(a) _npriv_darray_remove_if_array = (a);                           \
    __typeof__(*(predicate)) *_npriv_darray_remove_if_predicate = (predicate);   \
    __typeof__(arg) _npriv_darray_remove_if_arg = (arg);                         \
    const size_t _npriv_darray_remove_if_old_count =                             \
        _npriv_darray_remove_if_array->count;                                    \
    size_t _npriv_darray_remove_if_dst = 0;                                      \
    for (size_t _src = 0; _src < _npriv_darray_remove_if_old_count; _src++) {    \
        __typeof__(_npriv_darray_remove_if_array->data) _element =               \
            &_npriv_darray_remove_if_array->data[_src];                          \
        if (_npriv_darray_remove_if_predicate(_npriv_darray_remove_if_arg,       \
                                              _element)) {                       \
            if (_npriv_darray_remove_if_array->user_free_func)                   \
                _npriv_darray_remove_if_array->user_free_func(                   \
                    _npriv_darray_remove_if_array->user_arg, _element);          \
            continue;                                                            \
        }                                                                        \
        if (_npriv_darray_remove_if_dst != _src)                                 \
            _npriv_darray_remove_if_array->data[_npriv_darray_remove_if_dst] =   \
                *_element;                                                       \
        _npriv_darray_remove_if_dst++;                                           \
    }                                                                            \
    _npriv_darray_remove_if_array->count = _npriv_darray_remove_if_dst;          \
} while (0)

#define npriv_darray_reset(a) do {                                               \
    npriv_darray_clear(a);                                                       \
    npriv_darray_free_((a)->data, NPRIV_DARRAY_ALIGNOF_(a));                     \
    (a)->data = NULL;                                                            \
    (a)->capacity = 0;                                                           \
} while (0)

#define npriv_darray_set_free_func(a, fn, arg) do {                              \
    npriv_assert((a)->count == 0);                                               \
    (a)->user_free_func = (fn);                                                  \
    (a)->user_arg = (arg);                                                       \
} while (0)

#endif
