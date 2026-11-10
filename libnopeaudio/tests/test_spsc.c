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

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "../src/utils/spsc_queue.h"

NAU_DECLARE_SPSC_QUEUE_WITH_NAME(size_queue, size_t);

/* Over-aligned elements take the aligned allocation path */
struct wide {
    _Alignas(128) float values[32];
};
NAU_DECLARE_SPSC_QUEUE_WITH_NAME(wide_queue, struct wide);

static void test_wrap(size_t first)
{
    struct size_queue *queue = calloc(1, sizeof(*queue));
    assert(queue);
    assert(nau_spsc_queue_init(queue, 3) == 0);
    assert(nau_spsc_queue_capacity(queue) == 4);
    atomic_store(&queue->state.head, first);
    atomic_store(&queue->state.tail, first);

    size_t value;
    assert(!nau_spsc_queue_pop(queue, &value));
    for (size_t round = 0; round < 100; round++) {
        for (size_t i = 0; i < 4; i++) {
            value = 4 * round + i;
            assert(nau_spsc_queue_push(queue, value));
        }
        assert(!nau_spsc_queue_push(queue, value));
        for (size_t i = 0; i < 4; i++) {
            assert(nau_spsc_queue_pop(queue, &value));
            assert(value == 4 * round + i);
        }
        assert(!nau_spsc_queue_pop(queue, &value));
    }
    nau_spsc_queue_uninit(queue);
    free(queue);
}

/* A zeroed queue rejects every operation, init() rejects an initialized
 * queue, a failed init() can be retried, and uninit() returns the queue to
 * its zeroed state */
static void test_lifecycle(void)
{
    struct size_queue queue = {0};
    size_t value = 1;
    assert(nau_spsc_queue_capacity(&queue) == 0);
    assert(!nau_spsc_queue_push(&queue, value));
    assert(!nau_spsc_queue_pop(&queue, &value));
    nau_spsc_queue_uninit(&queue);

    assert(nau_spsc_queue_init(&queue, SIZE_MAX) < 0);
    assert(!queue.data);
    assert(nau_spsc_queue_init(&queue, 2) == NAU_OK);
    assert(nau_spsc_queue_init(&queue, 2) == NAU_ERROR_INVALID);
    assert(nau_spsc_queue_push(&queue, value));
    nau_spsc_queue_uninit(&queue);
    assert(!queue.data && nau_spsc_queue_capacity(&queue) == 0);
    assert(!nau_spsc_queue_pop(&queue, &value));
    assert(nau_spsc_queue_init(&queue, 2) == NAU_OK);
    nau_spsc_queue_uninit(&queue);
}

static void test_aligned(void)
{
    struct wide_queue queue = {0};
    assert(nau_spsc_queue_init(&queue, 2) == NAU_OK);
    assert((uintptr_t)queue.data % _Alignof(struct wide) == 0);
    const struct wide in = {.values = {1.f, 2.f}};
    struct wide out = {0};
    assert(nau_spsc_queue_push(&queue, in));
    assert(nau_spsc_queue_pop(&queue, &out));
    assert(out.values[0] == 1.f && out.values[1] == 2.f);
    nau_spsc_queue_uninit(&queue);
}

int main(void)
{
    static_assert(_Alignof(struct spsc_queue) <= _Alignof(max_align_t));
    static_assert(offsetof(struct spsc_queue, tail) - offsetof(struct spsc_queue, head) >= 64);
    test_wrap(0);
    test_wrap(SIZE_MAX - 1);
    test_lifecycle();
    test_aligned();

    struct size_queue queue = {0};
    assert(nau_spsc_queue_init(&queue, 0) < 0);
    assert(nau_spsc_queue_init(&queue, SIZE_MAX) < 0);
    nau_spsc_queue_uninit(&queue);

    /* Element sizes the typed macros cannot express */
    struct spsc_queue state = {0};
    nau_spsc_queue_data_alias data = NULL;
    assert(nau_spsc_queue_init_(&state, &data, 1, 0, 1) < 0);
    assert(nau_spsc_queue_init_(&state, &data, 2, SIZE_MAX, 1) < 0);
    assert(!data);
    return 0;
}
