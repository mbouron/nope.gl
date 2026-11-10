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
#include <pthread.h>
#include <sched.h>
#include <stdint.h>

#include "../src/utils/spsc_queue.h"

#define NB_ELEMENTS 1000000

/* Large enough to be copied in several steps: a torn element shows */
struct element {
    uint64_t values[8];
};
NAU_DECLARE_SPSC_QUEUE_WITH_NAME(element_queue, struct element);

static void *produce(void *arg)
{
    struct element_queue *queue = arg;
    for (uint64_t i = 0; i < NB_ELEMENTS;) {
        struct element element;
        for (size_t j = 0; j < 8; j++)
            element.values[j] = i + j;
        if (nau_spsc_queue_push(queue, element))
            i++;
        else
            sched_yield();
    }
    return NULL;
}

/* Every element comes out once, whole and in order, through a queue small
 * enough to be full or empty most of the time */
int main(void)
{
    struct element_queue queue = {0};
    int ret = nau_spsc_queue_init(&queue, 4);
    assert(ret == NAU_OK);

    pthread_t producer;
    ret = pthread_create(&producer, NULL, produce, &queue);
    assert(ret == 0);
    for (uint64_t i = 0; i < NB_ELEMENTS;) {
        struct element element;
        if (!nau_spsc_queue_pop(&queue, &element)) {
            sched_yield();
            continue;
        }
        for (size_t j = 0; j < 8; j++)
            assert(element.values[j] == i + j);
        i++;
    }
    pthread_join(producer, NULL);

    struct element element;
    assert(!nau_spsc_queue_pop(&queue, &element));
    nau_spsc_queue_uninit(&queue);
    return 0;
}
