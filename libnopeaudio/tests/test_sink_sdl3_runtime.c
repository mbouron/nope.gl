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
#include <stdatomic.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include "nopeaudio/nopeaudio.h"

struct state {
    struct nau_engine *engine;
    struct nau_transport *transport;
    struct nau_sink *sink;
    int expected;
    _Atomic bool done;
};

static void *create(void *arg)
{
    struct state *s = arg;
    assert(nau_sink_backend_init() == NAU_ERROR_INVALID);
    assert(nau_sink_poll() == NAU_ERROR_INVALID);
    assert(nau_sink_backend_uninit() == NAU_ERROR_INVALID);
    s->sink = nau_sink_create();
    assert(s->sink);
    const int ret = nau_sink_init(s->sink, s->transport, NULL);
    assert(ret == s->expected);
    if (ret < 0)
        nau_sink_freep(&s->sink);
    /* SDL keeps data for each thread calling it, which a thread SDL did not create frees */
    SDL_CleanupTLS();
    return NULL;
}

static void *destroy(void *arg)
{
    struct state *s = arg;
    assert(nau_sink_start(s->sink) == NAU_OK);
    assert(nau_sink_stop(s->sink) == NAU_OK);
    nau_sink_freep(&s->sink);
    SDL_CleanupTLS();
    return NULL;
}

static void *cycle(void *arg)
{
    struct state *s = arg;
    for (int i = 0; i < 16; i++) {
        create(s);
        destroy(s);
    }
    atomic_store(&s->done, true);
    return NULL;
}

int main(void)
{
    SDL_SetMainReady();
    assert(SDL_InitSubSystem(SDL_INIT_AUDIO));
    assert(nau_sink_backend_init() == NAU_OK);
    assert(nau_sink_backend_init() == NAU_OK);
    struct state state = {.expected = NAU_OK};
    const struct nau_engine_config config = {48000, 2, 128};
    const struct nau_transport_config transport_config = {0};
    state.engine = nau_engine_create();
    assert(state.engine);
    assert(nau_engine_init(state.engine, &config) == NAU_OK);
    state.transport = nau_transport_create();
    assert(state.transport);
    assert(nau_transport_init(state.transport, state.engine, &transport_config) == NAU_OK);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, create, &state));
    assert(!pthread_join(worker, NULL));
    assert(state.sink);
    assert(nau_sink_backend_uninit() == NAU_OK);
    assert(nau_sink_backend_uninit() == NAU_AGAIN);
    assert(!pthread_create(&worker, NULL, destroy, &state));
    assert(!pthread_join(worker, NULL));
    assert(nau_sink_backend_uninit() == NAU_OK);
    assert(SDL_WasInit(SDL_INIT_AUDIO));
    state.expected = NAU_ERROR_INVALID;
    assert(!pthread_create(&worker, NULL, create, &state));
    assert(!pthread_join(worker, NULL));
    assert(!state.sink);
    assert(nau_sink_backend_init() == NAU_OK);
    state.expected = NAU_OK;
    assert(!pthread_create(&worker, NULL, cycle, &state));
    while (!atomic_load(&state.done)) {
        assert(nau_sink_poll() == NAU_OK);
        SDL_Delay(1);
    }
    assert(!pthread_join(worker, NULL));
    assert(nau_sink_backend_uninit() == NAU_OK);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    assert(!SDL_WasInit(SDL_INIT_AUDIO));
    nau_transport_freep(&state.transport);
    nau_engine_freep(&state.engine);
    return 0;
}
