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
#include <stdatomic.h>
#include <stdlib.h>
#include "nopeaudio/nopeaudio.h"

struct state {
    struct nau_engine *engine;
    _Atomic int stop;
    pthread_t control;
    uint32_t destroyed;
};
struct source {
    struct state *owner;
    float value;
};

static int process(void *user, const struct nau_block *p)
{
    const struct source *s = user;
    for (uint32_t i = 0; i < p->frames * p->channels; i++)
        p->output[i] = s->value;
    return NAU_OK;
}

static void destroy(void *user)
{
    struct source *s = user;
    assert(pthread_equal(pthread_self(), s->owner->control));
    s->owner->destroyed++;
    free(s);
}

static void *render(void *user)
{
    struct state *s = user;
    int64_t frame = 0;
    float out[514];
    while (!atomic_load(&s->stop)) {
        assert(nau_engine_render(s->engine, frame, 257, out) == NAU_OK);
        for (uint32_t i = 0; i < 514; i++)
            assert(out[i] >= 0 && out[i] <= 2);
        frame += 257;
    }
    return NULL;
}

int main(void)
{
    struct state s = {.control = pthread_self()};
    atomic_init(&s.stop, 0);
    const struct nau_engine_config config = {48000, 2, 64};
    s.engine = nau_engine_create();
    assert(s.engine);
    assert(nau_engine_init(s.engine, &config) == NAU_OK);
    struct nau_node *gain = NULL;
    gain = nau_node_create();
    assert(gain);
    assert(nau_gain_init(gain, s.engine, 1, 240) == NAU_OK);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, render, &s));
    for (uint32_t i = 0; i < 5000; i++) {
        struct source *source = malloc(sizeof(*source));
        assert(source);
        *source = (struct source){.owner = &s, .value = 1 + (float)(i & 1)};
        struct nau_node *node = NULL;
        const struct nau_processor p = {.user = source, .process = process, .destroy = destroy};
        node = nau_node_create();
        assert(node);
        assert(nau_node_init(node, s.engine, &p) == NAU_OK);
        uint32_t inputs[] = {0};
        const struct nau_route routes[] = {{node, NULL}, {gain, inputs}};
        int ret;
        do {
            ret = nau_engine_commit(s.engine, routes, 2, 1);
            if (ret == NAU_AGAIN)
                sched_yield();
        } while (ret == NAU_AGAIN);
        assert(ret == NAU_OK);
        assert(nau_gain_set(gain, (i & 1) ? 0.25f : 1.f) == NAU_OK);
        nau_node_unrefp(&node);
    }
    atomic_store(&s.stop, 1);
    assert(!pthread_join(thread, NULL));
    nau_node_unrefp(&gain);
    nau_engine_freep(&s.engine);
    assert(s.destroyed == 5000);
    return 0;
}
