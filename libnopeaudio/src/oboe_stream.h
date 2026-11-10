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

#ifndef NAU_OBOE_STREAM_H
#define NAU_OBOE_STREAM_H

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include <android/log.h>
#include <oboe/Oboe.h>

#include "nopeaudio/nopeaudio.h"

namespace nau {

/*
 * Oboe stream lifecycle and callback synchronization. The sink derives
 * from this class to configure the output and render its audio.
 *
 * Ownership
 *
 * The OboeStream is the data callback and the error callback of its stream.
 * Oboe takes them as shared_ptr and keeps them as long as the stream lives,
 * so the stream and the OboeStream hold each other:
 *
 *                                 stream
 *    owner ---> OboeStream <============> AudioStream <--- Oboe's error thread
 *                               callbacks
 *
 * close() breaks the cycle by closing and releasing the stream. Oboe's error
 * thread may still hold a reference, keeping OboeStream alive after its
 * owner is destroyed. closed prevents that thread from reopening the stream.
 *
 * Callbacks
 *
 * After stop() or close(), no callback may access the sink: the caller can
 * render from another thread or destroy it. Callbacks increment their count
 * before checking whether their stream is still admitted:
 *
 *    callback (Oboe's thread)              control: disable_callbacks()
 *
 *    callbacks++                           callback_stream = nullptr
 *    stream == callback_stream ?           wait until callbacks == 0
 *       yes: on_audio()
 *       no:  on_rejected()
 *    callbacks--
 *
 * Both sides use sequentially consistent atomics. Either the callback is
 * counted before the control thread reads the count, and the control thread
 * waits for it; or it reads callback_stream after it was cleared, and is not
 * admitted.
 *
 * stream->stop() alone is insufficient: it can fail or time out while a
 * callback is still running, and Oboe can close the stream itself on error.
 */
class OboeStream : public oboe::AudioStreamDataCallback,
                   public oboe::AudioStreamErrorCallback,
                   public std::enable_shared_from_this<OboeStream> {
public:
    int open()
    {
        std::lock_guard<std::mutex> guard(lock);
        return open_stream();
    }

    int start()
    {
        std::lock_guard<std::mutex> guard(lock);
        if (started)
            return NAU_OK;
        on_start();
        int ret = start_locked();
        status.store(ret, std::memory_order_relaxed);
        if (ret == NAU_OK)
            started = true;
        return ret;
    }

    /* When it returns, no callback is admitted anymore */
    int stop()
    {
        std::lock_guard<std::mutex> guard(lock);
        disable_callbacks();
        started = false;
        if (!stream)
            return NAU_OK;
        oboe::Result result = stream->stop();
        if (result != oboe::Result::OK) {
            close_stream();
            return NAU_ERROR_IO;
        }
        return NAU_OK;
    }

    void close()
    {
        std::lock_guard<std::mutex> guard(lock);
        closed = true;
        started = false;
        close_stream();
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audio_stream, void *data, int32_t frames) final
    {
        callbacks.fetch_add(1);
        const bool accepted = audio_stream == callback_stream.load();
        if (!accepted) {
            callbacks.fetch_sub(1);
            /*
             * Return Continue so Oboe does not stop the stream concurrently
             * with the control thread that disabled callbacks.
             */
            on_rejected(data, frames);
            return oboe::DataCallbackResult::Continue;
        }
        on_audio(audio_stream, data, frames);
        callbacks.fetch_sub(1);
        return oboe::DataCallbackResult::Continue;
    }

    /*
     * Called on Oboe's error thread, once it has stopped and closed the
     * stream. After a disconnection (headphones plugged, device captured,
     * ...), another stream is opened on the new device, and started if this
     * one was. Any other error, or a failure to do so, stops the device and
     * is reported by its status.
     */
    void onErrorAfterClose(oboe::AudioStream *closed_stream, oboe::Result error) final
    {
        std::lock_guard<std::mutex> guard(lock);
        if (closed || stream.get() != closed_stream)
            return;
        disable_callbacks();
        stream.reset();
        int ret = NAU_ERROR_IO;
        if (error == oboe::Result::ErrorDisconnected)
            ret = open_stream();
        if (ret == NAU_OK && started)
            ret = start_locked();
        if (ret == NAU_OK)
            return;
        started = false;
        status.store(ret < 0 ? ret : NAU_ERROR_IO, std::memory_order_relaxed);
    }

    std::atomic<int> status{NAU_OK};  /* last device error */

protected:
    /* "output" or "input", for the logs */
    virtual const char *direction() const = 0;

    /* Configure the stream direction and requested format. */
    virtual void configure(oboe::AudioStreamBuilder &builder) = 0;

    /*
     * The stream was opened: check it, and get what is needed from it. With
     * an error, the stream is closed and the opening fails.
     */
    virtual int on_open(oboe::AudioStream *opened) = 0;

    /* The device is about to start */
    virtual void on_start() {}

    /* An admitted callback: render or take the frames */
    virtual void on_audio(oboe::AudioStream *audio_stream, void *data, int32_t frames) = 0;

    /* A callback which is not admitted */
    virtual void on_rejected(void *, int32_t) {}

private:
    /*
     * Refuse the next callbacks and wait for the one running, if any. See the
     * diagram above the class: the default (sequentially consistent) order
     * of these atomics is required.
     */
    void disable_callbacks()
    {
        callback_stream.store(nullptr);
        while (callbacks.load())
            std::this_thread::yield();
    }

    void close_stream()
    {
        disable_callbacks();
        if (stream) {
            stream->stop();
            stream->close();
            stream.reset();
        }
    }

    int open_stream()
    {
        oboe::AudioStreamBuilder builder;
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)
            ->setFormat(oboe::AudioFormat::Float)
            ->setFormatConversionAllowed(true)
            ->setChannelConversionAllowed(true)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            ->setDataCallback(shared_from_this())
            ->setErrorCallback(shared_from_this());
        configure(builder);
        std::shared_ptr<oboe::AudioStream> opened;
        oboe::Result result = builder.openStream(opened);
        if (result != oboe::Result::OK) {
            __android_log_print(ANDROID_LOG_ERROR, "nau", "Oboe: opening the %s failed: %s", direction(),
                                oboe::convertToText(result));
            return NAU_ERROR_IO;
        }
        int ret = on_open(opened.get());
        if (ret < 0) {
            opened->close();
            return ret;
        }
        __android_log_print(ANDROID_LOG_INFO, "nau",
                            "Oboe: %s%s %s, %d Hz, %d channels, bursts of %d frames, callbacks of %d, "
                            "buffer %d of %d",
                            opened->getAudioApi() == oboe::AudioApi::AAudio ? "AAudio" : "OpenSL ES",
                            oboe::OboeExtensions::isMMapUsed(opened.get()) ? " MMAP" : "", direction(),
                            opened->getSampleRate(), opened->getChannelCount(), opened->getFramesPerBurst(),
                            opened->getFramesPerDataCallback(), opened->getBufferSizeInFrames(),
                            opened->getBufferCapacityInFrames());
        stream = std::move(opened);
        return NAU_OK;
    }

    int start_locked()
    {
        if (!stream) {
            int ret = open_stream();
            if (ret != NAU_OK)
                return ret;
        }
        callback_stream.store(stream.get());
        oboe::Result result = stream->start();
        if (result == oboe::Result::OK)
            return NAU_OK;
        close_stream();
        return NAU_ERROR_IO;
    }

    /*
     * Protects the stream, `started` and `closed`, between the control
     * thread and Oboe's error thread
     */
    std::mutex lock;
    std::shared_ptr<oboe::AudioStream> stream;
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    static_assert(std::atomic<oboe::AudioStream *>::is_always_lock_free);
    std::atomic<uint32_t> callbacks{0};
    std::atomic<oboe::AudioStream *> callback_stream{nullptr};
    bool started = false;
    bool closed = false;
};

} // namespace nau

#endif
