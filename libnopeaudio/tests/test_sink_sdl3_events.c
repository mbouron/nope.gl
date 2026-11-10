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
#include <stdint.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>

static bool fail_open;
static bool fail_format;
static int device_rate = 48000;
static int reported_frames = 2400;
static uint32_t opens;

static bool get_format(SDL_AudioDeviceID device, SDL_AudioSpec *spec, int *frames)
{
    if (fail_format)
        return SDL_SetError("injected format failure");
    if (!SDL_GetAudioDeviceFormat(device, spec, frames))
        return false;
    spec->freq = device_rate;
    *frames = reported_frames;
    return true;
}

static SDL_AudioStream *mock_open_device(SDL_AudioDeviceID device, const SDL_AudioSpec *spec,
                                  SDL_AudioStreamCallback callback, void *user)
{
    opens++;
    if (fail_open) {
        SDL_SetError("injected open failure");
        return NULL;
    }
    return SDL_OpenAudioDeviceStream(device, spec, callback, user);
}

/* Keep SDL's stream and callback threads; inject only device observations. */
#define SDL_GetAudioDeviceFormat get_format
#define SDL_OpenAudioDeviceStream mock_open_device
#include "../src/sink/sink_sdl3.c"

static _Atomic uint32_t calls;
static _Atomic int source_status;

static int process(void *user, const struct nau_block *p)
{
    atomic_fetch_add(&calls, 1);
    return atomic_load(&source_status);
}

static void event(Uint32 type, bool recording)
{
    SDL_Event e = {.adevice = {.type = type, .which = 12345, .recording = recording}};
    assert(SDL_PushEvent(&e));
}

static void wait_callback(void)
{
    const uint32_t before = atomic_load(&calls);
    for (int i = 0; i < 200 && atomic_load(&calls) == before; i++)
        SDL_Delay(5);
    assert(atomic_load(&calls) > before);
}

int main(void)
{
    assert(nau_sink_poll() == NAU_ERROR_INVALID);
    assert(nau_sink_backend_init() == NAU_OK);
    struct nau_engine *engine = NULL;
    struct nau_node *node = NULL;
    struct nau_transport *transport = NULL;
    struct nau_sink *sink = NULL;
    const struct nau_engine_config config = {48000, 2, 128};
    const struct nau_processor processor = {.process = process};
    engine = nau_engine_create();
    assert(engine);
    assert(nau_engine_init(engine, &config) == NAU_OK);
    node = nau_node_create();
    assert(node);
    assert(nau_node_init(node, engine, &processor) == NAU_OK);
    const struct nau_route route = {node, NULL};
    assert(nau_engine_commit(engine, &route, 1, 0) == NAU_OK);
    const struct nau_transport_config transport_config = {0};
    transport = nau_transport_create();
    assert(transport);
    assert(nau_transport_init(transport, engine, &transport_config) == NAU_OK);
    assert(nau_transport_set_duration(transport, 48000 * 60) == NAU_OK);
    nau_transport_play(transport);
    sink = nau_sink_create();
    assert(sink);
    assert(nau_sink_init(sink, transport, NULL) == NAU_OK);
    assert(nau_sink_get_latency(sink) == 100000000);

    /* Migration can change buffering without a format-change event. */
    device_rate = 96000;
    reported_frames = 480;
    assert(nau_sink_poll() == NAU_OK);
    assert(nau_sink_get_latency(sink) == 10000000);
    assert(nau_sink_start(sink) == NAU_OK);
    wait_callback();
    reported_frames = 4800;
    assert(nau_sink_poll() == NAU_OK);
    assert(nau_sink_get_latency(sink) >= 100000000);

    uint32_t before = opens;
    event(SDL_EVENT_AUDIO_DEVICE_REMOVED, true);
    assert(nau_sink_poll() == NAU_OK && opens == before);
    fail_format = true;
    event(SDL_EVENT_AUDIO_DEVICE_REMOVED, false);
    assert(nau_sink_poll() == NAU_ERROR_IO);
    assert(nau_sink_get_status(sink) == NAU_ERROR_IO);
    const uint32_t stopped_calls = atomic_load(&calls);
    SDL_Delay(30);
    assert(atomic_load(&calls) == stopped_calls);
    assert(nau_sink_poll() == NAU_ERROR_IO && opens == before);
    /* The sink is already started: starting it again neither opens a stream
     * nor clears the error */
    assert(nau_sink_start(sink) == NAU_OK && opens == before);
    assert(nau_sink_get_status(sink) == NAU_ERROR_IO);
    fail_format = false;
    event(SDL_EVENT_AUDIO_DEVICE_ADDED, false);
    assert(nau_sink_poll() == NAU_OK);
    assert(nau_sink_get_status(sink) == NAU_OK && opens == before);
    wait_callback();

    atomic_store(&source_status, NAU_ERROR_IO);
    for (int i = 0; i < 200 && nau_sink_get_status(sink) == NAU_OK; i++)
        SDL_Delay(5);
    assert(nau_sink_get_status(sink) == NAU_ERROR_IO);
    atomic_store(&source_status, NAU_OK);
    fail_format = true;
    assert(nau_sink_poll() == NAU_ERROR_IO);
    fail_format = false;
    assert(nau_sink_poll() == NAU_OK);
    assert(nau_sink_get_status(sink) == NAU_ERROR_IO);
    assert(nau_sink_stop(sink) == NAU_OK);
    fail_open = true;
    assert(nau_sink_start(sink) == NAU_ERROR_IO);
    assert(nau_sink_poll() == NAU_ERROR_IO);
    fail_open = false;
    assert(nau_sink_start(sink) == NAU_OK);
    assert(nau_sink_get_status(sink) == NAU_OK);
    wait_callback();
    assert(nau_sink_stop(sink) == NAU_OK);
    before = opens;
    event(SDL_EVENT_AUDIO_DEVICE_ADDED, false);
    assert(nau_sink_poll() == NAU_OK && opens == before);

    /* Polling preserves the host's event queue. */
    SDL_Event events[32];
    assert(SDL_PeepEvents(events, 32, SDL_PEEKEVENT, SDL_EVENT_AUDIO_DEVICE_REMOVED,
                          SDL_EVENT_AUDIO_DEVICE_REMOVED) >= 2);
    nau_sink_freep(&sink);
    nau_transport_freep(&transport);
    nau_node_unrefp(&node);
    nau_engine_freep(&engine);
    assert(nau_sink_backend_uninit() == NAU_OK);
    assert(nau_sink_poll() == NAU_ERROR_INVALID);
    return 0;
}
