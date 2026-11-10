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

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "nopeaudio/nopeaudio.h"
#include "sink/sink_internal.h"
#include "utils/memory.h"
#include "utils/pthread_compat.h"
#include "utils/semaphore.h"
#include "utils/thread.h"
#include "utils/time.h"

/* A null device renders in real time without audio output. Available in every build. */
struct nau_sink_null {
    struct nau_sink *sink;
    uint32_t sample_rate;
    uint32_t period_frames;
    float *buffer;                 /* one period */
    pthread_t thread;
    nau_sem wake;
    bool wake_ready;
    _Atomic bool running;
    bool started;
};

/* Pace rendering like an audio device. */
static void *run_null(void *arg)
{
    struct nau_sink_null *s = arg;
    nau_thread_set_name("nau/null");
    const int64_t period = nau_sink_null_get_latency(s);
    int64_t next = nau_sink_now();
    while (atomic_load_explicit(&s->running, memory_order_relaxed)) {
        nau_sink_render(s->sink, s->buffer, s->period_frames, nau_sink_now() + period);
        next += period;
        const int64_t now = nau_sink_now();
        if (next > now)
            nau_sem_timedwait(&s->wake, next - now);
        else if (now - next > period)
            next = now; /* late: skip the missed periods instead of rendering them in a burst */
    }
    return NULL;
}

struct nau_sink_null *nau_sink_null_create(void)
{
    struct nau_sink_null *s = nau_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    atomic_init(&s->running, false);
    return s;
}

int nau_sink_null_init(struct nau_sink_null *s, struct nau_sink *sink,
                       const struct nau_engine_config *config, uint32_t period_ms)
{
    s->sample_rate = config->sample_rate;
    s->period_frames = nau_sink_period(s->sample_rate, period_ms ? period_ms : 10);
    s->buffer = nau_try_calloc(s->period_frames, config->channels * sizeof(*s->buffer));
    if (!s->buffer)
        return NAU_ERROR_MEMORY;
    int ret = nau_sem_init(&s->wake);
    if (ret < 0) {
        nau_freep(&s->buffer);
        return ret;
    }
    s->wake_ready = true;
    s->sink = sink;
    return NAU_OK;
}

int nau_sink_null_start(struct nau_sink_null *s)
{
    if (s->started)
        return NAU_OK;
    nau_sink_reset_status(s->sink);
    /* Drop the wake-up a previous stop may have left. */
    for (;;) {
        const bool woken = nau_sem_timedwait(&s->wake, 0);
        if (!woken)
            break;
    }
    atomic_store_explicit(&s->running, true, memory_order_relaxed);
    int ret = pthread_create(&s->thread, NULL, run_null, s);
    if (ret != 0) {
        atomic_store_explicit(&s->running, false, memory_order_relaxed);
        return NAU_ERROR_MEMORY;
    }
    s->started = true;
    return NAU_OK;
}

void nau_sink_null_stop(struct nau_sink_null *s)
{
    if (!s->started)
        return;
    atomic_store_explicit(&s->running, false, memory_order_relaxed);
    nau_sem_post(&s->wake);
    pthread_join(s->thread, NULL);
    s->started = false;
}

/* Simulate one period of output latency. */
int64_t nau_sink_null_get_latency(const struct nau_sink_null *s)
{
    return (int64_t)s->period_frames * NAU_NS_PER_SEC / s->sample_rate;
}

void nau_sink_null_freep(struct nau_sink_null **sp)
{
    if (!sp)
        return;
    struct nau_sink_null *s = *sp;
    if (!s)
        return;
    nau_sink_null_stop(s);
    if (s->wake_ready)
        nau_sem_destroy(&s->wake);
    nau_freep(&s->buffer);
    nau_freep(sp);
}

#if NAU_DEFAULT_SINK_NULL

/* The default backend in builds without SDL3 or Oboe uses the same null device. */
struct nau_sink_device {
    struct nau_sink_null *null_device;
};

int nau_sink_backend_init(void)
{
    return NAU_OK;
}

int nau_sink_backend_uninit(void)
{
    return NAU_OK;
}

int nau_sink_poll(void)
{
    return NAU_OK;
}

struct nau_sink_device *nau_sink_device_create(void)
{
    return nau_try_calloc(1, sizeof(struct nau_sink_device));
}

int nau_sink_device_init(struct nau_sink_device *d, struct nau_sink *sink,
                         const struct nau_engine_config *config, uint32_t period_ms)
{
    struct nau_sink_null *s = nau_sink_null_create();
    if (!s)
        return NAU_ERROR_MEMORY;
    int ret = nau_sink_null_init(s, sink, config, period_ms);
    if (ret < 0) {
        nau_sink_null_freep(&s);
        return ret;
    }
    d->null_device = s;
    return NAU_OK;
}

int nau_sink_device_start(struct nau_sink_device *d)
{
    return nau_sink_null_start(d->null_device);
}

int nau_sink_device_stop(struct nau_sink_device *d)
{
    nau_sink_null_stop(d->null_device);
    return NAU_OK;
}

int64_t nau_sink_device_get_latency(const struct nau_sink_device *d)
{
    return nau_sink_null_get_latency(d->null_device);
}

int nau_sink_device_get_status(const struct nau_sink_device *d)
{
    return NAU_OK;
}

void nau_sink_device_freep(struct nau_sink_device **dp)
{
    if (!dp)
        return;
    struct nau_sink_device *d = *dp;
    if (!d)
        return;
    nau_sink_null_freep(&d->null_device);
    nau_freep(dp);
}

#endif
