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

#define CLOCK_HISTORY_SIZE 256

struct clock_entry {
    _Atomic int64_t position;
    _Atomic int64_t time;
    _Atomic int64_t end_time;
    _Atomic uint32_t frames;
    _Atomic int playing;
};

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
    _Atomic uint32_t clock_count;
    _Atomic uint32_t clock_write;
    struct clock_entry clocks[CLOCK_HISTORY_SIZE];

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
    atomic_init(&t->clock_count, 0);
    atomic_init(&t->clock_write, 0);
    for (unsigned i = 0; i < CLOCK_HISTORY_SIZE; i++) {
        atomic_init(&t->clocks[i].position, 0);
        atomic_init(&t->clocks[i].time, 0);
        atomic_init(&t->clocks[i].end_time, 0);
        atomic_init(&t->clocks[i].frames, 0);
        atomic_init(&t->clocks[i].playing, 0);
    }
    if (!atomic_is_lock_free(&t->seek) || !atomic_is_lock_free(&t->duration)) {
        free(t);
        return NAE_ERROR_UNSUPPORTED;
    }
    /* Playback starts from silence */
    t->waiting = true;
    *transport = t;
    return NAE_OK;
}

struct nae_engine *nae_transport_get_engine(const struct nae_transport *t)
{
    return t ? t->engine : NULL;
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

static void read_clock(const struct nae_transport *t, uint32_t index, struct nae_clock *clock)
{
    const struct clock_entry *entry = &t->clocks[index];
    *clock = (struct nae_clock){
        .position = atomic_load_explicit(&entry->position, memory_order_relaxed),
        .time = atomic_load_explicit(&entry->time, memory_order_relaxed),
        .playing = atomic_load_explicit(&entry->playing, memory_order_relaxed),
        .sample_rate = t->sample_rate,
    };
}

int nae_transport_get_clock(const struct nae_transport *t, struct nae_clock *clock)
{
    if (!t || !clock)
        return NAE_ERROR_INVALID;
    for (int attempt = 0; attempt < 64; attempt++) {
        const uint32_t sequence = atomic_load_explicit(&t->clock_sequence, memory_order_acquire);
        if (sequence & 1)
            continue;
        const uint32_t count = atomic_load_explicit(&t->clock_count, memory_order_relaxed);
        const uint32_t next = atomic_load_explicit(&t->clock_write, memory_order_relaxed);
        struct nae_clock snapshot;
        read_clock(t, (next + CLOCK_HISTORY_SIZE - 1) % CLOCK_HISTORY_SIZE, &snapshot);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&t->clock_sequence, memory_order_relaxed) == sequence) {
            if (!count)
                return NAE_AGAIN;
            *clock = snapshot;
            return NAE_OK;
        }
    }
    return NAE_AGAIN;
}

int64_t nae_clock_position_at(const struct nae_clock *clock, int64_t time)
{
    if (!clock)
        return 0;
    if (!clock->playing)
        return clock->position;
    const long double position = clock->position + ((long double)time - clock->time) * clock->sample_rate / 1e9L;
    if (position <= 0)
        return 0;
    if (position >= 0x1p63L)
        return INT64_MAX;
    return (int64_t)position;
}

int nae_transport_get_position_at(const struct nae_transport *t, int64_t time, int64_t *position)
{
    if (!t || !position)
        return NAE_ERROR_INVALID;
    for (int attempt = 0; attempt < 64; attempt++) {
        const uint32_t sequence = atomic_load_explicit(&t->clock_sequence, memory_order_acquire);
        if (sequence & 1)
            continue;
        const uint32_t count = atomic_load_explicit(&t->clock_count, memory_order_relaxed);
        const uint32_t next = atomic_load_explicit(&t->clock_write, memory_order_relaxed);
        int ret = NAE_AGAIN;
        int64_t value = 0;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t index = (next + CLOCK_HISTORY_SIZE - 1 - i) % CLOCK_HISTORY_SIZE;
            struct nae_clock clock;
            read_clock(t, index, &clock);
            if (!clock.time)
                break;
            if (time < clock.time) {
                if (i == count - 1 && count < CLOCK_HISTORY_SIZE) {
                    value = clock.position;
                    ret = NAE_OK;
                }
                continue;
            }
            const struct clock_entry *entry = &t->clocks[index];
            const int64_t end = atomic_load_explicit(&entry->end_time, memory_order_relaxed);
            const uint32_t frames = atomic_load_explicit(&entry->frames, memory_order_relaxed);
            value = time >= end ? clock.position + (clock.playing ? frames : 0)
                                : nae_clock_position_at(&clock, time);
            ret = NAE_OK;
            break;
        }
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&t->clock_sequence, memory_order_relaxed) == sequence) {
            if (ret == NAE_OK)
                *position = value;
            return ret;
        }
    }
    return NAE_AGAIN;
}

struct clock_writer {
    uint32_t sequence;
    uint32_t next;
    uint32_t count;
    bool merge;
    int64_t time;
    double period;
};

static struct clock_writer begin_clock(struct nae_transport *t, int64_t time)
{
    struct clock_writer writer = {
        .sequence = atomic_load_explicit(&t->clock_sequence, memory_order_relaxed),
        .next = atomic_load_explicit(&t->clock_write, memory_order_relaxed),
        .count = atomic_load_explicit(&t->clock_count, memory_order_relaxed),
        .time = time,
        .period = t->clock_period,
    };
    return writer;
}

static int64_t clamp_time(long double time)
{
    if (time >= 0x1p63L)
        return INT64_MAX;
    if (time <= -0x1p63L)
        return INT64_MIN;
    return (int64_t)time;
}

static int64_t clock_time_at(const struct clock_writer *writer, uint32_t offset)
{
    return writer->time ? clamp_time(writer->time + (long double)offset * writer->period) : 0;
}

static void start_clock_write(struct nae_transport *t, const struct clock_writer *writer)
{
    atomic_store_explicit(&t->clock_sequence, writer->sequence + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}

static void end_clock(struct nae_transport *t, struct clock_writer *writer)
{
    atomic_store_explicit(&t->clock_count, writer->count, memory_order_relaxed);
    atomic_store_explicit(&t->clock_write, writer->next, memory_order_relaxed);
    writer->sequence += 2;
    atomic_store_explicit(&t->clock_sequence, writer->sequence, memory_order_release);
}

static void record_clock(struct nae_transport *t, struct clock_writer *writer, uint32_t offset,
                         uint32_t frames, int64_t position, int playing)
{
    if (!frames)
        return;
    start_clock_write(t, writer);
    struct clock_entry *entry = &t->clocks[(writer->next + CLOCK_HISTORY_SIZE - 1) % CLOCK_HISTORY_SIZE];
    if (writer->merge && atomic_load_explicit(&entry->playing, memory_order_relaxed) == playing) {
        const int64_t previous = atomic_load_explicit(&entry->position, memory_order_relaxed);
        const uint32_t count = atomic_load_explicit(&entry->frames, memory_order_relaxed);
        if (position == previous + (playing ? count : 0)) {
            atomic_store_explicit(&entry->frames, count + frames, memory_order_relaxed);
            atomic_store_explicit(&entry->end_time, clock_time_at(writer, offset + frames), memory_order_relaxed);
            end_clock(t, writer);
            return;
        }
    }
    entry = &t->clocks[writer->next];
    atomic_store_explicit(&entry->position, position, memory_order_relaxed);
    atomic_store_explicit(&entry->time, clock_time_at(writer, offset), memory_order_relaxed);
    atomic_store_explicit(&entry->end_time, clock_time_at(writer, offset + frames), memory_order_relaxed);
    atomic_store_explicit(&entry->frames, frames, memory_order_relaxed);
    atomic_store_explicit(&entry->playing, playing, memory_order_relaxed);
    writer->next = (writer->next + 1) % CLOCK_HISTORY_SIZE;
    if (writer->count < CLOCK_HISTORY_SIZE)
        writer->count++;
    writer->merge = true;
    end_clock(t, writer);
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
    const double w = 2 * 3.14159265358979323846 * CLOCK_BANDWIDTH_HZ * frames / t->sample_rate;
    if (!t->clock_ready || error > CLOCK_RESET_NS || error < -CLOCK_RESET_NS || w >= 1.) {
        t->clock_ready = true;
        t->clock_period = nominal;
        t->clock_next = (double)time + nominal * frames;
        return time;
    }
    const double smoothed = t->clock_next + sqrt(2.) * w * error;
    t->clock_period += w * w * error / frames;
    t->clock_next = smoothed + t->clock_period * frames;
    return clamp_time(smoothed);
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
static bool silent(const struct nae_transport *t, uint32_t declick)
{
    return t->gain == 0.f || !declick;
}

/* Ramps the output gain to `target` over the declick length, applying it */
static void ramp(struct nae_transport *t, float *output, uint32_t frames, float target, uint32_t declick)
{
    if (target != t->target) {
        t->target = target;
        t->ramp_left = (uint32_t)ceilf(fabsf(target - t->gain) * (float)declick);
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

void nae_transport_sync(struct nae_transport *t)
{
    if (!t)
        return;
    /* Nothing rendered meanwhile: restarting from silence */
    t->gain = t->target = 0.f;
    t->ramp_left = 0;
    const uint64_t seek = atomic_load_explicit(&t->seek, memory_order_acquire);
    const uint16_t number = (uint16_t)(seek >> FRAME_BITS);
    if (number != t->seek_seen) {
        const int64_t duration = atomic_load(&t->duration);
        const int64_t frame = (int64_t)(seek & FRAME_MASK);
        t->position = frame < duration ? frame : duration;
        t->seek_seen = number;
        t->holding = false;
        t->wrap_next = false;
        atomic_store_explicit(&t->position_done, t->position, memory_order_relaxed);
        atomic_store_explicit(&t->seek_done, number, memory_order_release);
    }
    nae_engine_prepare(t->engine, t->position, 0, 1);
    struct clock_writer writer = begin_clock(t, 0);
    start_clock_write(t, &writer);
    writer.count = 0;
    end_clock(t, &writer);
    t->clock_ready = false;
}

int nae_transport_render(struct nae_transport *t, int64_t time, uint32_t frames, float *output)
{
    if (!t || (frames && !output) || (size_t)frames > SIZE_MAX / sizeof(*output) / t->channels)
        return NAE_ERROR_INVALID;
    if (!frames)
        return NAE_OK;
    memset(output, 0, (size_t)frames * t->channels * sizeof(*output));

    const int64_t duration = atomic_load(&t->duration);
    const int looping = atomic_load(&t->looping);
    const int64_t hold = atomic_load(&t->loop_hold);
    int64_t start, end;
    play_range(t, looping, duration, &start, &end);
    /* Leave room for both ramps, including in short loops. */
    const uint32_t declick = end - start < 2 * (int64_t)t->declick
                           ? (uint32_t)((end - start) / 2) : t->declick;

    const int64_t smoothed = time ? smooth_time(t, time, frames) : 0;
    struct clock_writer writer = begin_clock(t, smoothed);

    int result = NAE_OK;
    uint32_t done = 0;
    while (done < frames) {
        const uint64_t seek = atomic_load_explicit(&t->seek, memory_order_acquire);
        const uint16_t number = (uint16_t)(seek >> FRAME_BITS);
        bool pending = number != t->seek_seen;
        /* A seek is taken once silent, for the jump not to be heard */
        if (pending && silent(t, declick)) {
            const int64_t frame = (int64_t)(seek & FRAME_MASK);
            t->position = frame < duration ? frame : duration;
            t->seek_seen = number;
            /* The position before the seek is done, for get_position() to tell either */
            atomic_store_explicit(&t->position_done, t->position, memory_order_relaxed);
            atomic_store_explicit(&t->seek_done, number, memory_order_release);
            t->holding = false;
            t->wrap_next = false;
            pending = false;
            nae_engine_prepare(t->engine, t->position, 0, 0);
        }

        const int playing = atomic_load(&t->playing);
        if (!playing && silent(t, declick)) {
            /* Paused: the commits are adopted all the same, their new nodes
             * positioned where the playback resumes */
            nae_engine_prepare(t->engine, t->position, 0, 0);
            record_clock(t, &writer, done, frames - done, t->position, 0);
            break;
        }

        if (t->position >= end) {
            if (!looping) {
                /* The end: back to the start, stopped */
                t->position = 0;
                atomic_store(&t->playing, 0);
                nae_engine_prepare(t->engine, 0, 0, 0);
                record_clock(t, &writer, done, frames - done, 0, 0);
                break;
            }
            if (!t->holding && hold > 0) {
                /* The sources go round the loop while it holds still */
                t->holding = true;
                t->hold_left = hold;
                nae_engine_prepare(t->engine, start, 1, 0);
            }
            if (t->holding) {
                /* Likewise while holding still, the new nodes at the start */
                nae_engine_prepare(t->engine, start, 1, 0);
                if (t->hold_left > hold)
                    t->hold_left = hold;
                const int64_t count = t->hold_left < frames - done ? t->hold_left : frames - done;
                record_clock(t, &writer, done, (uint32_t)count, t->position, 0);
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
            if (t->position >= end) {
                record_clock(t, &writer, done, frames - done, t->position, 0);
                break;
            }
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
        if (to_end > declick && block > to_end - declick)
            block = to_end - declick;
        /* Ramping or waiting: short enough to follow within a ramp */
        if (declick && block > declick && (t->ramp_left || t->waiting || pending || !playing))
            block = declick;

        if (t->waiting && t->waited >= t->ready_timeout)
            t->waiting = false;

        float *dst = output + (size_t)done * t->channels;
        /* Fading out into a pause, the graph faded is the one playing: an
         * edit made meanwhile, as an editor pauses for, is adopted once silent */
        const int ret = nae_engine_render_jump(t->engine, t->position, (uint32_t)block, dst, t->wrap_next,
                                               playing || silent(t, declick));
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

        const bool fading_out = declick && to_end <= declick;
        ramp(t, dst, (uint32_t)block, playing && !pending && !t->waiting && !fading_out ? 1.f : 0.f, declick);
        record_clock(t, &writer, done, (uint32_t)block, t->position, 1);
        t->position += block;
        done += (uint32_t)block;
    }

    atomic_store_explicit(&t->position_done, t->position, memory_order_relaxed);
    return result;
}
