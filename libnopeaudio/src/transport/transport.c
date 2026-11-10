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
#include <string.h>

#include "nopeaudio/nopeaudio.h"
#include "engine_internal.h"
#include "transport/transport_clock.h"
#include "utils/memory.h"
#include "utils/utils.h"

/*
 * The control thread publishes playback controls through atomics. The render
 * thread applies them and publishes the position and clock (transport_clock.c).
 *
 * Seeks, pauses and loop wraps fade to silence before changing position to
 * avoid clicks. Rendering resumes with a fade in once the sources are ready:
 *
 *    playing -- pause, seek or range end --> fading out
 *       ^                                       |
 *       | sources ready or timeout              | gain == 0
 *       | (fade in)                             v
 *    waiting <------- playback requested ---- silent
 *
 * While paused, commits are still adopted and sources are positioned so they
 * can decode ahead. At a loop end, an optional hold keeps the output silent
 * and the position at the end of the range. Sources are told to wrap when
 * the hold starts, giving them time to prepare the next pass.
 *
 * Waiting holds the position until the sources are ready or ready_timeout
 * expires, checked between blocks. This preserves the first audio after a
 * seek while decoding catches up. Before resuming from silence, processors
 * are reset without seeking the sources: gain and parameter changes made
 * while idle apply immediately.
 *
 * A seek packs a serial number and target frame in one atomic word so the
 * renderer cannot read a new number with an old target:
 *
 *    63          48 47                                   0
 *    +-------------+-------------------------------------+
 *    |   number    |               frame                 |
 *    +-------------+-------------------------------------+
 *
 * The number distinguishes successive seeks, even to the same frame.
 * seek_seen is the last one applied by the renderer; seek_done publishes it
 * to position getters. Until then, nau_transport_get_position() returns the
 * pending target.
 */

/* Seek packing: the number in the 16 upper bits, the frame in the others */
#define FRAME_BITS 48
#define FRAME_MASK ((UINT64_C(1) << FRAME_BITS) - 1)

#define LOOP_SLOTS 4

struct nau_transport {
    /* Constant once initialized */
    struct nau_engine *engine;
    uint32_t channels;
    uint32_t declick;              /* length of a fade, in frames */
    int64_t ready_timeout;         /* longest wait for the sources, in frames */

    /* Set by the control thread, applied by the renderer */
    _Atomic int playing;           /* also cleared by the renderer at the end */
    _Atomic int looping;
    _Atomic int64_t duration;
    /*
     * The loop range is a pair the renderer must read whole: each range is
     * written to the next slot, then published by the revision selecting it
     * (see get_loop_range() in source_nopemedia.c). A single thread sets it.
     */
    struct {
        _Atomic int64_t start;
        _Atomic int64_t end;
    } loops[LOOP_SLOTS];
    _Atomic uint32_t loop_revision;
    _Atomic int64_t loop_hold;
    _Atomic uint64_t seek;         /* number << FRAME_BITS | frame */
    uint16_t seeks;                /* control thread: number of the last seek */

    /* Published by the renderer */
    _Atomic uint32_t seek_done;    /* number of the last seek applied */
    _Atomic int64_t position_done; /* position after the last render */
    struct nau_transport_clock clock;  /* with its render thread state */

    /* Render thread only */
    int64_t position;              /* frame rendered next */
    uint16_t seek_seen;            /* number of the last seek applied */
    float gain;                    /* gain of the output, ramped to `target` */
    float target;
    uint32_t ramp_left;            /* frames left in the ramp */
    bool waiting;                  /* silent until the sources are ready */
    int64_t waited;                /* frames spent waiting */
    bool wrap_next;                /* the next engine jump is a loop wrap */
    bool holding;                  /* silent at the end of the loop */
    bool idle;                     /* the last block did not render the graph */
    int64_t hold_left;             /* frames left to hold */
};

struct nau_transport *nau_transport_create(void)
{
    struct nau_transport *t = nau_try_calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    atomic_init(&t->playing, 0);
    atomic_init(&t->looping, 0);
    atomic_init(&t->duration, 0);
    for (uint32_t i = 0; i < LOOP_SLOTS; i++) {
        atomic_init(&t->loops[i].start, 0);
        atomic_init(&t->loops[i].end, 0);
    }
    atomic_init(&t->loop_revision, 0);
    atomic_init(&t->loop_hold, 0);
    atomic_init(&t->seek, 0);
    atomic_init(&t->seek_done, 0);
    atomic_init(&t->position_done, 0);
    return t;
}

int nau_transport_init(struct nau_transport *t, struct nau_engine *engine,
                        const struct nau_transport_config *config)
{
    if (!t || t->engine)
        return NAU_ERROR_INVALID;

    if (!config)
        return NAU_ERROR_INVALID;

    const struct nau_engine_config *c = nau_engine_get_config(engine);
    if (!c)
        return NAU_ERROR_INVALID;

    if (!atomic_is_lock_free(&t->seek) || !atomic_is_lock_free(&t->duration))
        return NAU_ERROR_UNSUPPORTED;

    t->channels = c->channels;
    t->declick = config->declick_frames;
    t->ready_timeout = config->ready_timeout_frames;
    nau_transport_clock_init(&t->clock, c->sample_rate);
    /* The first render waits for the sources, like any start from silence */
    t->waiting = true;
    t->engine = engine;
    return NAU_OK;
}

struct nau_engine *nau_transport_get_engine(const struct nau_transport *t)
{
    return t ? t->engine : NULL;
}

void nau_transport_freep(struct nau_transport **tp)
{
    if (!tp || !*tp)
        return;
    nau_freep(tp);
}

void nau_transport_play(struct nau_transport *t)
{
    if (t && t->engine)
        atomic_store(&t->playing, 1);
}

void nau_transport_pause(struct nau_transport *t)
{
    if (t && t->engine)
        atomic_store(&t->playing, 0);
}

int nau_transport_is_playing(const struct nau_transport *t)
{
    return t && t->engine ? atomic_load(&t->playing) : 0;
}

int nau_transport_seek(struct nau_transport *t, int64_t frame)
{
    if (!t || !t->engine)
        return NAU_ERROR_INVALID;

    if (frame < 0 || (uint64_t)frame > FRAME_MASK)
        return NAU_ERROR_INVALID;

    t->seeks++;
    atomic_store_explicit(&t->seek, (uint64_t)t->seeks << FRAME_BITS | (uint64_t)frame, memory_order_release);
    return NAU_OK;
}

int nau_transport_set_duration(struct nau_transport *t, int64_t frames)
{
    if (!t || !t->engine)
        return NAU_ERROR_INVALID;

    if (frames < 0)
        return NAU_ERROR_INVALID;

    atomic_store(&t->duration, frames);
    return NAU_OK;
}

int64_t nau_transport_get_duration(const struct nau_transport *t)
{
    return t && t->engine ? atomic_load(&t->duration) : 0;
}

void nau_transport_set_looping(struct nau_transport *t, int looping)
{
    if (t && t->engine)
        atomic_store(&t->looping, !!looping);
}

int nau_transport_is_looping(const struct nau_transport *t)
{
    return t && t->engine ? atomic_load(&t->looping) : 0;
}

int nau_transport_set_loop_range(struct nau_transport *t, int64_t start, int64_t end)
{
    if (!t || !t->engine)
        return NAU_ERROR_INVALID;

    if (start < 0 || end < 0)
        return NAU_ERROR_INVALID;

    const uint32_t revision = atomic_load(&t->loop_revision) + 1;
    atomic_store(&t->loops[revision % LOOP_SLOTS].start, start);
    atomic_store(&t->loops[revision % LOOP_SLOTS].end, end);
    atomic_store(&t->loop_revision, revision);
    return NAU_OK;
}

int nau_transport_set_loop_hold(struct nau_transport *t, int64_t frames)
{
    if (!t || !t->engine)
        return NAU_ERROR_INVALID;

    if (frames < 0)
        return NAU_ERROR_INVALID;

    atomic_store(&t->loop_hold, frames);
    return NAU_OK;
}

int64_t nau_transport_get_position(const struct nau_transport *t)
{
    if (!t || !t->engine)
        return 0;
    /*
     * A seek the renderer has not applied yet: its target is the position.
     * The seek is read after seek_done, so it is at least as recent as the
     * last one applied.
     */
    const uint16_t done = (uint16_t)atomic_load_explicit(&t->seek_done, memory_order_acquire);
    const uint64_t seek = atomic_load_explicit(&t->seek, memory_order_acquire);
    if ((uint16_t)(seek >> FRAME_BITS) != done) {
        const int64_t duration = atomic_load(&t->duration);
        const int64_t frame = (int64_t)(seek & FRAME_MASK);
        return NAU_MIN(frame, duration);
    }
    return atomic_load_explicit(&t->position_done, memory_order_relaxed);
}

int nau_transport_get_clock(const struct nau_transport *t, struct nau_clock *clock)
{
    if (!t || !t->engine)
        return NAU_ERROR_INVALID;
    return nau_transport_clock_get(&t->clock, clock);
}

int nau_transport_get_position_at(const struct nau_transport *t, int64_t time, int64_t *position)
{
    if (!t || !t->engine)
        return NAU_ERROR_INVALID;
    return nau_transport_clock_get_position_at(&t->clock, time, position);
}

/*
 * The range played: [0, duration), or the loop range clamped to it when
 * looping with a valid range
 */
static void play_range(const struct nau_transport *t, int looping, int64_t duration, int64_t *start, int64_t *end)
{
    *start = 0;
    *end = duration;
    if (!looping)
        return;
    int64_t loop_start, loop_end;
    for (;;) {
        const uint32_t revision = atomic_load(&t->loop_revision);
        loop_start = atomic_load(&t->loops[revision % LOOP_SLOTS].start);
        loop_end = atomic_load(&t->loops[revision % LOOP_SLOTS].end);
        if (atomic_load(&t->loop_revision) == revision)
            break;
    }
    if (loop_end <= loop_start || loop_start >= duration)
        return;
    *start = loop_start;
    if (loop_end < duration)
        *end = loop_end;
}

/*
 * Tell whether the position can jump now: the output is silent, or the
 * fades are disabled
 */
static bool silent(const struct nau_transport *t, uint32_t declick)
{
    return t->gain == 0.f || !declick;
}

/*
 * Apply the gain to the output while ramping it to `target`. The slope is
 * constant: a full ramp, from 0 to 1, takes `declick` frames.
 */
static void ramp(struct nau_transport *t, float *output, uint32_t frames, float target, uint32_t declick)
{
    if (target != t->target) {
        t->target = target;
        /*
         * Computed in double: as a float, a declick close to UINT32_MAX
         * rounds up to 2^32, which does not fit
         */
        t->ramp_left = (uint32_t)ceil(fabs((double)target - t->gain) * declick);
    }
    if (!t->ramp_left) {
        t->gain = target;
        if (target == 0.f)
            memset(output, 0, (size_t)frames * t->channels * sizeof(*output));
        return;
    }
    for (uint32_t i = 0; i < frames; i++) {
        if (t->ramp_left) {
            t->gain += (target - t->gain) / (float)t->ramp_left;
            t->ramp_left--;
        }
        for (uint32_t c = 0; c < t->channels; c++)
            output[(size_t)i * t->channels + c] *= t->gain;
    }
}

/*
 * Move to the target of a seek. The position is published before the number:
 * once nau_transport_get_position() sees the seek as applied, it must not read
 * the position from before the seek.
 */
static void apply_seek(struct nau_transport *t, uint64_t seek, int64_t duration)
{
    const int64_t frame = (int64_t)(seek & FRAME_MASK);
    t->position = NAU_MIN(frame, duration);
    t->seek_seen = (uint16_t)(seek >> FRAME_BITS);
    t->holding = false;
    t->wrap_next = false;
    atomic_store_explicit(&t->position_done, t->position, memory_order_relaxed);
    atomic_store_explicit(&t->seek_done, t->seek_seen, memory_order_release);
}

void nau_transport_sync(struct nau_transport *t)
{
    if (!t || !t->engine)
        return;
    /* The rendering was interrupted: the next render starts from silence */
    t->gain = t->target = 0.f;
    t->ramp_left = 0;
    t->waiting = false;
    const uint64_t seek = atomic_load_explicit(&t->seek, memory_order_acquire);
    if ((uint16_t)(seek >> FRAME_BITS) != t->seek_seen)
        apply_seek(t, seek, atomic_load(&t->duration));
    nau_engine_prepare(t->engine, t->position, 0, 1);
    t->idle = true;
    /* Discard clock history from before the stopped playback was repositioned. */
    nau_transport_clock_clear(&t->clock);
}

int nau_transport_render(struct nau_transport *t, int64_t time, uint32_t frames, float *output)
{
    if (!t || !t->engine)
        return NAU_ERROR_INVALID;

    if (frames && !output)
        return NAU_ERROR_INVALID;

    if ((size_t)frames > SIZE_MAX / sizeof(*output) / t->channels)
        return NAU_ERROR_INVALID;

    if (!frames)
        return NAU_OK;
    memset(output, 0, (size_t)frames * t->channels * sizeof(*output));

    const int64_t duration = atomic_load(&t->duration);
    const int looping = atomic_load(&t->looping);
    const int64_t hold = atomic_load(&t->loop_hold);
    int64_t start, end;
    play_range(t, looping, duration, &start, &end);
    /*
     * A range shorter than two fades gets shorter fades, so that the fade in
     * and the fade out both fit in it
     */
    const uint32_t declick = end - start < 2 * (int64_t)t->declick
                           ? (uint32_t)((end - start) / 2) : t->declick;

    struct nau_clock_writer writer = nau_transport_clock_begin(&t->clock, time, frames);

    int result = NAU_OK;
    uint32_t done = 0;
    while (done < frames) {
        const uint64_t seek = atomic_load_explicit(&t->seek, memory_order_acquire);
        bool pending = (uint16_t)(seek >> FRAME_BITS) != t->seek_seen;
        /*
         * A pending seek is applied once the output is silent. Until then
         * it makes the output fade out (see the target of the ramp below).
         */
        if (pending && silent(t, declick)) {
            apply_seek(t, seek, duration);
            t->waiting = false; /* a new wait starts at the new position */
            pending = false;
            nau_engine_prepare(t->engine, t->position, 0, 0);
        }

        const int playing = atomic_load(&t->playing);
        if (!playing && silent(t, declick)) {
            /*
             * Paused. The pending commit is still adopted, and its new
             * nodes are positioned where the playback will resume: at the
             * start of the loop during a hold.
             */
            if (t->holding)
                nau_engine_prepare(t->engine, start, 1, 0);
            else
                nau_engine_prepare(t->engine, t->position, 0, 0);
            nau_transport_clock_record(&t->clock, &writer, done, frames - done, t->position, 0);
            t->waiting = false; /* a new wait starts on play */
            t->idle = true;
            break;
        }

        /*
         * The end of the range. If it moved to or before the playhead, the
         * output fades out first, past the end.
         */
        if (t->position >= end && silent(t, declick)) {
            if (!looping) {
                /* The end of the project: stop, and go back to its start */
                t->position = 0;
                atomic_store(&t->playing, 0);
                nau_engine_prepare(t->engine, 0, 0, 0);
                nau_transport_clock_record(&t->clock, &writer, done, frames - done, 0, 0);
                t->idle = true;
                break;
            }
            if (!t->holding && hold > 0) {
                t->holding = true;
                t->hold_left = hold;
                /* The end may have moved before the playhead: hold at the end */
                t->position = end;
            }
            if (t->holding) {
                /*
                 * The sources are told about the wrap when the hold starts,
                 * so that they get the start of the loop ready during the
                 * hold. A commit adopted during the hold has its new nodes
                 * positioned there as well.
                 */
                nau_engine_prepare(t->engine, start, 1, 0);
                /* The hold was shortened meanwhile */
                if (t->hold_left > hold)
                    t->hold_left = hold;
                const int64_t count = NAU_MIN(t->hold_left, frames - done);
                nau_transport_clock_record(&t->clock, &writer, done, (uint32_t)count, t->position, 0);
                done += (uint32_t)count;
                t->hold_left -= count;
                t->idle = true;
                if (t->hold_left)
                    continue;
                t->holding = false;
                t->position = start;
            } else {
                /*
                 * No hold: the sources are told about the wrap by the next
                 * engine call
                 */
                t->position = start;
                t->wrap_next = true;
            }
            /* An empty range: nothing to play */
            if (t->position >= end) {
                nau_transport_clock_record(&t->clock, &writer, done, frames - done, t->position, 0);
                t->idle = true;
                break;
            }
            continue;
        }
        /* The range moved past a hold: the playback goes on */
        t->holding = false;

        /*
         * Starting from silence: wait for the sources before fading in,
         * up to the readiness timeout.
         */
        if (t->gain == 0.f && !t->waiting) {
            t->waiting = true;
            t->waited = 0;
        }

        const int64_t to_end = end - t->position;
        int64_t block = frames - done;
        if (to_end > 0 && block > to_end)
            block = to_end;
        /* Split at the fade-out boundary so the ramp starts on the right frame. */
        if (to_end > declick && block > to_end - declick)
            block = to_end - declick;
        /*
         * Keep transitions short so seeks, pauses, readiness and the end of
         * the range are checked again within one fade duration.
         */
        if (declick && block > declick && (t->ramp_left || t->waiting || pending || !playing || to_end <= 0))
            block = declick;
        /*
         * Fading out for a pause, a seek or the end of the range: the block
         * ends with the fade, so that the transition applies on the first
         * silent frame instead of after silent frames
         */
        if (declick && t->gain > 0.f && (!playing || pending || to_end <= (int64_t)declick)) {
            const int64_t fade = t->target == 0.f ? t->ramp_left : (int64_t)ceil((double)t->gain * declick);
            if (fade && block > fade)
                block = fade;
        }

        if (t->waiting && t->waited >= t->ready_timeout)
            t->waiting = false;

        /*
         * Waiting: while a source is not ready, the block is left silent
         * and the position does not advance. The first frames are then
         * heard instead of being skipped. The clock is recorded as stopped.
         */
        if (t->waiting) {
            nau_engine_prepare(t->engine, t->position, t->wrap_next, 0);
            t->wrap_next = false;
            int ready = nau_engine_ready(t->engine, t->position, (uint32_t)block);
            if (ready == NAU_AGAIN) {
                nau_transport_clock_record(&t->clock, &writer, done, (uint32_t)block, t->position, 0);
                done += (uint32_t)block;
                t->waited += block;
                t->idle = true;
                if (result == NAU_OK)
                    result = NAU_AGAIN;
                continue;
            }
        }

        /* Apply targets set while idle immediately; playback resumes from silence. */
        if (t->idle)
            nau_engine_reset_processors(t->engine, t->position);
        t->idle = false;

        float *dst = output + (size_t)done * t->channels;
        /*
         * Finish fading the current graph before adopting edits made during
         * a pause. Replacing it mid-fade could introduce a discontinuity.
         */
        int ret = nau_engine_render_jump(t->engine, t->position, (uint32_t)block, dst, t->wrap_next,
                                         playing || silent(t, declick));
        t->wrap_next = false;
        if (ret < 0)
            result = ret;
        else if (ret == NAU_AGAIN && result == NAU_OK)
            result = NAU_AGAIN;
        if (t->waiting) {
            if (ret != NAU_AGAIN)
                t->waiting = false;
            else
                t->waited += block;
        }

        /*
         * The last `declick` frames of the range fade out. The gain goes
         * to 1 only when nothing calls for silence.
         */
        const bool fading_out = declick && to_end <= declick;
        ramp(t, dst, (uint32_t)block, playing && !pending && !t->waiting && !fading_out ? 1.f : 0.f, declick);
        /*
         * Published before the clock: a reader that sees these frames in the
         * history also sees them rendered
         */
        atomic_store_explicit(&t->position_done, t->position + block, memory_order_relaxed);
        nau_transport_clock_record(&t->clock, &writer, done, (uint32_t)block, t->position, 1);
        t->position += block;
        done += (uint32_t)block;
    }

    atomic_store_explicit(&t->position_done, t->position, memory_order_relaxed);
    return result;
}
