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

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#endif

#include "nopeaudio/nopeaudio.h"
#include "transport/transport_clock.h"
#include "utils/time.h"
#include "utils/utils.h"

/*
 * Video synchronization needs the position currently heard, which can be
 * several renders behind the latest position. Keep the last
 * NAU_CLOCK_HISTORY_SIZE segments in a ring so
 * nau_transport_get_position_at() can look up their presentation times.
 *
 *                                     write (next entry written)
 *                                     v
 *    +------+------+------+------+------+------+------+------+
 *    | e[4] | e[5] | e[6] | e[7] | e[0] | e[1] | e[2] | e[3] |
 *    +------+------+------+------+------+------+------+------+
 *                            newest oldest
 *
 * An entry describes a run of `frames` frames starting at `position`, heard
 * from `time` to `end_time`. `playing` is 0 when the position did not
 * advance during the run (pause, hold, waiting).
 *
 * The render thread cannot take a lock, so the ring is protected by a
 * sequence number (`sequence`):
 *
 *    writer    sequence 4 -> 5 (odd) | write the entry | 5 -> 6 (even)
 *    reader    read the sequence, retry if odd | read the entries |
 *              read the sequence again, retry if it changed
 *
 * The writer never waits. Readers accept a copy only if the sequence was
 * even and unchanged. Retries are bounded; a reader returns NAU_AGAIN if it
 * cannot get a consistent copy. Entries are atomic because a writer may
 * overwrite them while a reader copies them.
 */

/*
 * Bandwidth of the filter smoothing the render times (see smooth_time()).
 * It is low, to reject the jitter of the callbacks and still follow the
 * drift of the device.
 */
#define CLOCK_BANDWIDTH_HZ 0.5
/* Reset the filter after a timing error this large, such as a device stall. */
#define CLOCK_RESET_NS 250e6

/*
 * How many times a clock reader retries during a write. It first spins,
 * since a write is short. It then yields between tries, in case the render
 * thread was preempted in the middle of a write, and gives up with
 * NAU_AGAIN after CLOCK_READ_TRIES.
 */
#define CLOCK_READ_SPINS 64
#define CLOCK_READ_TRIES (CLOCK_READ_SPINS + 4096)

void nau_transport_clock_init(struct nau_transport_clock *c, uint32_t sample_rate)
{
    c->sample_rate = sample_rate;
    atomic_init(&c->sequence, 0);
    atomic_init(&c->count, 0);
    atomic_init(&c->write, 0);
    for (uint32_t i = 0; i < NAU_CLOCK_HISTORY_SIZE; i++) {
        atomic_init(&c->entries[i].position, 0);
        atomic_init(&c->entries[i].time, 0);
        atomic_init(&c->entries[i].end_time, 0);
        atomic_init(&c->entries[i].frames, 0);
        atomic_init(&c->entries[i].playing, 0);
    }
    c->ready = false;
    c->next_time = 0.;
    c->period = 0.;
}

static void yield_thread(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

static void read_clock(const struct nau_transport_clock *c, uint32_t index, struct nau_clock *clock)
{
    const struct nau_clock_entry *entry = &c->entries[index];
    *clock = (struct nau_clock){
        .position = atomic_load_explicit(&entry->position, memory_order_relaxed),
        .time = atomic_load_explicit(&entry->time, memory_order_relaxed),
        .playing = atomic_load_explicit(&entry->playing, memory_order_relaxed),
        .sample_rate = c->sample_rate,
    };
}

int nau_transport_clock_get(const struct nau_transport_clock *c, struct nau_clock *clock)
{
    if (!clock)
        return NAU_ERROR_INVALID;
    for (int attempt = 0; attempt < CLOCK_READ_TRIES; attempt++) {
        if (attempt >= CLOCK_READ_SPINS)
            yield_thread();
        const uint32_t sequence = atomic_load_explicit(&c->sequence, memory_order_acquire);
        if (sequence & 1)
            continue;
        const uint32_t count = atomic_load_explicit(&c->count, memory_order_relaxed);
        const uint32_t next = atomic_load_explicit(&c->write, memory_order_relaxed);
        struct nau_clock snapshot;
        read_clock(c, (next + NAU_CLOCK_HISTORY_SIZE - 1) % NAU_CLOCK_HISTORY_SIZE, &snapshot);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&c->sequence, memory_order_relaxed) == sequence) {
            /* No render yet, or rendered without a time */
            if (!count || !snapshot.time)
                return NAU_AGAIN;
            *clock = snapshot;
            return NAU_OK;
        }
    }
    return NAU_AGAIN;
}

int64_t nau_clock_position_at(const struct nau_clock *clock, int64_t time)
{
    if (!clock)
        return 0;
    if (!clock->playing)
        return clock->position;
    const long double position = (long double)clock->position +
                                 ((long double)time - (long double)clock->time) * clock->sample_rate / NAU_NS_PER_SEC;
    if (position <= 0)
        return 0;
    if (position >= NAU_INT64_MAX_EXCLUSIVE)
        return INT64_MAX;
    return (int64_t)position;
}

/*
 * The position at a time within a run, along the run's own span: it follows
 * the smoothed pace of the device rather than the nominal rate, so that it
 * neither passes the end of the run nor goes back at the next one
 */
static int64_t position_in_run(const struct nau_clock *clock, uint32_t frames, int64_t end, int64_t time)
{
    if (!clock->playing || end <= clock->time)
        return clock->position;
    const long double offset = ((long double)time - (long double)clock->time) * frames /
                               ((long double)end - (long double)clock->time);
    return clock->position + (int64_t)offset;
}

int nau_transport_clock_get_position_at(const struct nau_transport_clock *c, int64_t time,
                                        int64_t *position)
{
    if (!position)
        return NAU_ERROR_INVALID;
    for (int attempt = 0; attempt < CLOCK_READ_TRIES; attempt++) {
        if (attempt >= CLOCK_READ_SPINS)
            yield_thread();
        const uint32_t sequence = atomic_load_explicit(&c->sequence, memory_order_acquire);
        if (sequence & 1)
            continue;
        const uint32_t count = atomic_load_explicit(&c->count, memory_order_relaxed);
        const uint32_t next = atomic_load_explicit(&c->write, memory_order_relaxed);
        int ret = NAU_AGAIN;
        int64_t value = 0;
        /*
         * From the newest entry back to the first one starting at or before
         * `time`
         */
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t index = (next + NAU_CLOCK_HISTORY_SIZE - 1 - i) % NAU_CLOCK_HISTORY_SIZE;
            struct nau_clock clock;
            read_clock(c, index, &clock);
            /* Rendered without a time */
            if (!clock.time)
                break;
            if (time < clock.time) {
                /*
                 * Before the oldest entry. While the ring has not wrapped,
                 * that entry is the first render, and its position is the
                 * answer. Afterwards older entries were overwritten, and
                 * the time is too old.
                 */
                if (i == count - 1 && count < NAU_CLOCK_HISTORY_SIZE) {
                    value = clock.position;
                    ret = NAU_OK;
                }
                continue;
            }
            const struct nau_clock_entry *entry = &c->entries[index];
            const int64_t end = atomic_load_explicit(&entry->end_time, memory_order_relaxed);
            const uint32_t frames = atomic_load_explicit(&entry->frames, memory_order_relaxed);
            /*
             * Past the end of the run (nothing rendered since, or a gap
             * before the next run): the position stays at its end
             */
            value = time >= end ? clock.position + (clock.playing ? frames : 0)
                                : position_in_run(&clock, frames, end, time);
            ret = NAU_OK;
            break;
        }
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&c->sequence, memory_order_relaxed) == sequence) {
            if (ret == NAU_OK)
                *position = value;
            return ret;
        }
    }
    return NAU_AGAIN;
}

static struct nau_clock_writer begin_clock(struct nau_transport_clock *c, int64_t time)
{
    struct nau_clock_writer writer = {
        .sequence = atomic_load_explicit(&c->sequence, memory_order_relaxed),
        .next = atomic_load_explicit(&c->write, memory_order_relaxed),
        .count = atomic_load_explicit(&c->count, memory_order_relaxed),
        .time = time,
        .period = c->period,
    };
    return writer;
}

static int64_t clamp_time(long double time)
{
    if (time >= NAU_INT64_MAX_EXCLUSIVE)
        return INT64_MAX;
    if (time <= -NAU_INT64_MAX_EXCLUSIVE)
        return INT64_MIN;
    return (int64_t)time;
}

/* The time the frame `offset` of the render call is heard */
static int64_t clock_time_at(const struct nau_clock_writer *writer, uint32_t offset)
{
    return writer->time ? clamp_time((long double)writer->time + (long double)offset * writer->period) : 0;
}

static void start_clock_write(struct nau_transport_clock *c, const struct nau_clock_writer *writer)
{
    atomic_store_explicit(&c->sequence, writer->sequence + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}

static void end_clock(struct nau_transport_clock *c, struct nau_clock_writer *writer)
{
    atomic_store_explicit(&c->count, writer->count, memory_order_relaxed);
    atomic_store_explicit(&c->write, writer->next, memory_order_relaxed);
    writer->sequence += 2;
    atomic_store_explicit(&c->sequence, writer->sequence, memory_order_release);
}

/*
 * Record frames rendered from position, starting at offset in the render call.
 *
 * Merge consecutive blocks into one entry so splitting a render call does
 * not consume the history faster.
 */
void nau_transport_clock_record(struct nau_transport_clock *c, struct nau_clock_writer *writer,
                                uint32_t offset, uint32_t frames, int64_t position, int playing)
{
    if (!frames)
        return;
    start_clock_write(c, writer);
    struct nau_clock_entry *entry = &c->entries[(writer->next + NAU_CLOCK_HISTORY_SIZE - 1) % NAU_CLOCK_HISTORY_SIZE];
    if (writer->merge && atomic_load_explicit(&entry->playing, memory_order_relaxed) == playing) {
        const int64_t previous = atomic_load_explicit(&entry->position, memory_order_relaxed);
        const uint32_t count = atomic_load_explicit(&entry->frames, memory_order_relaxed);
        if (position == previous + (playing ? count : 0)) {
            atomic_store_explicit(&entry->frames, count + frames, memory_order_relaxed);
            atomic_store_explicit(&entry->end_time, clock_time_at(writer, offset + frames), memory_order_relaxed);
            end_clock(c, writer);
            return;
        }
    }
    entry = &c->entries[writer->next];
    atomic_store_explicit(&entry->position, position, memory_order_relaxed);
    atomic_store_explicit(&entry->time, clock_time_at(writer, offset), memory_order_relaxed);
    atomic_store_explicit(&entry->end_time, clock_time_at(writer, offset + frames), memory_order_relaxed);
    atomic_store_explicit(&entry->frames, frames, memory_order_relaxed);
    atomic_store_explicit(&entry->playing, playing, memory_order_relaxed);
    writer->next = (writer->next + 1) % NAU_CLOCK_HISTORY_SIZE;
    if (writer->count < NAU_CLOCK_HISTORY_SIZE)
        writer->count++;
    writer->merge = true;
    end_clock(c, writer);
}

/*
 * Smooth the presentation time of a render call.
 *
 * The times given by the device callbacks jitter with the scheduling of the
 * callbacks, while the device itself plays at a steady rate. A clock made of
 * the raw times would step back and forth, and so would a video following
 * it. The times are filtered with a delay-locked loop (see Fons Adriaensen,
 * "Using a DLL to filter time"): it predicts the time of the next render
 * from the duration of a frame, and corrects both by a fraction of the
 * error between the prediction and the time given.
 *
 * The filter starts over from the time given on the first render, after a
 * large error (CLOCK_RESET_NS), and for renders too long for it to be
 * stable.
 */
static int64_t smooth_time(struct nau_transport_clock *c, int64_t time, uint32_t frames)
{
    const double nominal = (double)NAU_NS_PER_SEC / c->sample_rate;
    const double error = (double)time - c->next_time;
    const double w = 2 * NAU_PI_F64 * CLOCK_BANDWIDTH_HZ * frames / c->sample_rate;
    if (!c->ready || error > CLOCK_RESET_NS || error < -CLOCK_RESET_NS || w >= 1.) {
        c->ready = true;
        c->period = nominal;
        c->next_time = (double)time + nominal * frames;
        return time;
    }
    const double smoothed = c->next_time + sqrt(2.) * w * error;
    c->period += w * w * error / frames;
    c->next_time = smoothed + c->period * frames;
    return clamp_time(smoothed);
}

struct nau_clock_writer nau_transport_clock_begin(struct nau_transport_clock *c, int64_t time, uint32_t frames)
{
    const int64_t smoothed = time ? smooth_time(c, time, frames) : 0;
    return begin_clock(c, smoothed);
}

void nau_transport_clock_clear(struct nau_transport_clock *c)
{
    struct nau_clock_writer writer = begin_clock(c, 0);
    start_clock_write(c, &writer);
    writer.count = 0;
    end_clock(c, &writer);
    c->ready = false;
}
