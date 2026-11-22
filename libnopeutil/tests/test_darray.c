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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libnopeutil/darray.h"
#include "libnopeutil/utils.h"

struct elem {
    int id;
    void *ptr;
};

struct aligned_mat {
    _Alignas(32) float m[16];
};

NPRIV_DECLARE_DARRAY_WITH_NAME(int_darray, int);
NPRIV_DEFINE_DARRAY_FIND(int_darray)

static void free_elem(void *user_arg, void *data)
{
    struct elem *elem = data;
    npriv_assert(!strcmp(user_arg, "test"));
    free(elem->ptr);
}

static int g_free_call_count;
static void count_free_call(void *user_arg, void *data)
{
    (void)user_arg;
    (void)data;
    g_free_call_count++;
}

static int divisible_by(const int *divisor, const int *data)
{
    return *data % *divisor == 0;
}

static void test_basic(void)
{
    struct int_darray a = {0};

    npriv_assert(a.count == 0);

    npriv_assert(npriv_darray_try_push(&a, 0xFF) == 0);
    npriv_assert(*npriv_darray_tail(&a) == 0xFF);
    npriv_assert(a.count == 1);

    npriv_assert(npriv_darray_try_push(&a, 0xFFFF) == 0);
    npriv_assert(*npriv_darray_tail(&a) == 0xFFFF);
    npriv_assert(a.count == 2);

    npriv_assert(a.data[0] == 0xFF);

    npriv_assert(int_darray_find(&a, 0xFF) == 0);
    npriv_assert(int_darray_find(&a, 0xFFFF) == 1);
    npriv_assert(int_darray_find(&a, 42) == SIZE_MAX);

    /* get/tail/pop assert on out-of-bounds / empty access */
    npriv_assert(*npriv_darray_get(&a, 0) == 0xFF);
    npriv_assert(*npriv_darray_get(&a, 1) == 0xFFFF);

    int *popped = npriv_darray_pop(&a);
    npriv_assert(*popped == 0xFFFF);
    npriv_assert(a.count == 1);

    popped = npriv_darray_pop(&a);
    npriv_assert(*popped == 0xFF);
    npriv_assert(a.count == 0);
    npriv_assert(int_darray_find(&a, 0xFF) == SIZE_MAX);

    for (int i = 0; i < 1000; i++)
        npriv_assert(npriv_darray_try_push(&a, i) == 0);
    npriv_assert(a.count == 1000);

    int sum = 0;
    for (size_t i = 0; i < a.count; i++)
        sum += a.data[i];
    npriv_assert(sum == 1000 * 999 / 2);

    /* same iteration via foreach */
    int sum2 = 0;
    npriv_darray_foreach(it, &a)
        sum2 += *it;
    npriv_assert(sum2 == 1000 * 999 / 2);

    npriv_darray_reset(&a);
    npriv_assert(a.data == NULL && a.count == 0 && a.capacity == 0);
}

static void test_compound_literal_push(void)
{
    NPRIV_DARRAY(struct elem) a = {0};

    /* compound literal: commas inside __VA_ARGS__ */
    npriv_assert(npriv_darray_try_push(&a, (struct elem){.id = 7, .ptr = (void *)0xbeef}) == 0);
    struct elem *p = npriv_darray_tail(&a);
    npriv_assert(p->id == 7 && p->ptr == (void *)0xbeef);

    /* zero-fill via compound literal */
    npriv_assert(npriv_darray_try_push(&a, (struct elem){0}) == 0);
    p = npriv_darray_tail(&a);
    npriv_assert(p->id == 0 && p->ptr == NULL);
    npriv_assert(a.count == 2);

    /* copy from an existing struct */
    const struct elem src = {.id = 42, .ptr = (void *)0xdead};
    npriv_assert(npriv_darray_try_push(&a, src) == 0);
    p = npriv_darray_tail(&a);
    npriv_assert(p->id == 42 && p->ptr == (void *)0xdead);

    npriv_darray_reset(&a);
}

static void test_reserve(void)
{
    NPRIV_DARRAY(int) a = {0};

    npriv_assert(npriv_darray_try_reserve(&a, 128) == 0);
    npriv_assert(a.capacity >= 128);
    npriv_assert(a.count == 0);

    const size_t cap_before = a.capacity;
    npriv_assert(npriv_darray_try_reserve(&a, 4) == 0);
    npriv_assert(a.capacity == cap_before);

    npriv_darray_reset(&a);
}

static void test_remove_range(void)
{
    NPRIV_DARRAY(int) a = {0};

    for (int i = 0; i < 6; i++)
        npriv_darray_try_push(&a, i);

    npriv_darray_remove(&a, 0);
    npriv_assert(a.count == 5);
    npriv_assert(a.data[0] == 1);

    npriv_darray_remove_range(&a, 1, 3); /* drops 2,3,4 */
    npriv_assert(a.count == 2);
    npriv_assert(a.data[0] == 1);
    npriv_assert(a.data[1] == 5);

    npriv_darray_reset(&a);
}

static void test_remove_if(void)
{
    NPRIV_DARRAY(int) a = {0};
    npriv_darray_set_free_func(&a, count_free_call, NULL);

    for (int i = 0; i < 6; i++)
        npriv_darray_push(&a, i);

    const int divisor = 2;
    g_free_call_count = 0;
    npriv_darray_remove_if(&a, divisible_by, &divisor);
    npriv_assert(g_free_call_count == 3);
    npriv_assert(a.count == 3);
    npriv_assert(a.data[0] == 1);
    npriv_assert(a.data[1] == 3);
    npriv_assert(a.data[2] == 5);

    npriv_darray_remove_if(&a, divisible_by, &divisor);
    npriv_assert(g_free_call_count == 3);

    npriv_darray_reset(&a);
    npriv_assert(g_free_call_count == 6);
}

static void test_insert(void)
{
    NPRIV_DARRAY(int) a = {0};

    /* Fill the initial allocation so the first insertion also exercises growth. */
    for (int i = 0; i < 8; i++)
        npriv_darray_push(&a, i);

    npriv_darray_insert(&a, 4, 42);
    npriv_assert(a.count == 9);
    for (int i = 0; i < 4; i++)
        npriv_assert(a.data[i] == i);
    npriv_assert(a.data[4] == 42);
    for (int i = 5; i < 9; i++)
        npriv_assert(a.data[i] == i - 1);

    npriv_darray_insert(&a, 0, -1);
    npriv_darray_insert(&a, a.count, 8);
    npriv_assert(a.data[0] == -1);
    npriv_assert(*npriv_darray_tail(&a) == 8);

    npriv_darray_reset(&a);
}

static void test_move(void)
{
    const struct {
        size_t from;
        size_t to;
        int expected[5];
    } cases[] = {
        {0, 4, {1, 2, 3, 4, 0}},
        {4, 0, {4, 0, 1, 2, 3}},
        {1, 3, {0, 2, 3, 1, 4}},
        {3, 1, {0, 3, 1, 2, 4}},
        {1, 2, {0, 2, 1, 3, 4}},
        {2, 1, {0, 2, 1, 3, 4}},
        {0, 0, {0, 1, 2, 3, 4}},
        {2, 2, {0, 1, 2, 3, 4}},
        {4, 4, {0, 1, 2, 3, 4}},
    };
    int values[5] = {0};
    NPRIV_DARRAY(struct elem) a = {0};
    npriv_darray_set_free_func(&a, count_free_call, NULL);
    for (int i = 0; i < 5; i++)
        npriv_darray_push(&a, (struct elem){0});

    struct elem *const data = a.data;
    const size_t capacity = a.capacity;
    g_free_call_count = 0;
    for (size_t i = 0; i < NPRIV_ARRAY_NB(cases); i++) {
        for (int j = 0; j < 5; j++)
            a.data[j] = (struct elem){.id = j, .ptr = &values[j]};

        npriv_darray_move(&a, cases[i].from, cases[i].to);
        npriv_assert(a.count == 5 && a.capacity == capacity && a.data == data);
        npriv_assert(g_free_call_count == 0);
        for (size_t j = 0; j < a.count; j++) {
            npriv_assert(a.data[j].id == cases[i].expected[j]);
            npriv_assert(a.data[j].ptr == &values[cases[i].expected[j]]);
        }
    }

    npriv_darray_reset(&a);
    npriv_assert(g_free_call_count == 5);
}

static void test_move_arguments(void)
{
    struct int_darray a = {0};
    npriv_darray_push(&a, 0);
    npriv_darray_move(&a, 0, 0);
    npriv_assert(a.count == 1 && a.data[0] == 0);

    npriv_darray_push(&a, 1);
    npriv_darray_push(&a, 2);
    struct int_darray *arrays[] = {&a};
    size_t index = 0, from = 0, to = 2;
    npriv_darray_move(arrays[index++], from++, to--);
    npriv_assert(index == 1 && from == 1 && to == 1);
    npriv_assert(a.data[0] == 1 && a.data[1] == 2 && a.data[2] == 0);
    npriv_darray_reset(&a);
}

static void test_clear_vs_reset(void)
{
    NPRIV_DARRAY(int) a = {0};

    for (int i = 0; i < 16; i++)
        npriv_darray_try_push(&a, i);

    const size_t cap_before_clear = a.capacity;
    npriv_darray_clear(&a);
    npriv_assert(a.count == 0);
    npriv_assert(a.capacity == cap_before_clear);
    npriv_assert(a.data != NULL);

    npriv_darray_reset(&a);
    npriv_assert(a.data == NULL && a.count == 0 && a.capacity == 0);
}

static void test_user_free_func(void)
{
    NPRIV_DARRAY(struct elem) a = {0};
    npriv_darray_set_free_func(&a, free_elem, (void *)"test");

    for (int i = 0; i < 6; i++) {
        void *p = malloc(10u + (size_t)i);
        npriv_assert(p);
        npriv_darray_try_push(&a, (struct elem){.id = i, .ptr = p});
    }

    npriv_darray_remove_range(&a, 1, 3);
    npriv_assert(a.count == 3);

    npriv_darray_clear(&a);
    npriv_assert(a.count == 0);

    void *again_ptr = malloc(8);
    npriv_assert(again_ptr);
    npriv_darray_try_push(&a, (struct elem){.id = 99, .ptr = again_ptr});
    npriv_darray_reset(&a);
}

static void test_free_func_call_count(void)
{
    NPRIV_DARRAY(struct elem) a = {0};
    npriv_darray_set_free_func(&a, count_free_call, NULL);

    for (int i = 0; i < 10; i++)
        npriv_darray_try_push(&a, (struct elem){.id = i, .ptr = NULL});

    g_free_call_count = 0;
    npriv_darray_remove_range(&a, 2, 4);
    npriv_assert(g_free_call_count == 4);

    g_free_call_count = 0;
    npriv_darray_clear(&a);
    npriv_assert(g_free_call_count == 6);

    g_free_call_count = 0;
    npriv_darray_reset(&a);
    npriv_assert(g_free_call_count == 0);
}

static void test_aligned(void)
{
    NPRIV_DARRAY(struct aligned_mat) a = {0};

    for (int i = 0; i < 32; i++) {
        struct aligned_mat m = {0};
        m.m[0] = (float)i;
        npriv_assert(npriv_darray_try_push(&a, m) == 0);
    }
    npriv_assert(a.count == 32);

    /* base pointer must satisfy the element's alignment */
    npriv_assert(((uintptr_t)a.data % _Alignof(struct aligned_mat)) == 0);

    /* data survived the growth copy */
    for (int i = 0; i < 32; i++)
        npriv_assert(a.data[i].m[0] == (float)i);

    npriv_darray_reset(&a);
    npriv_assert(a.data == NULL && a.count == 0 && a.capacity == 0);
}

int main(void)
{
    test_basic();
    test_compound_literal_push();
    test_reserve();
    test_remove_range();
    test_remove_if();
    test_insert();
    test_move();
    test_move_arguments();
    test_clear_vs_reset();
    test_user_free_func();
    test_free_func_call_count();
    test_aligned();
    return 0;
}
