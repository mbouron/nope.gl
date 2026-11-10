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
#include <stdio.h>

#include "nopeaudio/nopeaudio.h"
#include "utils/time.h"

#define CHECK(cond) do {                                                     \
    if (!(cond)) {                                                           \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        return -1;                                                           \
    }                                                                        \
} while (0)

static int wait_for_render(struct nau_transport *transport, int64_t position)
{
    const int64_t deadline = nau_sink_now() + 2 * NAU_NS_PER_SEC;
    while (nau_transport_get_position(transport) == position && nau_sink_now() < deadline)
        nau_sleep_ms(1);
    CHECK(nau_transport_get_position(transport) > position);
    return 0;
}

static int test_sink(int null_device, uint32_t channels, uint32_t period_ms)
{
    const struct nau_engine_config engine_config = {
        .sample_rate = 48000, .channels = channels, .block_frames = 64,
    };
    const struct nau_transport_config transport_config = {0};
    const struct nau_sink_config sink_config = {
        .null_device = null_device, .period_ms = period_ms,
    };
    struct nau_engine *engine = NULL;
    struct nau_transport *transport = NULL;
    struct nau_sink *sink = NULL;
    engine = nau_engine_create();
    CHECK(engine);
    CHECK(nau_engine_init(engine, &engine_config) == NAU_OK);
    transport = nau_transport_create();
    CHECK(transport);
    CHECK(nau_transport_init(transport, engine, &transport_config) == NAU_OK);
    CHECK(nau_transport_set_duration(transport, 48000 * 60) == NAU_OK);

    /* Freeing a device before starting it must release its buffer and semaphore. */
    sink = nau_sink_create();
    CHECK(sink);
    CHECK(nau_sink_init(sink, transport, &sink_config) == NAU_OK);
    nau_sink_freep(&sink);
    CHECK(!sink);
    sink = nau_sink_create();
    CHECK(sink);
    CHECK(nau_sink_init(sink, transport, &sink_config) == NAU_OK);
    const int64_t period = (int64_t)(period_ms ? period_ms : 10) * NAU_NS_PER_SEC / 1000;
    CHECK(nau_sink_get_latency(sink) == period);
    CHECK(nau_sink_stop(sink) == NAU_OK);
    nau_transport_play(transport);
    for (int i = 0; i < 3; i++) {
        const int64_t before = nau_transport_get_position(transport);
        CHECK(nau_sink_start(sink) == NAU_OK);
        CHECK(nau_sink_start(sink) == NAU_OK);
        CHECK(wait_for_render(transport, before) == 0);
        CHECK(nau_sink_stop(sink) == NAU_OK);
        CHECK(nau_sink_stop(sink) == NAU_OK);
        const int64_t stopped = nau_transport_get_position(transport);
        nau_sleep_ms(20);
        CHECK(nau_transport_get_position(transport) == stopped);
        CHECK(nau_sink_get_status(sink) == NAU_OK && nau_sink_get_underruns(sink) == 0);
    }

    /* Destroying a running sink must join its thread before returning. */
    const int64_t before = nau_transport_get_position(transport);
    CHECK(nau_sink_start(sink) == NAU_OK);
    CHECK(wait_for_render(transport, before) == 0);
    nau_sink_freep(&sink);
    CHECK(!sink);
    const int64_t stopped = nau_transport_get_position(transport);
    nau_sleep_ms(20);
    CHECK(nau_transport_get_position(transport) == stopped);
    nau_sink_freep(&sink);
    nau_transport_freep(&transport);
    nau_engine_freep(&engine);
    return 0;
}

int main(int argc, char **argv)
{
    /* With an argument, test the build's default backend (only for sink=null). */
    const int null_device = argc == 1;
    int ret = test_sink(null_device, 2, 2);
    if (!ret)
        ret = test_sink(null_device, 8, 0);
    return ret != 0;
}
