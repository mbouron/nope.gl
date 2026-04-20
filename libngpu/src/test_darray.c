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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils/darray.h"
#include "utils/utils.h"

struct my_item {
    int id;
    void *ptr;
};

struct aligned_mat {
    _Alignas(32) float m[16];
};

static void free_elem(void *user_arg, void *data)
{
    struct my_item *item = data;
    ngpu_assert(!strcmp(user_arg, "test"));
    free(item->ptr);
}

static int g_free_calls;
static void count_free(void *user_arg, void *data)
{
    (void)user_arg;
    (void)data;
    g_free_calls++;
}

static void test_basic(void)
{
    NGPU_DARRAY(int) a = {0};

    ngpu_assert(a.count == 0);

    ngpu_assert(ngpu_darray_push(&a, 0xFF) == 0);
    ngpu_assert(*ngpu_darray_tail(&a) == 0xFF);
    ngpu_assert(a.count == 1);

    ngpu_assert(ngpu_darray_push(&a, 0xFFFF) == 0);
    ngpu_assert(*ngpu_darray_tail(&a) == 0xFFFF);
    ngpu_assert(a.count == 2);

    ngpu_assert(a.data[0] == 0xFF);

    /* get/tail/pop assert on out-of-bounds / empty access */
    ngpu_assert(*ngpu_darray_get(&a, 0) == 0xFF);
    ngpu_assert(*ngpu_darray_get(&a, 1) == 0xFFFF);

    int *popped = ngpu_darray_pop(&a);
    ngpu_assert(*popped == 0xFFFF);
    ngpu_assert(a.count == 1);

    popped = ngpu_darray_pop(&a);
    ngpu_assert(*popped == 0xFF);
    ngpu_assert(a.count == 0);

    for (int i = 0; i < 1000; i++)
        ngpu_assert(ngpu_darray_push(&a, i) == 0);
    ngpu_assert(a.count == 1000);

    int sum = 0;
    for (size_t i = 0; i < a.count; i++)
        sum += a.data[i];
    ngpu_assert(sum == 1000 * 999 / 2);

    /* same iteration via foreach */
    int sum2 = 0;
    ngpu_darray_foreach(it, &a)
        sum2 += *it;
    ngpu_assert(sum2 == 1000 * 999 / 2);

    ngpu_darray_reset(&a);
    ngpu_assert(a.data == NULL && a.count == 0 && a.capacity == 0);
}

static void test_compound_literal_push(void)
{
    NGPU_DARRAY(struct my_item) a = {0};

    /* libplacebo-style compound literal: commas inside __VA_ARGS__ */
    ngpu_assert(ngpu_darray_push(&a, (struct my_item){.id = 7, .ptr = (void *)0xbeef}) == 0);
    struct my_item *p = ngpu_darray_tail(&a);
    ngpu_assert(p->id == 7 && p->ptr == (void *)0xbeef);

    /* zero-fill via compound literal */
    ngpu_assert(ngpu_darray_push(&a, (struct my_item){0}) == 0);
    p = ngpu_darray_tail(&a);
    ngpu_assert(p->id == 0 && p->ptr == NULL);
    ngpu_assert(a.count == 2);

    /* copy from an existing struct */
    const struct my_item src = {.id = 42, .ptr = (void *)0xdead};
    ngpu_assert(ngpu_darray_push(&a, src) == 0);
    p = ngpu_darray_tail(&a);
    ngpu_assert(p->id == 42 && p->ptr == (void *)0xdead);

    ngpu_darray_reset(&a);
}

static void test_reserve(void)
{
    NGPU_DARRAY(int) a = {0};

    ngpu_assert(ngpu_darray_reserve(&a, 128) == 0);
    ngpu_assert(a.capacity >= 128);
    ngpu_assert(a.count == 0);

    const size_t cap_before = a.capacity;
    ngpu_assert(ngpu_darray_reserve(&a, 4) == 0);
    ngpu_assert(a.capacity == cap_before);

    ngpu_darray_reset(&a);
}

static void test_remove_range(void)
{
    NGPU_DARRAY(int) a = {0};

    for (int i = 0; i < 6; i++)
        ngpu_darray_push(&a, i);

    ngpu_darray_remove(&a, 0);
    ngpu_assert(a.count == 5);
    ngpu_assert(a.data[0] == 1);

    ngpu_darray_remove_range(&a, 1, 3); /* drops 2,3,4 */
    ngpu_assert(a.count == 2);
    ngpu_assert(a.data[0] == 1);
    ngpu_assert(a.data[1] == 5);

    ngpu_darray_reset(&a);
}

static void test_clear_vs_reset(void)
{
    NGPU_DARRAY(int) a = {0};

    for (int i = 0; i < 16; i++)
        ngpu_darray_push(&a, i);

    const size_t cap_before_clear = a.capacity;
    ngpu_darray_clear(&a);
    ngpu_assert(a.count == 0);
    ngpu_assert(a.capacity == cap_before_clear);
    ngpu_assert(a.data != NULL);

    ngpu_darray_reset(&a);
    ngpu_assert(a.data == NULL && a.count == 0 && a.capacity == 0);
}

static void test_user_free_func(void)
{
    NGPU_DARRAY(struct my_item) a = {0};
    ngpu_darray_set_free_func(&a, free_elem, (void *)"test");

    for (int i = 0; i < 6; i++) {
        void *p = malloc(10u + (size_t)i);
        ngpu_assert(p);
        ngpu_darray_push(&a, (struct my_item){.id = i, .ptr = p});
    }

    ngpu_darray_remove_range(&a, 1, 3);
    ngpu_assert(a.count == 3);

    ngpu_darray_clear(&a);
    ngpu_assert(a.count == 0);

    void *again_ptr = malloc(8);
    ngpu_assert(again_ptr);
    ngpu_darray_push(&a, (struct my_item){.id = 99, .ptr = again_ptr});
    ngpu_darray_reset(&a);
}

static void test_free_func_call_count(void)
{
    NGPU_DARRAY(struct my_item) a = {0};
    ngpu_darray_set_free_func(&a, count_free, NULL);

    for (int i = 0; i < 10; i++)
        ngpu_darray_push(&a, (struct my_item){.id = i, .ptr = NULL});

    g_free_calls = 0;
    ngpu_darray_remove_range(&a, 2, 4);
    ngpu_assert(g_free_calls == 4);

    g_free_calls = 0;
    ngpu_darray_clear(&a);
    ngpu_assert(g_free_calls == 6);

    g_free_calls = 0;
    ngpu_darray_reset(&a);
    ngpu_assert(g_free_calls == 0);
}

static void test_aligned(void)
{
    NGPU_DARRAY(struct aligned_mat) a = {0};

    for (int i = 0; i < 32; i++) {
        struct aligned_mat m = {0};
        m.m[0] = (float)i;
        ngpu_assert(ngpu_darray_push(&a, m) == 0);
    }
    ngpu_assert(a.count == 32);

    /* base pointer must satisfy the element's alignment */
    ngpu_assert(((uintptr_t)a.data % _Alignof(struct aligned_mat)) == 0);

    /* data survived the growth copy */
    for (int i = 0; i < 32; i++)
        ngpu_assert(a.data[i].m[0] == (float)i);

    ngpu_darray_reset(&a);
    ngpu_assert(a.data == NULL && a.count == 0 && a.capacity == 0);
}

int main(void)
{
    test_basic();
    test_compound_literal_push();
    test_reserve();
    test_remove_range();
    test_clear_vs_reset();
    test_user_free_func();
    test_free_func_call_count();
    test_aligned();
    return 0;
}
