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
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "nopeaudio/nopeaudio.h"
#include "../src/utils/time.h"

/*
 * The renderer playing while the control thread seeks and moves the loop
 * around, as an editor does: only valid audio plays, nothing crashes, and
 * the sanitizers find no race.
 */

struct control {
    struct nau_transport *transport;
    struct nau_media *media;
    _Atomic int stop;
};

static void *run_control(void *arg)
{
    struct control *c = arg;
    uint32_t seed = 1;
    for (int i = 0; i < 400 && !atomic_load(&c->stop); i++) {
        seed = seed * 1103515245 + 12345;
        const int64_t start = (seed >> 8) % 40000, end = start + 2400 + (seed >> 4) % 8000;
        if (i % 3 == 0) {
            nau_transport_set_looping(c->transport, 1);
            assert(nau_transport_set_loop_range(c->transport, start, end) == NAU_OK);
            assert(nau_media_set_loop(c->media, start, end) == NAU_OK);
        } else if (i % 3 == 1) {
            assert(nau_transport_seek(c->transport, (seed >> 3) % 48000) == NAU_OK);
        } else {
            nau_transport_set_looping(c->transport, 0);
            assert(nau_media_set_loop(c->media, 0, 0) == NAU_OK);
        }
        nau_sleep_ms(2);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct nau_engine *e = NULL;
    struct nau_media *m = NULL;
    struct nau_node *source = NULL;
    struct nau_transport *t = NULL;
    const struct nau_engine_config config = {48000, 2, 64};
    e = nau_engine_create();
    assert(e);
    assert(nau_engine_init(e, &config) == NAU_OK);
    m = nau_media_create();
    assert(m);
    assert(nau_media_init(m, e, argv[1], NULL) == NAU_OK);
    source = nau_node_create();
    assert(source);
    assert(nau_media_init_source(m, source, 0, 0, 48000) == NAU_OK);
    const struct nau_route route = {source, NULL};
    assert(nau_engine_commit(e, &route, 1, 0) == NAU_OK);
    const struct nau_transport_config tc = {.declick_frames = 240, .ready_timeout_frames = 4800};
    t = nau_transport_create();
    assert(t);
    assert(nau_transport_init(t, e, &tc) == NAU_OK);
    assert(nau_transport_set_duration(t, 48000) == NAU_OK);
    nau_transport_play(t);

    struct control c = {.transport = t, .media = m};
    atomic_init(&c.stop, 0);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, run_control, &c));
    float out[2 * 256];
    int64_t audible = 0;
    for (int i = 0; i < 2000; i++) {
        if (i % 4 == 0)
            nau_transport_play(t);
        assert(nau_transport_render(t, 0, 256, out) >= 0);
        for (uint32_t f = 0; f < 512; f++) {
            assert(isfinite(out[f]) && fabsf(out[f]) <= 1.f);
            audible += out[f] != 0;
        }
        if (i % 8 == 0)
            nau_sleep_ms(1);
    }
    atomic_store(&c.stop, 1);
    assert(!pthread_join(thread, NULL));
    struct nau_media_stats stats;
    nau_media_get_stats(m, &stats);
    printf("concurrent: %" PRId64 " audible samples, %" PRId64 " requests, %" PRId64 " errors\n",
           audible, stats.requests, stats.errors);
    assert(audible > 0 && stats.errors == 0);
    nau_transport_freep(&t);
    nau_node_unrefp(&source);
    nau_engine_freep(&e);
    nau_media_unrefp(&m);
    return 0;
}
