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
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_mutex.h>
/*
 * Only included for SDL_SetMainReady(). main() belongs to the application:
 * SDL_MAIN_HANDLED keeps SDL_main.h from defining it on iOS and Windows.
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_version.h>

#include "nopeaudio/nopeaudio.h"
#include "sdl3.h"
#include "sink/sink_internal.h"
#include "utils/memory.h"
#include "utils/utils.h"
#include "utils/time.h"

/*
 * SDL exposes neither presentation timestamps nor backend buffering.
 * Estimate output latency from:
 *
 * - the audio the stream already holds (see feed()),
 * - DEVICE_BUFFERS device buffers: the one the render is mixed into, and one
 *   being played.
 *
 * What a backend buffers beyond that is not accounted for.
 */
#define DEVICE_BUFFERS 2

/* The device of a sink: SDL's default output, played through a stream */
struct nau_sink_device {
    struct nau_sink *sink;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t period_frames;        /* requested device buffer size */
    struct nau_sink_device *next;  /* in `devices` */
    bool started;
    bool backend_ready;            /* holds a reference from nau_sdl3_acquire() */
    SDL_AudioStream *stream;       /* NULL while the device is closed */
    _Atomic int64_t device_latency; /* nanoseconds: DEVICE_BUFFERS device buffers */
    float *buffer;                 /* feed() renders into it */
    uint32_t buffer_frames;
    _Atomic int64_t latency;       /* nanoseconds: audio held by the stream */
    _Atomic int status;            /* last device error: start, or nau_sink_poll() */
};

/* The device thread must not wait to read or write the atomics */
static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2, "lock-free atomics");

/*
 * State of the audio subsystem, shared by all the sinks:
 *
 * - init_refs counts the calls to nau_sink_backend_init(). Only the main thread uses
 *   it: the first call initializes SDL's audio, the last nau_sink_backend_uninit()
 *   quits it.
 * - device_refs counts backend references held by devices (excluding null
 *   devices). Initialization acquires one, released on failure or destruction,
 *   on any thread. UINT32_MAX means that the subsystem is not initialized:
 *   no device can be initialized. The last nau_sink_backend_uninit() replaces 0
 *   with UINT32_MAX in one atomic step: either a device holds a reference and
 *   it fails with NAU_AGAIN, or device initialization is blocked while SDL quits.
 * - devices lists the devices for nau_sink_poll(). devices_lock protects the
 *   list and the stream of each device, between the main thread polling and
 *   the threads controlling the sinks.
 * - polling is set while nau_sink_poll() runs. Polling pumps SDL's events,
 *   which calls the event filters of the host: they must neither poll again
 *   nor uninitialize the subsystem from there.
 */
static uint32_t init_refs;
static bool polling;
static _Atomic uint32_t device_refs = UINT32_MAX;
static SDL_Mutex *devices_lock;
static struct nau_sink_device *devices;

int nau_sink_backend_init(void)
{
    if (!SDL_IsMainThread())
        return NAU_ERROR_INVALID;

    if (init_refs == UINT32_MAX)
        return NAU_ERROR_INVALID;

    if (init_refs) {
        init_refs++;
        return NAU_OK;
    }
    /*
     * SDL installs SIGINT and SIGTERM handlers when it is first initialized:
     * prevent it, unless the application initialized SDL or set the hint
     */
    if (!SDL_WasInit(0) && !SDL_GetHint(SDL_HINT_NO_SIGNAL_HANDLERS))
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /*
     * main() does not go through SDL_main: SDL has to be told, otherwise it
     * does not initialize on iOS
     */
    SDL_SetMainReady();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_ERROR("SDL: initializing the audio failed: %s", SDL_GetError());
        return NAU_ERROR_IO;
    }
    devices_lock = SDL_CreateMutex();
    if (!devices_lock) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NAU_ERROR_MEMORY;
    }
    init_refs = 1;
    atomic_store_explicit(&device_refs, 0, memory_order_release);
    return NAU_OK;
}

int nau_sink_backend_uninit(void)
{
    if (!SDL_IsMainThread())
        return NAU_ERROR_INVALID;

    if (!init_refs)
        return NAU_ERROR_INVALID;

    if (init_refs > 1) {
        init_refs--;
        return NAU_OK;
    }
    if (polling)
        return NAU_AGAIN;
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&device_refs, &expected, UINT32_MAX,
                                                 memory_order_acq_rel, memory_order_relaxed))
        return NAU_AGAIN;
    SDL_DestroyMutex(devices_lock);
    devices_lock = NULL;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    init_refs = 0;
    return NAU_OK;
}

int nau_sdl3_acquire(void)
{
    uint32_t refs = atomic_load_explicit(&device_refs, memory_order_relaxed);
    do {
        if (refs >= UINT32_MAX - 1)
            return NAU_ERROR_INVALID;
    } while (!atomic_compare_exchange_weak_explicit(&device_refs, &refs, refs + 1,
                                                    memory_order_acquire, memory_order_relaxed));
    return NAU_OK;
}

/*
 * Callback of the stream. SDL calls it from its device thread, with the
 * stream locked, when it needs `additional` more bytes. `total` also counts
 * the bytes the stream already holds.
 */
static void SDLCALL feed(void *user, SDL_AudioStream *stream, int additional, int total)
{
    struct nau_sink_device *d = user;
    const int frame_size = (int)(d->channels * sizeof(float));
    if (additional <= 0)
        return;
    const int64_t now = nau_sink_now();
    /* Frames the stream already holds: they are played before this render */
    int64_t ahead = total > additional ? (total - additional) / frame_size : 0;
    const int64_t device_latency = atomic_load_explicit(&d->device_latency, memory_order_relaxed);
    atomic_store_explicit(&d->latency, ahead * NAU_NS_PER_SEC / d->sample_rate, memory_order_relaxed);
    int64_t left = ((int64_t)additional + frame_size - 1) / frame_size;
    while (left > 0) {
        const uint32_t frames = left < d->buffer_frames ? (uint32_t)left : d->buffer_frames;
        nau_sink_render(d->sink, d->buffer, frames, now + device_latency + ahead * NAU_NS_PER_SEC / d->sample_rate);
        if (!SDL_PutAudioStreamData(stream, d->buffer, (int)frames * frame_size)) {
            nau_sink_set_status(d->sink, NAU_ERROR_MEMORY);
            return;
        }
        left -= frames;
        ahead += frames;
    }
}

/* Query the device format and validate the values used for its latency. */
static bool get_device_format(SDL_AudioDeviceID device, SDL_AudioSpec *spec, int *frames)
{
    if (!SDL_GetAudioDeviceFormat(device, spec, frames))
        return false;

    if (spec->freq <= 0)
        return false;

    if (*frames <= 0)
        return false;

    return true;
}

/*
 * Open the default output. SDL opens it paused: start() resumes it.
 *
 * SDL cannot stop a device: a paused device stays open and plays silence.
 * The stream is therefore destroyed when the sink stops, which closes the
 * device and releases the audio session on iOS. The next start reopens it.
 */
static int open_stream(struct nau_sink_device *d)
{
    /*
     * SDL reads this hint when it opens the device, and again when it opens
     * another one to move the stream to a new default output: the hint is
     * left set
     */
    char frames[16];
    snprintf(frames, sizeof(frames), "%" PRIu32, d->period_frames);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
    const SDL_AudioSpec spec = {
        .format = SDL_AUDIO_F32,
        .channels = (int)d->channels,
        .freq = (int)d->sample_rate,
    };
    /*
     * When the default output changes, SDL moves the stream to the new one
     * from its own thread; nau_sink_poll() then updates the latency. The
     * sound servers follow the default output on their own.
     */
    d->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, d);
    if (!d->stream) {
        LOG_ERROR("SDL: opening the output failed: %s", SDL_GetError());
        return NAU_ERROR_IO;
    }
    const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(d->stream);
    SDL_AudioSpec device_spec = spec;
    int device_frames = 0;
    if (!get_device_format(device, &device_spec, &device_frames)) {
        SDL_DestroyAudioStream(d->stream);
        d->stream = NULL;
        return NAU_ERROR_IO;
    }
    const int64_t device_latency = (int64_t)DEVICE_BUFFERS * device_frames * NAU_NS_PER_SEC / device_spec.freq;
    atomic_store_explicit(&d->device_latency, device_latency, memory_order_relaxed);
    atomic_store_explicit(&d->latency, 0, memory_order_relaxed);
    const int version = SDL_GetVersion();
    const char *name = SDL_GetAudioDeviceName(device);
    LOG_INFO("SDL %d.%d.%d %s: %s output, %d Hz, %d channels, %s, buffers of %d frames, latency estimated %.1f ms",
             SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version),
             SDL_GetCurrentAudioDriver(), name ? name : "default", device_spec.freq, device_spec.channels,
             SDL_GetAudioFormatName(device_spec.format), device_frames, (double)device_latency / 1e6);
    return NAU_OK;
}

/*
 * Destroy the stream, which closes the device. SDL destroys the stream under
 * the lock its device thread holds while mixing: once this function returns,
 * feed() is not running and will not be called again.
 */
static void close_stream(struct nau_sink_device *d)
{
    if (d->stream)
        SDL_DestroyAudioStream(d->stream);
    d->stream = NULL;
}

void nau_sdl3_release(void)
{
    atomic_fetch_sub_explicit(&device_refs, 1, memory_order_release);
}

void nau_sdl3_lock(void)
{
    SDL_LockMutex(devices_lock);
}

void nau_sdl3_unlock(void)
{
    SDL_UnlockMutex(devices_lock);
}

static void device_uninit(struct nau_sink_device *d)
{
    if (d->backend_ready) {
        SDL_LockMutex(devices_lock);
        struct nau_sink_device **entry = &devices;
        while (*entry && *entry != d)
            entry = &(*entry)->next;
        if (*entry)
            *entry = d->next;
        close_stream(d);
        SDL_UnlockMutex(devices_lock);
        nau_sdl3_release();
        d->backend_ready = false;
    }
    nau_freep(&d->buffer);
    d->sink = NULL;
    d->next = NULL;
    d->started = false;
    atomic_store_explicit(&d->device_latency, 0, memory_order_relaxed);
    atomic_store_explicit(&d->latency, 0, memory_order_relaxed);
    atomic_store_explicit(&d->status, NAU_OK, memory_order_relaxed);
}

struct nau_sink_device *nau_sink_device_create(void)
{
    struct nau_sink_device *d = nau_try_calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    atomic_init(&d->device_latency, 0);
    atomic_init(&d->latency, 0);
    atomic_init(&d->status, NAU_OK);
    return d;
}

int nau_sink_device_init(struct nau_sink_device *d, struct nau_sink *sink,
                         const struct nau_engine_config *config, uint32_t period_ms)
{
    /* The SDL library loaded must have the major version of the headers */
    if (SDL_VERSIONNUM_MAJOR(SDL_GetVersion()) != SDL_MAJOR_VERSION)
        return NAU_ERROR_UNSUPPORTED;
    int ret = nau_sdl3_acquire();
    if (ret < 0)
        return ret;
    d->backend_ready = true;
    d->sink = sink;
    d->channels = config->channels;
    d->sample_rate = config->sample_rate;
    /* 10 ms by default: SDL's own default is about 20 ms */
    d->period_frames = nau_sink_period(config->sample_rate, period_ms ? period_ms : 10);
    /*
     * feed() renders through the buffer in several rounds when SDL asks for
     * more frames. SDL copies data of 64 KiB or more to a new allocation:
     * each round stays below.
     */
    d->buffer_frames = NAU_MIN(4096U, (uint32_t)((64 * 1024 - 1) / (d->channels * sizeof(*d->buffer))));
    d->buffer = nau_try_calloc(d->buffer_frames, d->channels * sizeof(*d->buffer));
    if (!d->buffer) {
        device_uninit(d);
        return NAU_ERROR_MEMORY;
    }
    /* Open the output now so that sink initialization fails if there is none. */
    SDL_LockMutex(devices_lock);
    ret = open_stream(d);
    if (ret == NAU_OK) {
        d->next = devices;
        devices = d;
    }
    SDL_UnlockMutex(devices_lock);
    if (ret < 0) {
        device_uninit(d);
        return ret;
    }
    return NAU_OK;
}

static int start(struct nau_sink_device *d)
{
    nau_sink_reset_status(d->sink);
    if (!d->stream) {
        int ret = open_stream(d);
        if (ret < 0)
            return ret;
    }
    if (!SDL_ResumeAudioStreamDevice(d->stream)) {
        LOG_ERROR("SDL: starting the output failed: %s", SDL_GetError());
        close_stream(d);
        return NAU_ERROR_IO;
    }
    d->started = true;
    return NAU_OK;
}

int nau_sink_device_start(struct nau_sink_device *d)
{
    SDL_LockMutex(devices_lock);
    int ret = NAU_OK;
    if (!d->started) {
        ret = start(d);
        atomic_store_explicit(&d->status, ret, memory_order_relaxed);
    }
    SDL_UnlockMutex(devices_lock);
    return ret;
}

int nau_sink_device_stop(struct nau_sink_device *d)
{
    SDL_LockMutex(devices_lock);
    d->started = false;
    atomic_store_explicit(&d->status, NAU_OK, memory_order_relaxed);
    close_stream(d);
    SDL_UnlockMutex(devices_lock);
    return NAU_OK;
}

int nau_sink_poll(void)
{
    if (!SDL_IsMainThread())
        return NAU_ERROR_INVALID;

    if (!init_refs)
        return NAU_ERROR_INVALID;

    if (polling)
        return NAU_ERROR_INVALID;

    polling = true;
    /*
     * Pumped without devices_lock: the event filters of the host run from
     * here, and may start, stop or free sinks
     */
    SDL_PumpEvents();
    int result = NAU_OK;
    SDL_LockMutex(devices_lock);
    for (struct nau_sink_device *d = devices; d; d = d->next) {
        int ret = atomic_load_explicit(&d->status, memory_order_relaxed);
        if (d->stream) {
            /*
             * Query the stream's device to detect availability changes:
             *
             * - Success: update latency and resume a started sink if the
             *   device has recovered.
             * - Failure: pause and clear queued audio so playback does not
             *   advance unheard. Keep the stream so SDL can move it to another
             *   output, and report the error until the device recovers.
             *
             * SDL can still report a lost default output as available. In
             * that case playback continues silently until SDL moves the
             * stream to a new default output or the sink stops.
             */
            SDL_AudioSpec spec;
            int frames;
            const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(d->stream);
            if (!get_device_format(device, &spec, &frames)) {
                /*
                 * Only once, when the output becomes unavailable. A stream
                 * which cannot be paused and emptied is closed: the sink
                 * then has to be stopped and started again.
                 */
                if (ret == NAU_OK) {
                    bool paused = SDL_PauseAudioStreamDevice(d->stream);
                    if (paused)
                        paused = SDL_ClearAudioStream(d->stream);
                    if (!paused)
                        close_stream(d);
                }
                ret = NAU_ERROR_IO;
            } else {
                const int64_t latency = (int64_t)DEVICE_BUFFERS * frames * NAU_NS_PER_SEC / spec.freq;
                atomic_store_explicit(&d->device_latency, latency, memory_order_relaxed);
                bool resumed = true;
                if (ret != NAU_OK && d->started)
                    resumed = SDL_ResumeAudioStreamDevice(d->stream);
                ret = NAU_OK;
                if (!resumed) {
                    close_stream(d);
                    ret = NAU_ERROR_IO;
                }
            }
        }
        if (ret != NAU_OK)
            result = ret;
        atomic_store_explicit(&d->status, ret, memory_order_relaxed);
    }
    SDL_UnlockMutex(devices_lock);
    polling = false;
    return result;
}

int64_t nau_sink_device_get_latency(const struct nau_sink_device *d)
{
    return atomic_load_explicit(&d->latency, memory_order_relaxed) +
           atomic_load_explicit(&d->device_latency, memory_order_relaxed);
}

int nau_sink_device_get_status(const struct nau_sink_device *d)
{
    return atomic_load_explicit(&d->status, memory_order_relaxed);
}

void nau_sink_device_freep(struct nau_sink_device **dp)
{
    if (!dp || !*dp)
        return;
    device_uninit(*dp);
    nau_freep(dp);
}
