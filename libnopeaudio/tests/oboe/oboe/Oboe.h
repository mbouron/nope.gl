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

#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>

/* The Oboe surface used by the sink, with controllable lifecycle failures. */
namespace oboe {
enum class Result { OK, ErrorTimeout, ErrorDisconnected };
enum class DataCallbackResult { Continue, Stop };
enum class Direction { Output };
enum class PerformanceMode { LowLatency };
enum class SharingMode { Shared };
enum class Usage { Media };
enum class ContentType { Movie };
enum class AudioFormat { Float };
enum class SampleRateConversionQuality { Medium };
enum class AudioApi { AAudio, OpenSLES };
class AudioStream;
class AudioStreamDataCallback {
public:
    virtual ~AudioStreamDataCallback() = default;
    virtual DataCallbackResult onAudioReady(AudioStream *, void *, int32_t) = 0;
};
class AudioStreamErrorCallback {
public:
    virtual ~AudioStreamErrorCallback() = default;
    virtual void onErrorAfterClose(AudioStream *, Result) = 0;
};
struct Timestamp { int64_t position = 0; int64_t timestamp = 0; };
struct TimestampResult {
    explicit operator bool() const { return false; }
    Timestamp value() const { return {}; }
};
class AudioStream {
public:
    std::shared_ptr<AudioStreamDataCallback> data;
    std::shared_ptr<AudioStreamErrorCallback> error;
    Result stop_result = Result::OK;
    bool closed = false;
    int rate = 48000;
    int channels = 2;
    int period = 480;
    std::function<Result(AudioStream *)> start_hook;

    Result start() { return start_hook ? start_hook(this) : Result::OK; }
    Result stop() { return stop_result; }
    Result close() { closed = true; return Result::OK; }
    TimestampResult getTimestamp(clockid_t) { return {}; }
    int getSampleRate() const { return rate; }
    int getChannelCount() const { return channels; }
    int getBufferSizeInFrames() const { return period; }
    int getBufferCapacityInFrames() const { return period * 2; }
    int getFramesPerDataCallback() const { return period; }
    int getFramesPerBurst() const { return period; }
    int64_t getFramesWritten() const { return 0; }
    AudioApi getAudioApi() const { return AudioApi::AAudio; }
};
inline std::shared_ptr<AudioStream> last_stream;
inline std::function<Result(AudioStream *)> next_start;
class AudioStreamBuilder {
public:
    AudioStreamBuilder *setDirection(Direction) { return this; }
    AudioStreamBuilder *setPerformanceMode(PerformanceMode) { return this; }
    AudioStreamBuilder *setSharingMode(SharingMode) { return this; }
    AudioStreamBuilder *setUsage(Usage) { return this; }
    AudioStreamBuilder *setContentType(ContentType) { return this; }
    AudioStreamBuilder *setFormat(AudioFormat) { return this; }
    AudioStreamBuilder *setFormatConversionAllowed(bool) { return this; }
    AudioStreamBuilder *setChannelConversionAllowed(bool) { return this; }
    AudioStreamBuilder *setSampleRateConversionQuality(SampleRateConversionQuality) { return this; }
    AudioStreamBuilder *setChannelCount(int v) { stream->channels = v; return this; }
    AudioStreamBuilder *setSampleRate(int v) { stream->rate = v; return this; }
    AudioStreamBuilder *setFramesPerDataCallback(int v) { stream->period = v; return this; }
    AudioStreamBuilder *setDataCallback(std::shared_ptr<AudioStreamDataCallback> v) { stream->data = v; return this; }
    AudioStreamBuilder *setErrorCallback(std::shared_ptr<AudioStreamErrorCallback> v) { stream->error = v; return this; }
    Result openStream(std::shared_ptr<AudioStream> &out) {
        stream->start_hook = std::move(next_start);
        out = last_stream = stream;
        return Result::OK;
    }
private:
    std::shared_ptr<AudioStream> stream = std::make_shared<AudioStream>();
};
struct OboeExtensions {
    static bool isMMapUsed(AudioStream *) { return false; }
};
inline const char *convertToText(Result) { return "injected error"; }
}
