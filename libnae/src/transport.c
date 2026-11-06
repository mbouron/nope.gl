/* SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "nae/nae.h"
#include "internal.h"

/* Seeks are published with their number in the 16 upper bits */
#define FRAME_BITS 48
#define FRAME_MASK ((UINT64_C(1) << FRAME_BITS) - 1)

/* Bandwidth of the filter smoothing the times of the renders into the clock:
 * low, to reject the jitter of the callbacks while following the device */
#define CLOCK_BANDWIDTH_HZ 0.5
/* A render further than this from the time expected starts the clock over */
#define CLOCK_RESET_NS 250e6

struct nae_transport {
    struct nae_engine *engine;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t declick;
    int64_t ready_timeout;

    /* Set by the control thread, applied by the renderer */
    _Atomic int playing;           /* also cleared by the renderer at the end */
    _Atomic int looping;
    _Atomic int64_t duration;
    _Atomic int64_t loop_start;
    _Atomic int64_t loop_end;
    _Atomic int64_t loop_hold;
    _Atomic uint64_t seek;         /* number << FRAME_BITS | frame */
    uint16_t seeks;                /* control thread: seeks made */

    /* Published by the renderer */
    _Atomic uint32_t seek_done;    /* number of the last seek applied */
    _Atomic int64_t position_done;
    _Atomic uint32_t clock_sequence;  /* odd while written */
    _Atomic int64_t clock_position;
    _Atomic int64_t clock_time;
    _Atomic int clock_playing;

    /* Render thread */
    int64_t position;
    uint16_t seek_seen;
    float gain;                    /* of the output, ramped to `target` */
    float target;
    uint32_t ramp_left;
    bool waiting;                  /* silent after a jump, for the sources to have their data */
    int64_t waited;
    bool wrap_next;                /* the next render goes round the loop */
    bool holding;
    int64_t hold_left;
    bool clock_ready;
    double clock_next;             /* time expected of the next render */
    double clock_period;           /* nanoseconds per frame */
};

int nae_transport_create(struct nae_engine *engine, const struct nae_transport_config *config,
                         struct nae_transport **transport)
{
    if (!transport)
        return NAE_ERROR_INVALID;
    *transport = NULL;
    const struct nae_config *c = nae_engine_get_config(engine);
    if (!c || !config)
        return NAE_ERROR_INVALID;
    struct nae_transport *t = calloc(1, sizeof(*t));
    if (!t)
        return NAE_ERROR_MEMORY;
    t->engine = engine;
    t->channels = c->channels;
    t->sample_rate = c->sample_rate;
    t->declick = config->declick_frames;
    t->ready_timeout = config->ready_timeout_frames;
    atomic_init(&t->playing, 0);
    atomic_init(&t->looping, 0);
    atomic_init(&t->duration, 0);
    atomic_init(&t->loop_start, 0);
    atomic_init(&t->loop_end, 0);
    atomic_init(&t->loop_hold, 0);
    atomic_init(&t->seek, 0);
    atomic_init(&t->seek_done, 0);
    atomic_init(&t->position_done, 0);
    atomic_init(&t->clock_sequence, 0);
    atomic_init(&t->clock_position, 0);
    atomic_init(&t->clock_time, 0);
    atomic_init(&t->clock_playing, 0);
    if (!atomic_is_lock_free(&t->seek) || !atomic_is_lock_free(&t->duration)) {
        free(t);
        return NAE_ERROR_UNSUPPORTED;
    }
    /* Playback starts from silence */
    t->waiting = true;
    *transport = t;
    return NAE_OK;
}

void nae_transport_freep(struct nae_transport **tp)
{
    if (!tp || !*tp)
        return;
    free(*tp);
    *tp = NULL;
}

void nae_transport_play(struct nae_transport *t)
{
    if (t)
        atomic_store(&t->playing, 1);
}

void nae_transport_pause(struct nae_transport *t)
{
    if (t)
        atomic_store(&t->playing, 0);
}

int nae_transport_is_playing(const struct nae_transport *t)
{
    return t ? atomic_load(&t->playing) : 0;
}

int nae_transport_seek(struct nae_transport *t, int64_t frame)
{
    if (!t || frame < 0 || (uint64_t)frame > FRAME_MASK)
        return NAE_ERROR_INVALID;
    t->seeks++;
    atomic_store_explicit(&t->seek, (uint64_t)t->seeks << FRAME_BITS | (uint64_t)frame, memory_order_release);
    return NAE_OK;
}

int nae_transport_set_duration(struct nae_transport *t, int64_t frames)
{
    if (!t || frames < 0)
        return NAE_ERROR_INVALID;
    atomic_store(&t->duration, frames);
    return NAE_OK;
}

int64_t nae_transport_get_duration(const struct nae_transport *t)
{
    return t ? atomic_load(&t->duration) : 0;
}

void nae_transport_set_looping(struct nae_transport *t, int looping)
{
    if (t)
        atomic_store(&t->looping, !!looping);
}

int nae_transport_is_looping(const struct nae_transport *t)
{
    return t ? atomic_load(&t->looping) : 0;
}

int nae_transport_set_loop_range(struct nae_transport *t, int64_t start, int64_t end)
{
    if (!t || start < 0 || end < 0)
        return NAE_ERROR_INVALID;
    atomic_store(&t->loop_start, start);
    atomic_store(&t->loop_end, end);
    return NAE_OK;
}

int nae_transport_set_loop_hold(struct nae_transport *t, int64_t frames)
{
    if (!t || frames < 0)
        return NAE_ERROR_INVALID;
    atomic_store(&t->loop_hold, frames);
    return NAE_OK;
}

int64_t nae_transport_get_position(const struct nae_transport *t)
{
    if (!t)
        return 0;
    /* Set, not applied by the renderer yet */
    const uint64_t seek = atomic_load_explicit(&t->seek, memory_order_acquire);
    if ((uint16_t)(seek >> FRAME_BITS) != (uint16_t)atomic_load_explicit(&t->seek_done, memory_order_acquire)) {
        const int64_t duration = atomic_load(&t->duration);
        const int64_t frame = (int64_t)(seek & FRAME_MASK);
        return frame < duration ? frame : duration;
    }
    return atomic_load_explicit(&t->position_done, memory_order_relaxed);
}

int nae_transport_get_clock(const struct nae_transport *t, struct nae_clock *clock)
{
    if (!t || !clock)
        return NAE_ERROR_INVALID;
    for (int attempt = 0; attempt < 64; attempt++) {
        const uint32_t sequence = atomic_load_explicit(&t->clock_sequence, memory_order_acquire);
        if (sequence & 1)
            continue;
        clock->position = atomic_load_explicit(&t->clock_position, memory_order_relaxed);
        clock->time = atomic_load_explicit(&t->clock_time, memory_order_relaxed);
        clock->playing = atomic_load_explicit(&t->clock_playing, memory_order_relaxed);
        clock->sample_rate = t->sample_rate;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&t->clock_sequence, memory_order_relaxed) == sequence)
            return sequence ? NAE_OK : NAE_AGAIN;
    }
    return NAE_AGAIN;
}

int64_t nae_clock_position_at(const struct nae_clock *clock, int64_t time)
{
    if (!clock)
        return 0;
    if (!clock->playing || time <= clock->time)
        return clock->position;
    return clock->position + (int64_t)((double)(time - clock->time) * clock->sample_rate / 1e9);
}

static void publish_clock(struct nae_transport *t, int64_t position, int64_t time, int playing)
{
    const uint32_t sequence = atomic_load_explicit(&t->clock_sequence, memory_order_relaxed);
    atomic_store_explicit(&t->clock_sequence, sequence + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&t->clock_position, position, memory_order_relaxed);
    atomic_store_explicit(&t->clock_time, time, memory_order_relaxed);
    atomic_store_explicit(&t->clock_playing, playing, memory_order_relaxed);
    atomic_store_explicit(&t->clock_sequence, sequence + 2, memory_order_release);
}

/*
 * The time the render is heard, smoothed with a delay-locked loop: the raw
 * times jitter with the scheduling of the callbacks, which would have the
 * clock step back and forth (see Fons Adriaensen, "Using a DLL to filter
 * time").
 */
static int64_t smooth_time(struct nae_transport *t, int64_t time, uint32_t frames)
{
    const double nominal = 1e9 / t->sample_rate;
    const double error = (double)time - t->clock_next;
    if (!t->clock_ready || error > CLOCK_RESET_NS || error < -CLOCK_RESET_NS) {
        t->clock_ready = true;
        t->clock_period = nominal;
        t->clock_next = (double)time + nominal * frames;
        return time;
    }
    const double w = 2 * 3.14159265358979323846 * CLOCK_BANDWIDTH_HZ * frames / t->sample_rate;
    const double smoothed = t->clock_next + sqrt(2.) * w * error;
    t->clock_period += w * w * error / frames;
    t->clock_next = smoothed + t->clock_period * frames;
    return (int64_t)smoothed;
}

/* The range played: [0, duration), or the loop range in it when looping over one */
static void play_range(const struct nae_transport *t, int looping, int64_t duration, int64_t *start, int64_t *end)
{
    *start = 0;
    *end = duration;
    if (!looping)
        return;
    const int64_t loop_start = atomic_load(&t->loop_start);
    const int64_t loop_end = atomic_load(&t->loop_end);
    if (loop_end <= loop_start || loop_start >= duration)
        return;
    *start = loop_start;
    if (loop_end < duration)
        *end = loop_end;
}

/* The output is silent, or would be at once, without ramps */
static bool silent(const struct nae_transport *t)
{
    return t->gain == 0.f || !t->declick;
}

/* Ramps the output gain to `target` over the declick length, applying it */
static void ramp(struct nae_transport *t, float *output, uint32_t frames, float target)
{
    if (target != t->target) {
        t->target = target;
        t->ramp_left = (uint32_t)ceilf(fabsf(target - t->gain) * (float)t->declick);
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

int nae_transport_render(struct nae_transport *t, int64_t time, uint32_t frames, float *output)
{
    if (!t || (frames && !output))
        return NAE_ERROR_INVALID;
    memset(output, 0, (size_t)frames * t->channels * sizeof(*output));

    const int64_t duration = atomic_load(&t->duration);
    const int looping = atomic_load(&t->looping);
    const int64_t hold = atomic_load(&t->loop_hold);
    int64_t start, end;
    play_range(t, looping, duration, &start, &end);

    /* What the clock tells: the position played from an offset in the output */
    int64_t clock_position = t->position;
    uint32_t clock_offset = 0;
    bool clock_moving = false;

    int result = NAE_OK;
    uint32_t done = 0;
    while (done < frames) {
        const uint64_t seek = atomic_load_explicit(&t->seek, memory_order_acquire);
        const uint16_t number = (uint16_t)(seek >> FRAME_BITS);
        bool pending = number != t->seek_seen;
        /* A seek is taken once silent, for the jump not to be heard */
        if (pending && silent(t)) {
            const int64_t frame = (int64_t)(seek & FRAME_MASK);
            t->position = frame < duration ? frame : duration;
            t->seek_seen = number;
            /* The position before the seek is done, for get_position() to tell either */
            atomic_store_explicit(&t->position_done, t->position, memory_order_relaxed);
            atomic_store_explicit(&t->seek_done, number, memory_order_release);
            t->holding = false;
            t->wrap_next = false;
            pending = false;
            nae_engine_prepare(t->engine, t->position, 0);
            clock_position = t->position;
            clock_offset = done;
        }

        const int playing = atomic_load(&t->playing);
        if (!playing && silent(t))
            break;

        if (t->position >= end) {
            if (!looping) {
                /* The end: back to the start, stopped */
                t->position = 0;
                atomic_store(&t->playing, 0);
                nae_engine_prepare(t->engine, 0, 0);
                clock_position = 0;
                clock_offset = done;
                clock_moving = false;
                break;
            }
            if (!t->holding && hold > 0) {
                /* The sources go round the loop while it holds still */
                t->holding = true;
                t->hold_left = hold;
                nae_engine_prepare(t->engine, start, 1);
            }
            if (t->holding) {
                if (t->hold_left > hold)
                    t->hold_left = hold;
                const int64_t count = t->hold_left < frames - done ? t->hold_left : frames - done;
                clock_position = t->position;
                clock_offset = done;
                clock_moving = false;
                done += (uint32_t)count;
                t->hold_left -= count;
                if (t->hold_left)
                    continue;
                t->holding = false;
                t->position = start;
            } else {
                t->position = start;
                t->wrap_next = true;
            }
            clock_position = t->position;
            clock_offset = done;
            if (t->position >= end)
                break;
            continue;
        }

        /* Silent: the output comes back once the sources have their data */
        if (t->gain == 0.f && !t->waiting) {
            t->waiting = true;
            t->waited = 0;
        }

        const int64_t to_end = end - t->position;
        int64_t block = frames - done;
        if (block > to_end)
            block = to_end;
        /* Stopping where the ramp into the end of the range starts */
        if (to_end > t->declick && block > to_end - t->declick)
            block = to_end - t->declick;
        /* Ramping or waiting: short enough to follow within a ramp */
        if (t->declick && block > t->declick && (t->ramp_left || t->waiting || pending || !playing))
            block = t->declick;

        if (t->waiting && t->waited >= t->ready_timeout)
            t->waiting = false;

        float *dst = output + (size_t)done * t->channels;
        const int ret = nae_engine_render_jump(t->engine, t->position, (uint32_t)block, dst, t->wrap_next);
        t->wrap_next = false;
        if (ret < 0)
            result = ret;
        else if (ret == NAE_AGAIN && result == NAE_OK)
            result = NAE_AGAIN;
        if (t->waiting) {
            if (ret != NAE_AGAIN)
                t->waiting = false;
            else
                t->waited += block;
        }

        const bool fading_out = t->declick && to_end <= t->declick;
        ramp(t, dst, (uint32_t)block, playing && !pending && !t->waiting && !fading_out ? 1.f : 0.f);
        if (!clock_moving) {
            clock_position = t->position;
            clock_offset = done;
            clock_moving = true;
        }
        t->position += block;
        done += (uint32_t)block;
    }

    atomic_store_explicit(&t->position_done, t->position, memory_order_relaxed);
    if (time) {
        const int64_t smoothed = smooth_time(t, time, frames);
        publish_clock(t, clock_position, smoothed + (int64_t)(clock_offset * t->clock_period), clock_moving);
    } else {
        publish_clock(t, clock_position, 0, clock_moving);
    }
    return result;
}
