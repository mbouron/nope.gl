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

#define _GNU_SOURCE  /* clock_gettime() with glibc */
#include <assert.h>
#include <math.h>
#include <stdatomic.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#include "nopeaudio/nopeaudio.h"
#include "sink/sink_internal.h"
#include "utils/memory.h"
#include "utils/time.h"

/* Shared sink API and rendering, independent of the device (see sink_internal.h). */
struct nau_sink {
    struct nau_transport *transport;
    uint32_t channels;
    _Atomic uint32_t underruns;
    _Atomic int status;             /* last rendering error */
    struct nau_sink_device *device;
    struct nau_sink_null *null_device;
};

/* The device thread must not wait to read or write the atomics */
static_assert(ATOMIC_INT_LOCK_FREE == 2, "lock-free atomics");

void nau_sink_render(struct nau_sink *s, float *output, uint32_t frames, int64_t time)
{
    int ret = nau_transport_render(s->transport, time, frames, output);
    if (ret != NAU_OK) {
        /* Only this thread writes the counter: no atomic increment needed */
        const uint32_t n = atomic_load_explicit(&s->underruns, memory_order_relaxed);
        atomic_store_explicit(&s->underruns, n + 1, memory_order_relaxed);
        if (ret < 0)
            atomic_store_explicit(&s->status, ret, memory_order_relaxed);
    }
    /*
     * The graph is rendered without clamping, to keep its headroom. The
     * samples are clamped here, where they leave for the device.
     */
    for (size_t i = 0; i < (size_t)frames * s->channels; i++)
        output[i] = isfinite(output[i]) ? fmaxf(-1.f, fminf(1.f, output[i])) : 0.f;
}

void nau_sink_reset_status(struct nau_sink *s)
{
    atomic_store_explicit(&s->underruns, 0, memory_order_relaxed);
    atomic_store_explicit(&s->status, NAU_OK, memory_order_relaxed);
}

void nau_sink_set_status(struct nau_sink *s, int status)
{
    atomic_store_explicit(&s->status, status, memory_order_relaxed);
}

struct nau_sink *nau_sink_create(void)
{
    struct nau_sink *s = nau_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    atomic_init(&s->underruns, 0);
    atomic_init(&s->status, NAU_OK);
    return s;
}

int nau_sink_init(struct nau_sink *s, struct nau_transport *transport, const struct nau_sink_config *config)
{
    if (!s || s->transport)
        return NAU_ERROR_INVALID;

    if (!transport)
        return NAU_ERROR_INVALID;

    const struct nau_engine_config *c = nau_engine_get_config(nau_transport_get_engine(transport));
    if (!c)
        return NAU_ERROR_INVALID;

    const uint32_t period_ms = config ? config->period_ms : 0;
    struct nau_sink_device *device = NULL;
    struct nau_sink_null *null_device = NULL;
    int ret;
    if (config && config->null_device) {
        null_device = nau_sink_null_create();
        if (!null_device)
            return NAU_ERROR_MEMORY;
        ret = nau_sink_null_init(null_device, s, c, period_ms);
    } else {
        device = nau_sink_device_create();
        if (!device)
            return NAU_ERROR_MEMORY;
        ret = nau_sink_device_init(device, s, c, period_ms);
    }
    if (ret < 0) {
        nau_sink_device_freep(&device);
        nau_sink_null_freep(&null_device);
        return ret;
    }

    s->transport = transport;
    s->channels = c->channels;
    s->device = device;
    s->null_device = null_device;
    return NAU_OK;
}

int nau_sink_start(struct nau_sink *s)
{
    if (!s || !s->transport)
        return NAU_ERROR_INVALID;
    return s->device ? nau_sink_device_start(s->device) : nau_sink_null_start(s->null_device);
}

int nau_sink_stop(struct nau_sink *s)
{
    if (!s || !s->transport)
        return NAU_ERROR_INVALID;
    if (s->device)
        return nau_sink_device_stop(s->device);
    nau_sink_null_stop(s->null_device);
    return NAU_OK;
}

int64_t nau_sink_get_latency(const struct nau_sink *s)
{
    if (!s || !s->transport)
        return 0;
    return s->device ? nau_sink_device_get_latency(s->device) : nau_sink_null_get_latency(s->null_device);
}

uint32_t nau_sink_get_underruns(const struct nau_sink *s)
{
    return s ? atomic_load_explicit(&s->underruns, memory_order_relaxed) : 0;
}

/* An error of the device comes first: nothing is heard while it lasts */
int nau_sink_get_status(const struct nau_sink *s)
{
    if (!s || !s->transport)
        return NAU_ERROR_INVALID;
    if (s->device) {
        int ret = nau_sink_device_get_status(s->device);
        if (ret != NAU_OK)
            return ret;
    }
    return atomic_load_explicit(&s->status, memory_order_relaxed);
}

void nau_sink_freep(struct nau_sink **sp)
{
    if (!sp || !*sp)
        return;
    struct nau_sink *s = *sp;
    nau_sink_device_freep(&s->device);
    nau_sink_null_freep(&s->null_device);
    nau_freep(sp);
}

int64_t nau_sink_now(void)
{
#if defined(_WIN32)
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (int64_t)((double)count.QuadPart * NAU_NS_PER_SEC / (double)frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * NAU_NS_PER_SEC + ts.tv_nsec;
#endif
}
