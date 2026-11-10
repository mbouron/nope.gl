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
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "nopeaudio/nopeaudio.h"
#include "../src/utils/time.h"

static int null_device = 1;

static int process(void *user, const struct nau_block *p)
{
    atomic_fetch_add((_Atomic uint32_t *)user, 1);
    return NAU_OK;
}

static int ones(void *user, const struct nau_block *p)
{
    for (uint32_t i = 0; i < p->frames * p->channels; i++)
        p->output[i] = 1;
    return NAU_OK;
}

/* A device playing a transport: it runs on paused, its callbacks timing the clock */
static void test_transport(void)
{
    struct nau_engine *e = NULL;
    struct nau_node *n = NULL;
    struct nau_transport *t = NULL;
    struct nau_sink *sink = NULL;
    const struct nau_engine_config config = {48000, 2, 128};
    const struct nau_processor p = {.process = ones};
    e = nau_engine_create();
    assert(e);
    assert(nau_engine_init(e, &config) == NAU_OK);
    n = nau_node_create();
    assert(n);
    assert(nau_node_init(n, e, &p) == NAU_OK);
    const struct nau_route route = {n, NULL};
    assert(nau_engine_commit(e, &route, 1, 0) == NAU_OK);
    const struct nau_transport_config tc = {.declick_frames = 240, .ready_timeout_frames = 4800};
    t = nau_transport_create();
    assert(t);
    assert(nau_transport_init(t, e, &tc) == NAU_OK);
    assert(nau_transport_set_duration(t, 48000 * 60) == NAU_OK);
    const struct nau_sink_config sink_config = {.null_device = null_device, .period_ms = 10};
    sink = nau_sink_create();
    assert(sink);
    assert(nau_sink_init(sink, t, &sink_config) == NAU_OK);
    assert(nau_sink_get_latency(sink) > 0);
    assert(nau_sink_start(sink) == NAU_OK);
    /* Paused: the device runs, the position stays */
    nau_sleep_ms(50);
    assert(nau_transport_get_position(t) == 0);
    nau_transport_play(t);
    const int64_t start = nau_sink_now();
    nau_sleep_ms(300);
    const int64_t played = nau_transport_get_position(t);
    const int64_t elapsed = nau_sink_now() - start;
    /* As fast as time goes, within a few periods */
    assert(llabs(played - elapsed * 48 / 1000000) < 48 * 60);
    struct nau_clock clock;
    assert(nau_transport_get_clock(t, &clock) == NAU_OK && clock.playing);
    /* The history lookup clamps when the device falls behind its nominal rate. */
    int64_t heard;
    assert(nau_transport_get_position_at(t, nau_sink_now(), &heard) == NAU_OK);
    const int64_t rendered = nau_transport_get_position(t);
    assert(heard <= rendered && rendered - heard < (nau_sink_get_latency(sink) + 60000000) * 48 / 1000000);
    nau_transport_pause(t);
    nau_sleep_ms(50);
    const int64_t paused = nau_transport_get_position(t);
    nau_sleep_ms(50);
    assert(nau_transport_get_position(t) == paused);
    /* A seek while paused, applied by the running device */
    assert(nau_transport_seek(t, 48000 * 10) == NAU_OK);
    nau_sleep_ms(50);
    assert(nau_transport_get_position(t) == 48000 * 10);
    assert(nau_sink_get_status(sink) == NAU_OK);
    assert(nau_sink_stop(sink) == NAU_OK);
    nau_sink_freep(&sink);
    nau_transport_freep(&t);
    nau_node_unrefp(&n);
    nau_engine_freep(&e);
}

/* A stopped device does not render, and plays on where it stopped once
 * started again */
static void test_stop(void)
{
    _Atomic uint32_t calls = 0;
    struct nau_engine *e = NULL;
    struct nau_node *n = NULL;
    struct nau_transport *t = NULL;
    struct nau_sink *sink = NULL;
    const struct nau_engine_config config = {48000, 2, 128};
    const struct nau_processor p = {.user = &calls, .process = process};
    e = nau_engine_create();
    assert(e);
    assert(nau_engine_init(e, &config) == NAU_OK);
    n = nau_node_create();
    assert(n);
    assert(nau_node_init(n, e, &p) == NAU_OK);
    const struct nau_route route = {n, NULL};
    assert(nau_engine_commit(e, &route, 1, 0) == NAU_OK);
    const struct nau_transport_config tc = {0};
    t = nau_transport_create();
    assert(t);
    assert(nau_transport_init(t, e, &tc) == NAU_OK);
    assert(nau_transport_set_duration(t, 48000 * 60) == NAU_OK);
    const struct nau_sink_config sink_config = {.null_device = null_device};
    /* A failed initialization leaves the sink to initialize again */
    sink = nau_sink_create();
    assert(sink);
    assert(nau_sink_init(sink, NULL, &sink_config) == NAU_ERROR_INVALID);
    assert(nau_sink_init(sink, t, &sink_config) == NAU_OK);
    nau_transport_play(t);
    assert(nau_sink_start(sink) == NAU_OK);
    for (uint32_t i = 0; i < 100 && !atomic_load(&calls); i++)
        nau_sleep_ms(10);
    assert(nau_sink_stop(sink) == NAU_OK);
    const uint32_t stopped_calls = atomic_load(&calls);
    const int64_t stopped = nau_transport_get_position(t);
    assert(stopped_calls > 0 && stopped > 0 && nau_sink_get_underruns(sink) == 0);
    nau_sleep_ms(20);
    assert(atomic_load(&calls) == stopped_calls && nau_transport_get_position(t) == stopped);
    assert(nau_sink_start(sink) == NAU_OK);
    for (uint32_t i = 0; i < 100 && atomic_load(&calls) == stopped_calls; i++)
        nau_sleep_ms(10);
    assert(nau_sink_stop(sink) == NAU_OK);
    assert(atomic_load(&calls) > stopped_calls && nau_transport_get_position(t) > stopped);
    assert(nau_sink_get_status(sink) == NAU_OK);
    nau_sink_freep(&sink);
    nau_transport_freep(&t);
    nau_node_unrefp(&n);
    nau_engine_freep(&e);
}

static void test_null_periods(void)
{
    const uint32_t rates[] = {1, 1, 48000};
    const uint32_t periods[] = {0, UINT32_MAX, UINT32_MAX};
    const int64_t frames[] = {1, 65536, 65536};
    for (uint32_t i = 0; i < 3; i++) {
        struct nau_engine *engine = NULL;
        struct nau_transport *transport = NULL;
        struct nau_sink *sink = NULL;
        const struct nau_engine_config config = {rates[i], 2, 128};
        const struct nau_transport_config transport_config = {0};
        const struct nau_sink_config sink_config = {.null_device = 1, .period_ms = periods[i]};
        engine = nau_engine_create();
        assert(engine);
        assert(nau_engine_init(engine, &config) == NAU_OK);
        transport = nau_transport_create();
        assert(transport);
        assert(nau_transport_init(transport, engine, &transport_config) == NAU_OK);
        sink = nau_sink_create();
        assert(sink);
        assert(nau_sink_init(sink, transport, &sink_config) == NAU_OK);
        assert(nau_sink_get_latency(sink) == frames[i] * 1000000000 / rates[i]);
        assert(nau_sink_start(sink) == NAU_OK);
        nau_sleep_ms(20);
        const int64_t start = nau_sink_now();
        assert(nau_sink_stop(sink) == NAU_OK);
        assert(nau_sink_now() - start < 500000000);
        nau_sink_freep(&sink);
        nau_transport_freep(&transport);
        nau_engine_freep(&engine);
    }
}

int main(int argc, char **argv)
{
    test_null_periods();
    assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "--device")));
    null_device = argc == 1;
    if (!null_device)
        assert(nau_sink_backend_init() == NAU_OK);
    test_stop();
    test_transport();
    if (!null_device)
        assert(nau_sink_backend_uninit() == NAU_OK);
    return 0;
}
