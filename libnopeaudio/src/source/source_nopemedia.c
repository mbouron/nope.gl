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
#include <stdckdint.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nopemedia/nopemedia.h>

#include "nopeaudio/nopeaudio.h"
#include "utils/memory.h"
#include "utils/pthread_compat.h"
#include "utils/semaphore.h"
#include "utils/spsc_queue.h"
#include "utils/thread.h"
#include "utils/utils.h"

/*
 * nope.media can block, so a dedicated thread decodes ahead of playback.
 * The render thread (the reader) exchanges decoded blocks with it through
 * two lock-free queues. In real time, it never waits for the decoder;
 * offline mode can wait for missing audio.
 *
 *    decoding thread                                    render thread
 *
 *                      blocks (decoded audio, in order)
 *    nope.media --> +----+----+----+----+----+----+ -->  media_read()
 *                   +----+----+----+----+----+----+
 *
 *                      released (blocks read or dropped)
 *    release    <-- +----+----+----+----+----+----+ <--  give_back()
 *                   +----+----+----+----+----+----+
 *
 *    seek       <--------- request (number, position) -- request()
 *
 * Blocks keep their nope.media frames alive. The released queue returns
 * them to the decoding thread so the reader never frees frames.
 *
 * The reader keeps a cursor, the media frame it reads next. When the audio
 * at the cursor is not decoded yet, silence is rendered and the cursor moves
 * on: the late audio is skipped when it arrives, so that the media stays in
 * sync with the project. After a seek, or when the decoder is too far
 * behind, the reader publishes a request. Requests are numbered and the
 * blocks carry the number of the request they answer, which lets the reader
 * drop those decoded for a position it has left. Timestamp gaps in the
 * decoded audio are rendered as silence.
 *
 * With a loop set, nope.media decodes the start of the loop right after its
 * end. The decoder counts these passes and tags the blocks with them: when
 * the transport wraps, the reader moves to the blocks of the next pass
 * instead of seeking.
 */

/*
 * Capacity of each queue, in blocks. The decoder never has more blocks out
 * than this, which is why the pushes cannot fail: see struct decoder.
 */
#define QUEUE_CAPACITY 64
#define LOOP_SLOTS 4       /* see get_loop_range() */
#define READ_AHEAD_MS 250  /* audio decoded ahead of the reader */

/*
 * Close gaps shorter than this and remove overlaps to compensate for
 * timestamps that are not sample accurate.
 */
#define CONTIGUOUS_MS 2

/*
 * How far behind the cursor a block can be before the reader asks the
 * decoder to seek instead of skipping the late audio.
 */
#define MAX_LATE_MS 500

/*
 * Initialization positions the decoder, including pre-roll, so this request
 * is already served when the decoding thread starts.
 */
#define FIRST_REQUEST 1

/*
 * Loudness normalization (dynaudnorm filter). The filter smooths its gain
 * with a minimum then a Gaussian window, each NORMALIZE_WINDOW filter frames
 * long: the gain at a given time depends on the audio up to
 * NORMALIZE_WINDOW - 1 filter frames before it. The filter also starts over
 * each time nope.media seeks.
 *
 * Start decoding NORMALIZE_PREROLL_MS before the requested position to
 * rebuild this history. Drop the pre-roll (see cut_preroll()) so playback
 * starts at the requested position, with a level closer to continuous playback.
 */
#define NORMALIZE_FRAME_MS 150
#define NORMALIZE_WINDOW 15
#define NORMALIZE_PREROLL_MS (NORMALIZE_FRAME_MS * NORMALIZE_WINDOW)

/* The audio of one nope.media frame */
struct chunk {
    const float *samples;    /* interleaved stereo frames */
    int64_t position;        /* media frame of its first frame */
    uint32_t frame_count;
    struct nmd_frame *frame; /* owns the samples */
};

/* What the queues carry: a chunk, or the end of the stream */
struct block {
    uint32_t request;        /* request served, see publish_request() */
    uint32_t pass;           /* loop pass of the chunk, see take_block() */
    int status;              /* NAU_OK with a chunk, else NAU_EOF or an error */
    struct chunk chunk;
};

/*
 * Thread ownership:
 *
 * - constant once initialized: engine, sample_rate, blocking, preroll, length
 * - control thread: refs, source_created, lock_ready, sems_ready, thread,
 *   thread_started; it also sets `stop`
 * - decoding thread: `decoder` (the control thread only uses it before the
 *   thread starts and after it has joined it), the producer end of `blocks`,
 *   the consumer end of `released`, and `errors`
 * - render thread (the reader): the fields under "Reader side", the other
 *   ends of the queues, the request it publishes, and the other statistics
 * - any thread: the loop range (setters) and the statistics (getters)
 */
struct nau_media {
    struct nau_engine *engine;
    uint32_t sample_rate;
    bool blocking;
    int64_t preroll;         /* frames decoded, then dropped, before a position */
    uint32_t refs;
    bool source_created;
    int64_t length;

    struct nmd_ctx *decoder;

    NAU_SPSC_QUEUE(struct block) blocks;      /* decoder -> reader */
    NAU_SPSC_QUEUE(struct block) released;    /* reader -> decoder */
    /* Wakes the decoder: blocks released, new request, new loop range, stop */
    nau_sem wake;
    /* Wakes a blocking reader: a block was queued */
    nau_sem queued;
    bool sems_ready;

    /*
     * The last request, written by the reader and read by the decoder: see
     * publish_request()
     */
    _Atomic uint32_t request_sequence;
    _Atomic uint32_t request_number;
    _Atomic int64_t request_position;

    /*
     * The loop range, read by the decoder and the reader: see
     * get_loop_range()
     */
    struct {
        _Atomic int64_t start;
        _Atomic int64_t end;
    } loops[LOOP_SLOTS];
    _Atomic uint32_t loop_revision;
    pthread_mutex_t loop_lock; /* serializes the setters */
    bool lock_ready;

    _Atomic bool stop;
    pthread_t thread;
    bool thread_started;

    /* Reader side */
    uint32_t number;         /* last request made */
    uint32_t pass;           /* loop pass being read, since the last request */
    bool pass_started;       /* a block of that pass was taken */
    int64_t cursor;          /* media frame read next */
    int64_t origin;          /* cursor at the last request or wrap, see count_skipped() */
    bool has_block;
    struct block block;      /* block being read, if has_block */
    int64_t block_read;      /* frames of it consumed so far, read or skipped */
    int64_t dropped;         /* frames of earlier passes dropped since the wrap */
    int end_status;          /* NAU_EOF at the end of the current pass, negative on error */

    /* Statistics, see struct nau_media_stats */
    _Atomic int64_t silent_frames;
    _Atomic int64_t skipped_frames;
    _Atomic int64_t requests;
    _Atomic int64_t errors;
};

static int64_t frames_in(const struct nau_media *m, int64_t ms)
{
    return ms * m->sample_rate / 1000;
}

/* Limit times passed to nope.media to avoid overflow in its conversions. */
#define MAX_SECONDS 1e9

static double seconds(const struct nau_media *m, int64_t frames)
{
    return fmin((double)frames / m->sample_rate, MAX_SECONDS);
}

/*
 * The decoder and reader must see start and end from the same loop range.
 * The setter writes the next slot, then publishes its revision:
 *
 *    revision:   5
 *    slots:    [ 4 | 5 | 2 | 3 ]      write 6 over 2, then publish 6
 *                    ^
 *
 * A reader copies the published slot and retries if the revision changed.
 * A setter paused before publication leaves the current slot readable, so
 * the render thread need not wait for it. The slot fields are atomic too:
 * a slow reader may still be reading a slot when a later update reuses it.
 */
static void get_loop_range(const struct nau_media *m, int64_t *start, int64_t *end)
{
    for (;;) {
        const uint32_t revision = atomic_load(&m->loop_revision);
        *start = atomic_load(&m->loops[revision % LOOP_SLOTS].start);
        *end = atomic_load(&m->loops[revision % LOOP_SLOTS].end);
        if (atomic_load(&m->loop_revision) == revision)
            return;
    }
}

static int64_t loop_length(const struct nau_media *m)
{
    int64_t start, end;
    get_loop_range(m, &start, &end);
    return end > start ? end - start : 0;
}

/* Decoding thread */

/*
 * Get the next chunk from nope.media; this can block. Return NAU_OK with a
 * chunk, NAU_EOF at the end of the media, NAU_ERROR_IO on error.
 */
static int decoder_next(struct nau_media *m, struct chunk *chunk)
{
    struct nmd_frame *frame = NULL;
    int ret;
    do {
        ret = nmd_get_next_frame(m->decoder, &frame);
    } while (ret == NMD_RET_UNCHANGED);
    if (ret == NMD_ERR_EOF)
        return NAU_EOF;
    if (ret != NMD_RET_NEWFRAME)
        return NAU_ERROR_IO;
    if (frame->pix_fmt != NMD_SMPFMT_FLT)
        goto fail;

    if (!frame->datap[0])
        goto fail;

    if (frame->nb_samples <= 0)
        goto fail;

    /* Despite its name, nmd_frame.ms is in microseconds */
    const int64_t us = NAU_MAX(frame->ms, 0);
    int64_t position;
    const int64_t fraction = (us % 1000000 * m->sample_rate + 500000) / 1000000;
    if (ckd_mul(&position, us / 1000000, m->sample_rate))
        goto fail;
    if (ckd_add(&position, position, fraction))
        goto fail;
    if (position > INT64_MAX - frame->nb_samples)
        goto fail;

    *chunk = (struct chunk){
        .samples = (const float *)frame->datap[0],
        .position = position,
        .frame_count = (uint32_t)frame->nb_samples,
        .frame = frame,
    };
    return NAU_OK;
fail:
    nmd_frame_releasep(&frame);
    return NAU_ERROR_IO;
}

static void release_chunk(struct chunk *chunk)
{
    nmd_frame_releasep(&chunk->frame);
}

static void release_block(struct block *block)
{
    if (block->status == NAU_OK)
        release_chunk(&block->chunk);
}

static void count_error(struct nau_media *m, int ret)
{
    if (ret < 0)
        atomic_fetch_add_explicit(&m->errors, 1, memory_order_relaxed);
}

/*
 * Requests. The reader numbers its requests, and the decoder tags each block
 * with the number of the request it was serving when it decoded the block:
 *
 *    reader                                  decoder (served = 7)
 *
 *    request():    number = 8
 *                  publish (8, cursor)  -->  8 != served: seek to the
 *                                            position, served = 8
 *
 *    take_block(): [7] dropped          <--  blocks decoded before the seek
 *                  [8] read             <--  blocks decoded after it
 *
 * Only the last request counts: it replaces the previous one, whether the
 * decoder has seen that one or not.
 *
 * A request is two values (number, position) which the decoder must read as
 * a pair. They are protected by a sequence number: the reader makes it odd,
 * writes both values, then makes it even again. The decoder retries while
 * the sequence is odd, or if it changed during the read. The reader never
 * waits.
 */
static void publish_request(struct nau_media *m, uint32_t number, int64_t position)
{
    const uint32_t sequence = atomic_load_explicit(&m->request_sequence, memory_order_relaxed);
    atomic_store_explicit(&m->request_sequence, sequence + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&m->request_number, number, memory_order_relaxed);
    atomic_store_explicit(&m->request_position, position, memory_order_relaxed);
    atomic_store_explicit(&m->request_sequence, sequence + 2, memory_order_release);
}

static void read_request(struct nau_media *m, uint32_t *number, int64_t *position)
{
    for (;;) {
        const uint32_t sequence = atomic_load_explicit(&m->request_sequence, memory_order_acquire);
        if (sequence & 1)
            continue;
        *number = atomic_load_explicit(&m->request_number, memory_order_relaxed);
        *position = atomic_load_explicit(&m->request_position, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&m->request_sequence, memory_order_relaxed) == sequence)
            return;
    }
}

/*
 * State of the decoding thread, which is the only one to use it.
 *
 * Queue capacity: `outstanding` counts the blocks pushed to `blocks` that
 * have not come back through `released` yet. Each of them is in one of three
 * places: in `blocks`, in the hands of the reader, or in `released`. The
 * decoder only decodes while outstanding + 2 <= capacity, and one decoded
 * chunk pushes at most two blocks (the held chunk and the new one, or the
 * held chunk and the end block). So outstanding never exceeds the capacity,
 * neither queue can hold more than `outstanding` blocks, and no push to
 * either queue can fail.
 */
struct decoder {
    struct nau_media *media;
    uint32_t served;         /* number of the request being served */
    uint32_t pass;           /* loop pass being decoded, since that request */
    size_t outstanding;      /* blocks pushed and not released yet */
    int64_t queued;          /* their frames, for the request served only */
    bool has_last;           /* a chunk was decoded in this pass */
    int64_t last_start;      /* its range: [last_start, last_end) */
    int64_t last_end;
    bool has_held;           /* first chunk of a pass, see queue_chunk() */
    uint32_t held_pass;
    struct chunk held;
    int64_t drop_before;     /* end of the pre-roll, 0 once reached */
    int64_t dropped_start;   /* start of the last pre-roll chunk dropped, -1 if none */
};

static void start_pass(struct decoder *d)
{
    d->pass++;
    d->has_last = false;
}

/*
 * Make nope.media decode from `position`, minus the pre-roll (which
 * cut_preroll() then drops)
 */
static void decode_from(struct decoder *d, int64_t position)
{
    struct nau_media *m = d->media;
    const int64_t start = position > m->preroll ? position - m->preroll : 0;
    if (start) {
        count_error(m, nmd_seek(m->decoder, seconds(m, start)));
    } else {
        /*
         * nope.media workaround: a seek to the start of the media loses the
         * first frame (AAC decodes a frame with the one before it). The
         * playback is stopped and started again instead, which decodes
         * from the very start, and the loop range is set again.
         */
        nmd_stop(m->decoder);
        count_error(m, nmd_start(m->decoder));
        int64_t loop_start, loop_end;
        get_loop_range(m, &loop_start, &loop_end);
        count_error(m, nmd_set_loop_range(m->decoder, seconds(m, loop_start), seconds(m, loop_end)));
    }
    d->drop_before = position;
    d->dropped_start = -1;
}

/*
 * Drop pre-roll before the requested position. Release whole chunks and
 * return false, or trim the first usable chunk and return true.
 */
static bool cut_preroll(struct decoder *d, struct chunk *chunk)
{
    if (!d->drop_before)
        return true;
    /*
     * nope.media went back to the start of the loop before reaching the
     * position: the audio ends before it, and this chunk starts the next pass
     */
    const int64_t contiguous = frames_in(d->media, CONTIGUOUS_MS);
    if (d->dropped_start >= 0 && chunk->position + contiguous < d->dropped_start) {
        d->drop_before = 0;
        start_pass(d);
        return true;
    }
    if (chunk->position + chunk->frame_count <= d->drop_before) {
        d->dropped_start = chunk->position;
        release_chunk(chunk);
        return false;
    }
    if (chunk->position < d->drop_before) {
        const int64_t offset = d->drop_before - chunk->position;
        chunk->samples += 2 * offset;
        chunk->frame_count -= (uint32_t)offset;
        chunk->position = d->drop_before;
    }
    /*
     * Done: the next passes of a loop start before this position and must
     * not be cut
     */
    d->drop_before = 0;
    return true;
}

static void push_block(struct decoder *d, const struct block *block)
{
    struct nau_media *m = d->media;
    /* Cannot fail: see the queue capacity in struct decoder */
    const bool pushed = nau_spsc_queue_push(&m->blocks, *block);
    nau_assert(pushed);
    d->outstanding++;
    if (block->status == NAU_OK)
        d->queued += block->chunk.frame_count;
    if (m->blocking)
        nau_sem_post(&m->queued);
}

static void push_chunk(struct decoder *d, uint32_t pass, const struct chunk *chunk)
{
    const struct block block = {
        .request = d->served,
        .pass    = pass,
        .chunk   = *chunk,
    };
    push_block(d, &block);
}

static void push_held(struct decoder *d)
{
    if (d->has_held) {
        push_chunk(d, d->held_pass, &d->held);
        d->has_held = false;
    }
}

/*
 * Queue a decoded chunk after determining its loop pass and position.
 *
 * Pass: nope.media does not tell when it goes back to the start of the loop.
 * A chunk starting before the previous one marks the next pass.
 *
 * Position: a chunk close enough to the end of the previous one is moved to
 * that end (see CONTIGUOUS_MS), so that the reader sees no gap or overlap.
 *
 * Held chunk (nope.media workaround): the first chunk after a seek or loop
 * wrap is timestamped at the requested position, but its audio can start
 * up to a chunk earlier. Hold it until the next chunk gives its actual end,
 * then move its start back if necessary:
 *
 *    as decoded:   |---- held ----|
 *                            |---- next ----|
 *
 *    as queued:  |---- held ----|---- next ----|
 *
 * The reader then skips the frames of the held chunk before its cursor.
 */
static void queue_chunk(struct decoder *d, struct chunk *chunk)
{
    const int64_t contiguous = frames_in(d->media, CONTIGUOUS_MS);

    const bool next_pass = d->has_last && chunk->position + contiguous < d->last_start;

    /* Corrects the held chunk, unless that would give it a negative position */
    if (d->has_held && !next_pass && chunk->position < d->last_end &&
        chunk->position >= (int64_t)d->held.frame_count) {
        d->held.position = chunk->position - d->held.frame_count;
        d->last_end = chunk->position;
    }
    push_held(d);

    if (next_pass)
        start_pass(d);
    /* First chunk of a pass: held back */
    if (!d->has_last) {
        d->has_held = true;
        d->held_pass = d->pass;
        d->held = *chunk;
        d->has_last = true;
        d->last_start = chunk->position;
        d->last_end = chunk->position + chunk->frame_count;
        return;
    }
    if (chunk->position < d->last_end + contiguous)
        chunk->position = d->last_end;
    d->last_start = chunk->position;
    d->last_end = chunk->position + chunk->frame_count;
    push_chunk(d, d->pass, chunk);
}

/*
 * The decoding thread. Each iteration collects the blocks the reader gave
 * back, applies a new loop range, serves a new request, then decodes one
 * chunk. It sleeps on `wake` when the media has ended, or when the queue or
 * the read-ahead is full.
 *
 * End of the loop (nope.media workarounds): nope.media only goes back to the
 * start of the loop when it knew the range before decoding its end. Three
 * cases where it does not are handled here:
 *
 * - A request at or past the loop end would keep decoding beyond the loop.
 *   Seek to the loop start and tag blocks with pass 1, ready for the reader
 *   to wrap.
 * - A chunk past the end of the loop, decoded before nope.media knew the
 *   range. It is dropped.
 * - The end of the media, reached before nope.media knew the range (a short
 *   media is decoded to its end as soon as it is opened). nope.media then
 *   restarts from the start of the media, not of the loop. The decoder
 *   seeks to the start of the loop instead.
 */
static void *run_decoder(void *arg)
{
    struct nau_media *m = arg;
    nau_thread_set_name("nau/media");
    const int64_t read_ahead = frames_in(m, READ_AHEAD_MS);
    const size_t capacity = nau_spsc_queue_capacity(&m->blocks);

    struct decoder d = {
        .media = m,
        /*
         * The reader may have requested another position before this thread
         * started. Only the initial request has been served.
         */
        .served = FIRST_REQUEST,
        /*
         * nau_media_init() started nope.media at the pre-roll of the
         * initial position
         */
        .drop_before = atomic_load_explicit(&m->request_position, memory_order_relaxed),
        .dropped_start = -1,
    };
    uint32_t loop_revision = 0;
    bool at_end = false;     /* the end block is queued */
    bool end_once = false;   /* the media just ended, nothing decoded since */

    while (!atomic_load(&m->stop)) {
        /* nope.media frames can only be released from this thread */
        struct block block;
        while (nau_spsc_queue_pop(&m->released, &block)) {
            if (block.status == NAU_OK && block.request == d.served)
                d.queued -= block.chunk.frame_count;
            release_block(&block);
            d.outstanding--;
        }

        const uint32_t revision = atomic_load_explicit(&m->loop_revision, memory_order_acquire);
        if (revision != loop_revision) {
            int64_t start, end;
            get_loop_range(m, &start, &end);
            count_error(m, nmd_set_loop_range(m->decoder, seconds(m, start), seconds(m, end)));
            loop_revision = revision;
            /*
             * A new loop makes an exhausted decoder usable again, from the
             * start of the loop: nope.media would restart from the start of
             * the media
             */
            if (end > start && at_end) {
                decode_from(&d, start);
                at_end = false;
            }
        }

        uint32_t number;
        int64_t position;
        read_request(m, &number, &position);
        if (number != d.served) {
            if (d.has_held) {
                release_chunk(&d.held);
                d.has_held = false;
            }
            /*
             * A request at or past the end of the loop: the engine leaves a
             * source there once its range is over. The next read comes
             * after the transport has wrapped, from the start of the loop,
             * in the next pass of the reader: this is what gets decoded.
             */
            int64_t loop_start, loop_end;
            get_loop_range(m, &loop_start, &loop_end);
            const bool next_pass = loop_end > loop_start && position >= loop_end;
            if (next_pass)
                position = loop_start;
            decode_from(&d, position);
            d.served = number;
            d.pass = next_pass;
            d.has_last = false;
            /*
             * The blocks of the earlier requests no longer count as
             * read-ahead. Some can stay queued for as long as the reader is
             * paused, and they must not keep the new position from being
             * decoded.
             */
            d.queued = 0;
            at_end = false;
        }

        /* A decoded chunk can push two blocks: see struct decoder */
        if (at_end || d.outstanding + 2 > capacity || d.queued >= read_ahead) {
            nau_sem_wait(&m->wake);
            continue;
        }

        struct chunk chunk = {0};
        int ret = decoder_next(m, &chunk);

        /* A request made while nope.media was blocking: the chunk is stale */
        read_request(m, &number, &position);
        if (number != d.served || atomic_load(&m->stop)) {
            if (ret == NAU_OK)
                release_chunk(&chunk);
            continue;
        }

        if (ret == NAU_OK && !cut_preroll(&d, &chunk))
            continue;

        int64_t loop_start, loop_end;
        get_loop_range(m, &loop_start, &loop_end);
        const bool looping = loop_end > loop_start;
        if (ret == NAU_OK && looping && chunk.position >= loop_end) {
            /*
             * Drop audio decoded past the loop end before the range was set,
             * leaving room for the next pass.
             */
            release_chunk(&chunk);
            continue;
        }

        /*
         * The end of the media while looping: nope.media reached it before
         * it knew the range, and would restart from the start of the media.
         * The next pass is decoded from the start of the loop instead. If
         * the end comes again with nothing decoded in between, it is final.
         */
        if (ret == NAU_EOF && looping && !end_once) {
            end_once = true;
            /* The position requested is past the end of the audio */
            if (d.drop_before)
                start_pass(&d);
            decode_from(&d, loop_start);
            continue;
        }
        end_once = false;

        if (ret != NAU_OK) {
            count_error(m, ret);
            push_held(&d);
            const struct block end_block = {
                .request = d.served,
                .pass    = d.pass,
                .status  = ret,
            };
            push_block(&d, &end_block);
            at_end = true;
            continue;
        }
        queue_chunk(&d, &chunk);
    }

    /* Stopping: the reader is gone, so both queues are emptied from here */
    if (d.has_held)
        release_chunk(&d.held);
    struct block block;
    while (nau_spsc_queue_pop(&m->released, &block))
        release_block(&block);
    while (nau_spsc_queue_pop(&m->blocks, &block))
        release_block(&block);
    return NULL;
}

/* Reader side (render thread) */

static void give_back(struct nau_media *m, const struct block *block)
{
    /* Cannot fail: see the queue capacity in struct decoder */
    const bool pushed = nau_spsc_queue_push(&m->released, *block);
    nau_assert(pushed);
    nau_sem_post(&m->wake);
}

static void drop_block(struct nau_media *m)
{
    if (m->has_block) {
        give_back(m, &m->block);
        m->has_block = false;
    }
}

/*
 * Count only late audio the reader has already passed. Frames before the
 * last seek or loop start were never requested and must not count.
 * They can still be queued because decoding starts at a packet boundary or
 * queue_chunk() corrects the first chunk's timestamp.
 */
static void count_skipped(struct nau_media *m, int64_t start, int64_t end)
{
    if (start < m->origin)
        start = m->origin;
    if (end > start)
        atomic_fetch_add_explicit(&m->skipped_frames, end - start, memory_order_relaxed);
}

/* Request decoding from the cursor, see publish_request(). */
static void request(struct nau_media *m)
{
    drop_block(m);
    /*
     * Return stale blocks now: playback may be paused, and the decoder
     * needs room to queue audio at the new position before the next read.
     */
    struct block block;
    while (nau_spsc_queue_pop(&m->blocks, &block))
        give_back(m, &block);
    m->number++;
    m->pass = 0;
    m->pass_started = false;
    m->origin = m->cursor;
    m->end_status = NAU_OK;
    publish_request(m, m->number, m->cursor);
    nau_sem_post(&m->wake);
    atomic_fetch_add_explicit(&m->requests, 1, memory_order_relaxed);
}

/*
 * Take the next block of the current loop pass. Return false if no block is
 * available for this pass, including when its audio has ended.
 *
 * Passes: with a loop set, the decoder runs ahead of the reader and reaches
 * the end of the loop first. Both count their passes from the last request,
 * and the reader only reads the blocks of its own pass. Here are the queued
 * blocks with their pass, around the end of the loop:
 *
 *                    end of the loop, for the decoder
 *                                 v
 *    blocks    ... [0] [0] [0] [0] [1] [1] [1] [1] ...
 *                       ^
 *                     reader, in pass 0
 *
 * When the transport wraps, media_seek() moves the reader to pass 1 and its
 * cursor to the start of the loop. The [0] blocks left are dropped and the
 * [1] blocks are read, without any request to the decoder.
 *
 * If a later pass arrives before any block of the current pass, the current
 * pass starts past the end of the audio. Keep that block for the next wrap
 * and render silence until then; another request would seek to the same gap.
 *
 * Otherwise, request decoding again if a later pass arrives early, or if
 * stale blocks show that the decoder did not loop with the reader.
 */
static bool take_block(struct nau_media *m)
{
    struct block block;
    while (nau_spsc_queue_pop(&m->blocks, &block)) {
        /* Decoded for an earlier request */
        if (block.request != m->number) {
            give_back(m, &block);
            continue;
        }
        /*
         * From a pass the reader has left. An error block is kept and
         * reported: the decoder stops after it, so nothing would follow.
         */
        if (block.pass < m->pass && block.status >= 0) {
            /*
             * The media ended and the loop is no longer set: the next pass
             * is not coming
             */
            if (block.status != NAU_OK && !loop_length(m)) {
                give_back(m, &block);
                request(m);
                return false;
            }
            if (block.status == NAU_OK)
                m->dropped += block.chunk.frame_count;
            give_back(m, &block);
            /*
             * More than a whole loop dropped: the decoder is not looping,
             * which happens when the loop is set after the decoder has
             * passed its end
             */
            if (m->dropped > loop_length(m) + frames_in(m, MAX_LATE_MS)) {
                request(m);
                return false;
            }
            continue;
        }
        /* From a later pass: the rest of the pass being read is not coming */
        if (block.pass > m->pass) {
            if (!m->pass_started) {
                m->block = block;
                m->has_block = true;
                m->block_read = 0;
                m->end_status = NAU_EOF;
                return false;
            }
            give_back(m, &block);
            request(m);
            return false;
        }
        m->block = block;
        m->has_block = true;
        m->block_read = 0;
        m->pass_started = true;
        return true;
    }
    return false;
}

static int media_read(void *user, int64_t frame, uint32_t frames, float *output)
{
    struct nau_media *m = user;
    const int64_t max_late = frames_in(m, MAX_LATE_MS);

    /*
     * A read that does not follow the previous one, without a seek: this
     * happens after the range of the source was changed
     */
    if (frame != m->cursor) {
        m->cursor = frame;
        request(m);
    }

    int64_t cursor = frame;
    int64_t done = 0;
    bool silent = false;
    while (done < frames && !m->end_status) {
        /* take_block() can make a request, which is for m->cursor */
        if (!m->has_block)
            m->cursor = cursor;
        /*
         * Nothing decoded yet. A blocking reader waits. Otherwise the rest
         * of the read is silent, and the cursor moves on all the same.
         */
        if (!m->has_block && !take_block(m)) {
            if (m->end_status)
                break;
            if (m->blocking) {
                nau_sem_wait(&m->queued);
                continue;
            }
            const int64_t count = frames - done;
            cursor += count;
            done += count;
            silent = true;
            atomic_fetch_add_explicit(&m->silent_frames, count, memory_order_relaxed);
            break;
        }

        const struct block *block = &m->block;
        if (block->status != NAU_OK) {
            m->end_status = block->status;
            drop_block(m);
            break;
        }

        const int64_t start = block->chunk.position;
        const int64_t end = start + block->chunk.frame_count;
        /* The block is behind the cursor: late audio, skipped */
        if (cursor >= end) {
            if (cursor - end > max_late) {
                m->cursor = cursor;
                request(m);
                continue;
            }
            count_skipped(m, start + m->block_read, end);
            drop_block(m);
            continue;
        }
        /*
         * The block is ahead of the cursor: a gap in the media, left silent.
         * Keep the block until its time comes: seeking to the cursor would
         * return the same gap, indefinitely with blocking reads.
         */
        if (cursor < start) {
            const int64_t count = NAU_MIN(start - cursor, frames - done);
            cursor += count;
            done += count;
            continue;
        }

        const int64_t offset = cursor - start;
        if (m->block_read == 0)
            count_skipped(m, start, cursor);
        const int64_t count = end - cursor < frames - done ? end - cursor : frames - done;
        memcpy(output + 2 * done, block->chunk.samples + 2 * offset, (size_t)count * 2 * sizeof(*output));
        cursor += count;
        done += count;
        m->block_read += count + (m->block_read == 0 ? offset : 0);
        if (cursor == end)
            drop_block(m);
    }

    /* Keep advancing past EOF so the next sequential read does not seek. */
    m->cursor = m->end_status ? frame + frames : cursor;
    if (m->end_status < 0)
        return m->end_status;
    if (silent)
        return NAU_AGAIN;
    return done < frames ? NAU_EOF : NAU_OK;
}

/*
 * Check whether the block containing frame is queued. Skip late blocks
 * as media_read() would.
 */
static int media_ready(void *user, int64_t frame)
{
    struct nau_media *m = user;
    if (m->blocking || m->end_status)
        return 1;
    if (frame != m->cursor) {
        m->cursor = frame;
        request(m);
    }
    while (m->has_block || take_block(m)) {
        const struct block *block = &m->block;
        const int64_t start = block->chunk.position;
        const int64_t end = start + block->chunk.frame_count;
        if (block->status != NAU_OK || frame < end)
            return 1;
        if (frame - end > frames_in(m, MAX_LATE_MS)) {
            request(m);
            return 0;
        }
        count_skipped(m, start + m->block_read, end);
        drop_block(m);
    }
    return m->end_status != NAU_OK;
}

static void media_seek(void *user, int64_t frame, int wrap)
{
    struct nau_media *m = user;
    if (wrap && loop_length(m)) {
        /*
         * The transport wrapped: the reader moves to the next pass, which
         * the decoder has queued or is decoding (see take_block())
         */
        m->pass++;
        /* Reuse a block held for this pass when the previous one ended early */
        if (m->has_block && m->block.pass != m->pass)
            drop_block(m);
        m->pass_started = m->has_block;
        m->cursor = frame;
        m->origin = frame;
        m->dropped = 0;
        m->end_status = NAU_OK;
        return;
    }
    /*
     * A seek to where the cursor already is needs no request, except at the
     * end of the loop (where the engine leaves a source once its range is
     * over). The decoder then has to decode the next pass from the start of
     * the loop, and a request is what makes it do so: see run_decoder().
     */
    int64_t loop_start, loop_end;
    get_loop_range(m, &loop_start, &loop_end);
    const bool at_loop_end = loop_end > loop_start && frame >= loop_end;
    if (frame == m->cursor && !m->end_status && !at_loop_end)
        return;
    m->cursor = frame;
    request(m);
}

static void media_destroy(void *user)
{
    struct nau_media *m = user;
    nau_media_unrefp(&m);
}

/* Control thread */

/*
 * Release decoder resources after the last reference or a failed initialization.
 * On initialization failure, preserve the allocation and reference for a retry.
 */
static void media_uninit(struct nau_media *m)
{
    if (m->thread_started) {
        /*
         * The block being read is released by the decoding thread as well.
         * It is outstanding, so `released` has room for it: see the queue
         * capacity in struct decoder.
         */
        if (m->has_block) {
            const bool pushed = nau_spsc_queue_push(&m->released, m->block);
            nau_assert(pushed);
        }
        atomic_store(&m->stop, true);
        nau_sem_post(&m->wake);
        pthread_join(m->thread, NULL);
        m->thread_started = false;
    }
    nmd_freep(&m->decoder);
    if (m->sems_ready) {
        nau_sem_destroy(&m->wake);
        nau_sem_destroy(&m->queued);
        m->sems_ready = false;
    }
    nau_spsc_queue_uninit(&m->blocks);
    nau_spsc_queue_uninit(&m->released);
    if (m->lock_ready) {
        pthread_mutex_destroy(&m->loop_lock);
        m->lock_ready = false;
    }
    m->engine = NULL;
    m->length = 0;
}

struct nau_media *nau_media_create(void)
{
    struct nau_media *m = nau_try_calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->refs = 1;
    atomic_init(&m->request_sequence, 0);
    atomic_init(&m->request_number, 0);
    atomic_init(&m->request_position, 0);
    for (uint32_t i = 0; i < LOOP_SLOTS; i++) {
        atomic_init(&m->loops[i].start, 0);
        atomic_init(&m->loops[i].end, 0);
    }
    atomic_init(&m->loop_revision, 0);
    atomic_init(&m->stop, false);
    atomic_init(&m->silent_frames, 0);
    atomic_init(&m->skipped_frames, 0);
    atomic_init(&m->requests, 0);
    atomic_init(&m->errors, 0);
    return m;
}

int nau_media_init(struct nau_media *m, struct nau_engine *engine, const char *filename,
                    const struct nau_media_params *params)
{
    if (!m || m->engine)
        return NAU_ERROR_INVALID;

    const struct nau_engine_config *config = nau_engine_get_config(engine);
    const struct nau_media_params defaults = {0};
    if (!params)
        params = &defaults;
    if (!config)
        return NAU_ERROR_INVALID;

    if (config->channels != 2)
        return NAU_ERROR_INVALID;

    if (!filename)
        return NAU_ERROR_INVALID;

    if (params->start < 0)
        return NAU_ERROR_INVALID;

    int ret = pthread_mutex_init(&m->loop_lock, NULL);
    if (ret != 0)
        return NAU_ERROR_MEMORY;
    m->lock_ready = true;

    m->engine = engine;
    m->sample_rate = config->sample_rate;
    m->blocking = params->blocking;
    m->preroll = params->normalize ? frames_in(m, NORMALIZE_PREROLL_MS) : 0;
    m->number = FIRST_REQUEST;
    m->origin = params->start;
    atomic_store(&m->request_number, FIRST_REQUEST);
    atomic_store(&m->request_position, params->start);
    m->cursor = params->start;

    if (!atomic_is_lock_free(&m->request_position)) {
        ret = NAU_ERROR_UNSUPPORTED;
        goto fail;
    }

    ret = nau_spsc_queue_init(&m->blocks, QUEUE_CAPACITY);
    if (ret < 0)
        goto fail;
    ret = nau_spsc_queue_init(&m->released, QUEUE_CAPACITY);
    if (ret < 0)
        goto fail;

    ret = nau_sem_init(&m->wake);
    if (ret < 0)
        goto fail;
    ret = nau_sem_init(&m->queued);
    if (ret < 0) {
        nau_sem_destroy(&m->wake);
        goto fail;
    }
    m->sems_ready = true;

    m->decoder = nmd_create(filename);
    if (!m->decoder) {
        ret = NAU_ERROR_MEMORY;
        goto fail;
    }

    char normalize[64] = "";
    if (params->normalize)
        snprintf(normalize, sizeof(normalize), "dynaudnorm=f=%d:g=%d,", NORMALIZE_FRAME_MS, NORMALIZE_WINDOW);
    char filters[128];
    snprintf(filters, sizeof(filters), "%saformat=sample_fmts=flt:channel_layouts=stereo:sample_rates=%u",
             normalize, m->sample_rate);
    ret = nmd_set_option(m->decoder, "avselect", NMD_SELECT_AUDIO);
    if (ret < 0)
        goto fail_io;
    ret = nmd_set_option(m->decoder, "audio_texture", 0);
    if (ret < 0)
        goto fail_io;
    ret = nmd_set_option(m->decoder, "auto_hwaccel", 0);
    if (ret < 0)
        goto fail_io;
    ret = nmd_set_option(m->decoder, "filters", filters);
    if (ret < 0)
        goto fail_io;

    const int64_t start = params->start > m->preroll ? params->start - m->preroll : 0;
    if (start) {
        ret = nmd_seek(m->decoder, seconds(m, start));
        if (ret < 0)
            goto fail_io;
    }

    ret = nmd_start(m->decoder);
    if (ret < 0)
        goto fail_io;

    struct nmd_info info;
    ret = nmd_get_info(m->decoder, &info);
    if (ret < 0)
        goto fail_io;
    if (info.duration > 0) {
        const double length = round(info.duration * m->sample_rate);
        if (!isfinite(length) || length >= NAU_INT64_MAX_EXCLUSIVE) {
            ret = NAU_ERROR_UNSUPPORTED;
            goto fail;
        }
        m->length = (int64_t)length;
    }

    /* From here on, only the decoding thread calls nope.media */
    ret = pthread_create(&m->thread, NULL, run_decoder, m);
    if (ret != 0) {
        ret = NAU_ERROR_MEMORY;
        goto fail;
    }
    m->thread_started = true;

    return NAU_OK;

fail_io:
    ret = NAU_ERROR_IO;
fail:
    media_uninit(m);
    return ret;
}

int64_t nau_media_get_length(const struct nau_media *m)
{
    return m ? m->length : 0;
}

int nau_media_init_source(struct nau_media *m, struct nau_node *node, int64_t start, int64_t offset, int64_t duration)
{
    if (!m || !m->engine)
        return NAU_ERROR_INVALID;

    if (m->source_created)
        return NAU_ERROR_INVALID;

    const struct nau_source source = {
        .user           = m,
        .read           = media_read,
        .seek           = media_seek,
        .ready          = media_ready,
        .destroy        = media_destroy,
        .timeline_start = start,
        .source_start   = offset,
        .duration       = duration,
    };
    int ret = nau_source_init(node, m->engine, &source);
    if (ret == NAU_OK) {
        m->refs++;
        m->source_created = true;
    }
    return ret;
}

int nau_media_set_loop(struct nau_media *m, int64_t start, int64_t end)
{
    if (!m || !m->engine)
        return NAU_ERROR_INVALID;

    if (start < 0 || end < 0)
        return NAU_ERROR_INVALID;

    if (end <= start)
        start = end = 0;
    pthread_mutex_lock(&m->loop_lock);
    int64_t current_start, current_end;
    get_loop_range(m, &current_start, &current_end);
    const bool changed = start != current_start || end != current_end;
    if (changed) {
        const uint32_t revision = atomic_load(&m->loop_revision) + 1;
        atomic_store(&m->loops[revision % LOOP_SLOTS].start, start);
        atomic_store(&m->loops[revision % LOOP_SLOTS].end, end);
        atomic_store(&m->loop_revision, revision);
    }
    pthread_mutex_unlock(&m->loop_lock);
    if (changed)
        nau_sem_post(&m->wake);
    return NAU_OK;
}

void nau_media_get_stats(const struct nau_media *m, struct nau_media_stats *stats)
{
    if (!m || !stats)
        return;
    *stats = (struct nau_media_stats){
        .silent_frames = atomic_load_explicit(&m->silent_frames, memory_order_relaxed),
        .skipped_frames = atomic_load_explicit(&m->skipped_frames, memory_order_relaxed),
        .requests = atomic_load_explicit(&m->requests, memory_order_relaxed),
        .errors = atomic_load_explicit(&m->errors, memory_order_relaxed),
    };
}

void nau_media_unrefp(struct nau_media **mp)
{
    if (!mp || !*mp)
        return;
    struct nau_media *m = *mp;
    *mp = NULL;
    if (--m->refs)
        return;
    media_uninit(m);
    nau_freep(&m);
}
