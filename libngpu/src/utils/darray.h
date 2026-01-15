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

#ifndef NGPU_DARRAY_H
#define NGPU_DARRAY_H

#include <stdint.h>
#include <stdlib.h>

#include "utils.h"

#define NGPU_DARRAY_FLAG_ALIGNED (1U << 0)

struct ngpu_darray {
    uint8_t *data;
    size_t count;
    size_t capacity;
    size_t element_size;
    ngpu_user_free_func_type user_free_func;
    void *user_arg;
    int (*reserve)(struct ngpu_darray *darray, size_t capacity);
    void (*release)(void *ptr);
};

void ngpu_darray_init(struct ngpu_darray *darray, size_t element_size, uint32_t flags);
void ngpu_darray_set_free_func(struct ngpu_darray *darray, ngpu_user_free_func_type user_free_func, void *user_arg);
void *ngpu_darray_push(struct ngpu_darray *darray, const void *element);
void *ngpu_darray_pop(struct ngpu_darray *darray);
void *ngpu_darray_pop_unsafe(struct ngpu_darray *darray);
void *ngpu_darray_tail(const struct ngpu_darray *darray);
void *ngpu_darray_get(const struct ngpu_darray *darray, size_t index);
void ngpu_darray_remove(struct ngpu_darray *darray, size_t index);
void ngpu_darray_remove_range(struct ngpu_darray *darray, size_t index, size_t count);
void ngpu_darray_clear(struct ngpu_darray *darray);
void ngpu_darray_reset(struct ngpu_darray *darray);

static inline size_t ngpu_darray_count(const struct ngpu_darray *darray)
{
    return darray->count;
}

static inline void *ngpu_darray_data(const struct ngpu_darray *darray)
{
    return (void *)darray->data;
}

#endif
