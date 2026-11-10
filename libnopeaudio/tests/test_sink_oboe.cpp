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

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

#include <oboe/Oboe.h>
#include "nopeaudio/nopeaudio.h"

using namespace std::chrono_literals;

struct renderer {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<uint32_t> calls{0};
};

static int process(void *user, const nau_block *)
{
    auto *r = static_cast<renderer *>(user);
    r->calls.fetch_add(1);
    r->entered.store(true);
    while (!r->release.load())
        std::this_thread::yield();
    return NAU_OK;
}

static void wait_for(const std::atomic<bool> &flag)
{
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!flag.load() && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
    assert(flag.load());
}

static void callback(const std::shared_ptr<oboe::AudioStream> &stream, bool accepted)
{
    float output[32];
    for (auto &sample : output)
        sample = 1.f;
    const auto ret = stream->data->onAudioReady(stream.get(), output, 16);
    assert(ret == oboe::DataCallbackResult::Continue);
    if (!accepted) {
        for (const auto sample : output)
            assert(sample == 0.f);
    }
}

static void test_failure(bool starting)
{
    nau_engine *engine = nullptr;
    nau_node *node = nullptr;
    nau_transport *transport = nullptr;
    nau_sink *sink = nullptr;
    const nau_engine_config config = {48000, 2, 128};
    engine = nau_engine_create();
    assert(engine);
    assert(nau_engine_init(engine, &config) == NAU_OK);
    renderer r;
    nau_processor processor = {};
    processor.user = &r;
    processor.process = process;
    node = nau_node_create();
    assert(node);
    assert(nau_node_init(node, engine, &processor) == NAU_OK);
    const nau_route route = {node, nullptr};
    assert(nau_engine_commit(engine, &route, 1, 0) == NAU_OK);
    const nau_transport_config transport_config = {};
    transport = nau_transport_create();
    assert(transport);
    assert(nau_transport_init(transport, engine, &transport_config) == NAU_OK);
    assert(nau_transport_set_duration(transport, 48000 * 60) == NAU_OK);
    nau_transport_play(transport);
    sink = nau_sink_create();
    assert(sink);
    assert(nau_sink_init(sink, transport, nullptr) == NAU_OK);
    auto old = oboe::last_stream;
    std::thread render_thread;
    if (starting) {
        old->start_hook = [&](oboe::AudioStream *) {
            render_thread = std::thread([&] { callback(old, true); });
            wait_for(r.entered);
            return oboe::Result::ErrorTimeout;
        };
    } else {
        assert(nau_sink_start(sink) == NAU_OK);
        old->stop_result = oboe::Result::ErrorTimeout;
        render_thread = std::thread([&] { callback(old, true); });
        wait_for(r.entered);
    }
    std::atomic<bool> returned{false};
    std::thread control([&] {
        assert((starting ? nau_sink_start(sink) : nau_sink_stop(sink)) == NAU_ERROR_IO);
        returned.store(true);
    });
    wait_for(r.entered);
    std::this_thread::sleep_for(20ms);
    assert(!returned.load());
    r.release.store(true);
    control.join();
    render_thread.join();
    assert(old->closed && r.calls.load() == 1);
    if (starting)
        assert(nau_sink_get_status(sink) == NAU_ERROR_IO);
    callback(old, false);
    assert(nau_sink_start(sink) == NAU_OK);
    assert(nau_sink_get_status(sink) == NAU_OK);
    assert(oboe::last_stream != old);
    callback(old, false);
    callback(oboe::last_stream, true);
    assert(r.calls.load() == 2);
    const auto current = oboe::last_stream;
    assert(nau_sink_start(sink) == NAU_OK && oboe::last_stream == current);
    std::thread late([&] {
        for (int i = 0; i < 1000; i++)
            callback(old, false);
    });
    for (int i = 0; i < 1000; i++)
        callback(current, true);
    late.join();
    assert(r.calls.load() == 1002);
    assert(nau_sink_stop(sink) == NAU_OK);

    /* Reopening a disconnected, stopped stream does not start it or report failure. */
    auto stopped = oboe::last_stream;
    stopped->error->onErrorAfterClose(stopped.get(), oboe::Result::ErrorDisconnected);
    assert(nau_sink_get_status(sink) == NAU_OK);
    callback(oboe::last_stream, false);
    nau_sink_freep(&sink);
    nau_transport_freep(&transport);
    nau_node_unrefp(&node);
    nau_engine_freep(&engine);
    callback(old, false);
    callback(oboe::last_stream, false);
    oboe::last_stream.reset();
}

int main()
{
    test_failure(false);
    test_failure(true);
    return 0;
}
