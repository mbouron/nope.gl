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

#ifndef NAU_SINK_INTERNAL_H
#define NAU_SINK_INTERNAL_H

#include <stdint.h>

#include "nopeaudio/nopeaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sink.c implements the API and rendering shared by all sinks. The backend
 * (SDL3, Oboe or null) manages the device, its latency and its events.
 * sink.c validates public arguments before calling the backend.
 *
 *    nau_sink_start()...  --->  sink.c  --->  nau_sink_device_start()...
 *                                                       |
 *                                                    backend
 *                                                       |
 *    nau_transport_render()  <---  sink.c  <---  nau_sink_render()
 *                                               (device thread)
 */

/* Number of frames of a period of `period_ms` milliseconds, within [1, 65536] */
static inline uint32_t nau_sink_period(uint32_t sample_rate, uint32_t period_ms)
{
    const uint64_t frames = (uint64_t)sample_rate * period_ms / 1000;
    return frames ? (frames < 65536 ? (uint32_t)frames : 65536) : 1;
}

/*
 * Implemented by sink.c, for the backends
 */

/* Render `frames` frames, heard at `time`: the device thread calls it */
void nau_sink_render(struct nau_sink *sink, float *output, uint32_t frames, int64_t time);

/* Reset underruns and rendering errors when the device starts. */
void nau_sink_reset_status(struct nau_sink *sink);

/* Report a device-thread failure through the sink status. */
void nau_sink_set_status(struct nau_sink *sink, int status);

/*
 * Implemented by sink_null.c in every build. The null device owns a thread
 * rendering in real time with periods of `period_ms` milliseconds (10 by
 * default). The null backend uses it too. Starting it again has no effect;
 * stopping or freeing it joins its thread.
 */
struct nau_sink_null;

struct nau_sink_null *nau_sink_null_create(void);
int nau_sink_null_init(struct nau_sink_null *null_device, struct nau_sink *sink,
                       const struct nau_engine_config *config, uint32_t period_ms);
int nau_sink_null_start(struct nau_sink_null *null_device);
void nau_sink_null_stop(struct nau_sink_null *null_device);
int64_t nau_sink_null_get_latency(const struct nau_sink_null *null_device);
void nau_sink_null_freep(struct nau_sink_null **null_device);

/*
 * Implemented by the backends, along with nau_sink_backend_init(),
 * nau_sink_backend_uninit() and nau_sink_poll()
 */

struct nau_sink_device;

/* Allocate a device without acquiring backend resources. */
struct nau_sink_device *nau_sink_device_create(void);

/*
 * Open the output for the format of `config`, with buffers of `period_ms`
 * milliseconds (0 for the default of the backend). Both device initializers
 * require uninitialized objects and a valid engine configuration. Failed
 * initialization can be retried; freep also accepts an uninitialized device.
 */
int nau_sink_device_init(struct nau_sink_device *device, struct nau_sink *sink,
                         const struct nau_engine_config *config, uint32_t period_ms);

/* Starting an already running device has no effect. */
int nau_sink_device_start(struct nau_sink_device *device);

/* When it returns, the device thread is not rendering anymore */
int nau_sink_device_stop(struct nau_sink_device *device);

int64_t nau_sink_device_get_latency(const struct nau_sink_device *device);

/* NAU_OK, or the error which makes the device unavailable */
int nau_sink_device_get_status(const struct nau_sink_device *device);

void nau_sink_device_freep(struct nau_sink_device **device);

#ifdef __cplusplus
}
#endif

#endif
