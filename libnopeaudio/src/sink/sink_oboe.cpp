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

#include <algorithm>
#include <atomic>
#include <ctime>
#include <memory>
#include <new>
#include <utility>

#include <android/api-level.h>
#include <oboe/Oboe.h>

#include "nopeaudio/nopeaudio.h"
#include "oboe_stream.h"
#include "sink/sink_internal.h"
#include "utils/time.h"

namespace {

/* The device of a sink: an Oboe output stream, see oboe_stream.h */
class Device final : public nau::OboeStream {
public:
    Device(nau_sink *sink, const nau_engine_config *config, uint32_t period_ms)
        : sink(sink),
          channels(config->channels),
          sample_rate(config->sample_rate),
          period_frames(period_ms ? nau_sink_period(config->sample_rate, period_ms) : 0)
    {
    }

    std::atomic<int64_t> latency{0};  /* nanoseconds, measured or estimated */

protected:
    const char *direction() const override
    {
        return "output";
    }

    void configure(oboe::AudioStreamBuilder &builder) override
    {
        builder.setDirection(oboe::Direction::Output)
            ->setUsage(oboe::Usage::Media)
            ->setContentType(oboe::ContentType::Movie)
            ->setChannelCount(static_cast<int>(channels))
            ->setSampleRate(static_cast<int>(sample_rate));
        if (period_frames)
            builder.setFramesPerDataCallback(static_cast<int>(period_frames));
    }

    int on_open(oboe::AudioStream *opened) override
    {
        /*
         * Latency estimate, used until a timestamp is available: the buffer
         * of the stream, plus the frames written by one callback (a burst
         * when Oboe chooses their size, reported as 0)
         */
        const int32_t rate = opened->getSampleRate() > 0 ? opened->getSampleRate() : static_cast<int32_t>(sample_rate);
        int32_t callback_frames = opened->getFramesPerDataCallback();
        if (callback_frames <= 0)
            callback_frames = std::max(opened->getFramesPerBurst(), 0);
        const int64_t buffered = static_cast<int64_t>(opened->getBufferSizeInFrames()) + callback_frames;
        latency.store(buffered * NAU_NS_PER_SEC / rate, std::memory_order_relaxed);
        return NAU_OK;
    }

    void on_start() override
    {
        nau_sink_reset_status(sink);
    }

    void on_audio(oboe::AudioStream *audio_stream, void *data, int32_t frames) override
    {
        nau_sink_render(sink, static_cast<float *>(data), static_cast<uint32_t>(frames), heard_at(audio_stream));
    }

    /* Not admitted: output silence */
    void on_rejected(void *data, int32_t frames) override
    {
        std::fill_n(static_cast<float *>(data), static_cast<size_t>(frames) * channels, 0.f);
    }

private:
    /*
     * Time at which the first frame of a callback is heard. It is computed
     * from the presentation timestamps of the stream when they are available,
     * from the latency estimate otherwise.
     */
    int64_t heard_at(oboe::AudioStream *audio_stream)
    {
        const int64_t now = nau_sink_now();
        if (timestamps) {
            const auto timestamp = audio_stream->getTimestamp(CLOCK_MONOTONIC);
            const int32_t rate = audio_stream->getSampleRate();
            if (timestamp && rate > 0) {
                const int64_t ahead = audio_stream->getFramesWritten() - timestamp.value().position;
                const int64_t time = timestamp.value().timestamp + ahead * NAU_NS_PER_SEC / rate;
                /*
                 * Sanity check: the time must be within the next second. A
                 * stream converting the sample rate may count the frames
                 * written and the frames presented at different rates.
                 */
                if (time >= now && time - now < NAU_NS_PER_SEC) {
                    latency.store(time - now, std::memory_order_relaxed);
                    return time;
                }
            }
        }
        return now + latency.load(std::memory_order_relaxed);
    }

    /*
     * not used once close() has returned: the sink may then
     * be destroyed
     */
    nau_sink *const sink;
    const uint32_t channels;
    const uint32_t sample_rate;
    const uint32_t period_frames;  /* frames per callback, 0 to let Oboe choose */
    /* Oboe advises against reading timestamps in a callback before Android 11 */
    const bool timestamps = android_get_device_api_level() >= 30;
};

} // namespace

struct nau_sink_device {
    std::shared_ptr<Device> device;
};

extern "C" {

int nau_sink_backend_init(void)
{
    return NAU_OK;
}

int nau_sink_poll(void)
{
    return NAU_OK;
}

int nau_sink_backend_uninit(void)
{
    return NAU_OK;
}

struct nau_sink_device *nau_sink_device_create(void)
{
    return new (std::nothrow) nau_sink_device;
}

int nau_sink_device_init(struct nau_sink_device *d, struct nau_sink *sink,
                         const struct nau_engine_config *config, uint32_t period_ms)
{
    std::shared_ptr<Device> device;
    try {
        device = std::make_shared<Device>(sink, config, period_ms);
    } catch (const std::bad_alloc &) {
        return NAU_ERROR_MEMORY;
    }
    int ret = device->open();
    if (ret < 0) {
        device->close();
        return ret;
    }
    d->device = std::move(device);
    return NAU_OK;
}

int nau_sink_device_start(struct nau_sink_device *d)
{
    return d->device->start();
}

int nau_sink_device_stop(struct nau_sink_device *d)
{
    return d->device->stop();
}

int64_t nau_sink_device_get_latency(const struct nau_sink_device *d)
{
    return d->device->latency.load(std::memory_order_relaxed);
}

int nau_sink_device_get_status(const struct nau_sink_device *d)
{
    return d->device->status.load(std::memory_order_relaxed);
}

void nau_sink_device_freep(struct nau_sink_device **dp)
{
    if (!dp || !*dp)
        return;
    if ((*dp)->device)
        (*dp)->device->close();
    delete *dp;
    *dp = nullptr;
}

} // extern "C"
