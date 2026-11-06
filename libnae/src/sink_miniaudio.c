/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L  /* clock_gettime() */
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <miniaudio/miniaudio.h>
#include "nae/miniaudio.h"

struct nae_sink {
    struct nae_engine *engine;
    struct nae_transport *transport;
    int64_t latency;              /* nanoseconds */
    ma_context context;
    ma_device device;
    bool context_ready, device_ready, started;
    int64_t cursor;               /* Callback, or control while stopped. */
    _Atomic uint32_t underruns;
    _Atomic int status;
};

static void data_callback(ma_device *device, void *output, const void *input, ma_uint32 frames)
{
    (void)input;
    struct nae_sink *s = device->pUserData;
    const struct nae_config *c = nae_engine_get_config(s->engine);
    if (s->transport) {
        const int ret = nae_transport_render(s->transport, nae_sink_now() + s->latency, frames, output);
        if (ret != NAE_OK) {
            uint32_t n = atomic_load_explicit(&s->underruns, memory_order_relaxed);
            atomic_store_explicit(&s->underruns, n + 1, memory_order_relaxed);
            if (ret < 0)
                atomic_store_explicit(&s->status, ret, memory_order_relaxed);
        }
        float *pcm = output;
        for (size_t i = 0; i < (size_t)frames * c->channels; i++)
            pcm[i] = isfinite(pcm[i]) ? fmaxf(-1.f, fminf(1.f, pcm[i])) : 0.f;
        return;
    }
    int ret = nae_engine_render(s->engine, s->cursor, frames, output);
    if (ret != NAE_OK) {
        /* This counter has a single writer, so no retrying RMW is needed. */
        uint32_t n = atomic_load_explicit(&s->underruns, memory_order_relaxed);
        atomic_store_explicit(&s->underruns, n + 1, memory_order_relaxed);
        if (ret < 0)
            atomic_store_explicit(&s->status, ret, memory_order_relaxed);
    }
    if (s->cursor <= INT64_MAX - frames)
        s->cursor += frames;
    else
        memset(output, 0, (size_t)frames * c->channels * sizeof(float));
    /* Internal mixes retain headroom; only the device boundary is clamped. */
    float *pcm = output;
    for (size_t i = 0; i < (size_t)frames * c->channels; i++)
        pcm[i] = isfinite(pcm[i]) ? fmaxf(-1.f, fminf(1.f, pcm[i])) : 0.f;
}

static void notification_callback(const ma_device_notification *notification)
{
    struct nae_sink *s = notification->pDevice->pUserData;
    if (notification->type == ma_device_notification_type_stopped)
        atomic_store_explicit(&s->status, NAE_AGAIN, memory_order_relaxed);
}

int nae_sink_create(struct nae_engine *engine, const struct nae_sink_config *config, struct nae_sink **sink)
{
    if (!sink)
        return NAE_ERROR_INVALID;
    *sink = NULL;
    const struct nae_config *c = nae_engine_get_config(engine);
    if (!c)
        return NAE_ERROR_INVALID;
    /* Refuse mismatched shared-library/header versions before passing structs. */
    if (strcmp(ma_version_string(), MA_VERSION_STRING))
        return NAE_ERROR_UNSUPPORTED;
    struct nae_sink *s = calloc(1, sizeof(*s));
    if (!s)
        return NAE_ERROR_MEMORY;
    s->engine = engine;
    s->transport = config ? config->transport : NULL;
    atomic_init(&s->underruns, 0);
    atomic_init(&s->status, NAE_OK);
    if (!atomic_is_lock_free(&s->underruns) || !atomic_is_lock_free(&s->status)) {
        free(s);
        return NAE_ERROR_UNSUPPORTED;
    }
    const ma_backend null_backend = ma_backend_null;
    const bool use_null = config && config->null_device;
    if (ma_context_init(use_null ? &null_backend : NULL, use_null ? 1 : 0, NULL, &s->context) != MA_SUCCESS)
        goto fail;
    s->context_ready = true;
    ma_device_config d = ma_device_config_init(ma_device_type_playback);
    d.playback.format = ma_format_f32;
    d.playback.channels = c->channels;
    d.sampleRate = c->sample_rate;
    d.performanceProfile = ma_performance_profile_low_latency;
    if (config && config->period_ms)
        d.periodSizeInMilliseconds = config->period_ms;
    d.aaudio.usage = ma_aaudio_usage_media;
    d.aaudio.contentType = ma_aaudio_content_type_movie;
    d.dataCallback = data_callback;
    d.notificationCallback = notification_callback;
    d.pUserData = s;
    if (ma_device_init(&s->context, &d, &s->device) != MA_SUCCESS)
        goto fail;
    s->device_ready = true;
    /* The buffering of the device: what the backends tell of their latency */
    const ma_uint32 rate = s->device.playback.internalSampleRate ? s->device.playback.internalSampleRate : c->sample_rate;
    s->latency = (int64_t)s->device.playback.internalPeriodSizeInFrames * s->device.playback.internalPeriods *
                 1000000000 / rate;
    *sink = s;
    return NAE_OK;
fail:
    nae_sink_freep(&s);
    return NAE_ERROR_IO;
}

int nae_sink_start(struct nae_sink *s)
{
    if (!s)
        return NAE_ERROR_INVALID;
    if (s->started && ma_device_is_started(&s->device))
        return NAE_OK;
    atomic_store_explicit(&s->underruns, 0, memory_order_relaxed);
    atomic_store_explicit(&s->status, NAE_OK, memory_order_relaxed);
    if (ma_device_start(&s->device) != MA_SUCCESS)
        return NAE_ERROR_IO;
    s->started = true;
    return NAE_OK;
}

int nae_sink_stop(struct nae_sink *s)
{
    if (!s)
        return NAE_ERROR_INVALID;
    if (ma_device_stop(&s->device) != MA_SUCCESS)
        return NAE_ERROR_IO;
    s->started = false;
    return NAE_OK;
}

int nae_sink_seek(struct nae_sink *s, int64_t frame)
{
    if (!s || s->started || s->transport || frame < 0)
        return NAE_ERROR_INVALID;
    s->cursor = frame;
    return NAE_OK;
}

uint32_t nae_sink_underruns(const struct nae_sink *s)
{
    return s ? atomic_load_explicit(&s->underruns, memory_order_relaxed) : 0;
}

int nae_sink_status(const struct nae_sink *s)
{
    return s ? atomic_load_explicit(&s->status, memory_order_relaxed) : NAE_ERROR_INVALID;
}

void nae_sink_freep(struct nae_sink **sp)
{
    if (!sp || !*sp)
        return;
    struct nae_sink *s = *sp;
    if (s->device_ready)
        ma_device_uninit(&s->device);
    if (s->context_ready)
        ma_context_uninit(&s->context);
    free(s);
    *sp = NULL;
}

int64_t nae_sink_latency(const struct nae_sink *s)
{
    return s ? s->latency : 0;
}

int64_t nae_sink_now(void)
{
#if defined(_WIN32)
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (int64_t)((double)count.QuadPart * 1e9 / (double)frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}
