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

#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include "nopeaudio/nopeaudio.h"

#define MAX_TRACKS 32

static void wait_ms(uint32_t ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    const struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

static volatile sig_atomic_t interrupted;
static void interrupt_handler(int signal_number)
{
    interrupted = 1;
}

/*
 * Automate a low-pass filter so that its track is heard under water from
 * `dive` to `surface`, in seconds. The cutoff goes down to 500 Hz in 0.3 s,
 * with a little resonance, and the filter opens again in 1.5 s.
 */
static int underwater(struct nau_node *lowpass, double dive, double surface, uint32_t rate)
{
    const int64_t in = (int64_t)(dive * rate), out = (int64_t)(surface * rate);
    const struct nau_ramp cutoff[] = {
        {.frame = in, .length = rate * 3 / 10, .value = 500.f},
        {.frame = out, .length = rate * 3 / 2, .value = NAU_LOWPASS_OPEN},
    };
    const struct nau_ramp resonance[] = {
        {.frame = in, .length = rate * 3 / 10, .value = 1.4f},
        {.frame = out, .length = rate * 3 / 2, .value = NAU_LOWPASS_FLAT},
    };
    int ret = nau_effect_set_automation(lowpass, NAU_LOWPASS_CUTOFF, cutoff, 2);
    if (ret < 0)
        return ret;
    return nau_effect_set_automation(lowpass, NAU_LOWPASS_RESONANCE, resonance, 2);
}

int main(int argc, char **argv)
{
    int arg = 1;
    struct nau_sink_config sink_config = {0};
    double dive = -1., surface = -1.;
    for (; arg < argc && !strncmp(argv[arg], "--", 2); arg++) {
        if (!strcmp(argv[arg], "--null")) {
            sink_config.null_device = 1;
        } else if (!strcmp(argv[arg], "--underwater") && arg + 2 < argc) {
            dive = atof(argv[++arg]);
            surface = atof(argv[++arg]);
        } else {
            break;
        }
    }
    const int count = argc - arg;
    if (count < 1 || count > MAX_TRACKS || (dive >= 0. && surface < dive)) {
        fprintf(stderr, "Usage: %s [--null] [--underwater IN OUT] audio-file [audio-file ...] (up to %d tracks)\n"
                        "  --underwater IN OUT: the first track heard under water from IN to OUT seconds\n",
                argv[0], MAX_TRACKS);
        return 1;
    }
    const int initialized = sink_config.null_device ? NAU_OK : nau_sink_backend_init();
    if (initialized != NAU_OK)
        return 1;
    struct nau_engine *engine = NULL;
    struct nau_sink *sink = NULL;
    struct nau_transport *transport = NULL;
    struct nau_media *media[MAX_TRACKS] = {0};
    /* Nodes of the graph: for each track, its source, its filter (first
     * track only, with --underwater) and its gain; then the mix and the
     * master gain */
    struct nau_node *nodes[2 * MAX_TRACKS + 3] = {0};
    struct nau_route routes[2 * MAX_TRACKS + 3] = {0};
    uint32_t inputs[MAX_TRACKS + 2], mix_inputs[MAX_TRACKS];
    uint32_t node_count = 0, input_count = 0;
    int64_t length = 0;
    const struct nau_engine_config config = {48000, 2, 256};
    int ret;
    engine = nau_engine_create();
    if (!engine) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }
    ret = nau_engine_init(engine, &config);
    if (ret < 0)
        goto end;
    for (int i = 0; i < 2 * count + 2 + (dive >= 0.); i++) {
        nodes[i] = nau_node_create();
        if (!nodes[i]) {
            ret = NAU_ERROR_MEMORY;
            goto end;
        }
    }
    for (int i = 0; i < count; i++) {
        media[i] = nau_media_create();
        if (!media[i]) {
            ret = NAU_ERROR_MEMORY;
            goto end;
        }
        ret = nau_media_init(media[i], engine, argv[arg + i], NULL);
        if (ret < 0)
            goto end;
        ret = nau_media_init_source(media[i], nodes[node_count], 0, 0, INT64_MAX);
        if (ret < 0)
            goto end;
        routes[node_count] = (struct nau_route){nodes[node_count], NULL};
        node_count++;
        if (!i && dive >= 0.) {
            ret = nau_effect_init(nodes[node_count], engine, NAU_EFFECT_LOWPASS, 0);
            if (ret < 0)
                goto end;
            ret = underwater(nodes[node_count], dive, surface, config.sample_rate);
            if (ret < 0)
                goto end;
            inputs[input_count] = node_count - 1;
            routes[node_count] = (struct nau_route){nodes[node_count], &inputs[input_count++]};
            node_count++;
        }
        ret = nau_gain_init(nodes[node_count], engine, 1.f / (float)count, 240);
        if (ret < 0)
            goto end;
        inputs[input_count] = node_count - 1;
        routes[node_count] = (struct nau_route){nodes[node_count], &inputs[input_count++]};
        mix_inputs[i] = node_count++;
        if (nau_media_get_length(media[i]) > length)
            length = nau_media_get_length(media[i]);
    }
    ret = nau_mix_init(nodes[node_count], engine, (uint32_t)count);
    if (ret < 0)
        goto end;
    routes[node_count] = (struct nau_route){nodes[node_count], mix_inputs};
    node_count++;
    ret = nau_gain_init(nodes[node_count], engine, 1, 240);
    if (ret < 0)
        goto end;
    inputs[input_count] = node_count - 1;
    routes[node_count] = (struct nau_route){nodes[node_count], &inputs[input_count++]};
    node_count++;
    ret = nau_engine_commit(engine, routes, node_count, node_count - 1);
    if (ret != NAU_OK)
        goto end;
    const struct nau_transport_config transport_config = {
        .declick_frames       = config.sample_rate / 200,
        .ready_timeout_frames = config.sample_rate / 4,
    };
    transport = nau_transport_create();
    if (!transport) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }
    ret = nau_transport_init(transport, engine, &transport_config);
    if (ret < 0)
        goto end;
    ret = nau_transport_set_duration(transport, length);
    if (ret < 0)
        goto end;
    sink = nau_sink_create();
    if (!sink) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }
    ret = nau_sink_init(sink, transport, &sink_config);
    if (ret < 0)
        goto end;
    signal(SIGINT, interrupt_handler);
    ret = nau_sink_start(sink);
    if (ret < 0)
        goto end;
    nau_transport_play(transport);
    /* The device renders from its own thread. Meanwhile, handle the device
     * events until the transport stops, which it does at the end of the
     * project. */
    while (!interrupted && nau_transport_is_playing(transport)) {
        if (!sink_config.null_device)
            nau_sink_poll();
        if (nau_sink_get_status(sink) != NAU_OK) {
            ret = NAU_ERROR_IO;
            goto end;
        }
        nau_engine_collect(engine);
        wait_ms(10);
    }
    fprintf(stderr, "source underrun callbacks: %u\n", nau_sink_get_underruns(sink));
    ret = NAU_OK;
end:
    nau_sink_freep(&sink);
    if (!sink_config.null_device)
        nau_sink_backend_uninit();
    nau_transport_freep(&transport);
    for (uint32_t i = 0; i < sizeof(nodes) / sizeof(*nodes); i++)
        nau_node_unrefp(&nodes[i]);
    nau_engine_freep(&engine);
    for (uint32_t i = 0; i < MAX_TRACKS; i++)
        nau_media_unrefp(&media[i]);
    if (ret < 0)
        fprintf(stderr, "nopeaudio-play failed: %d\n", ret);
    return ret < 0 ? 1 : 0;
}
