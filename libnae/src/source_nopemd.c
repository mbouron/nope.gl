/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE  /* pthread_setname_np() with glibc */
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nopemd.h>
#include "nae/nopemd.h"
#include "spsc.h"
#include "thread.h"

/*
 * The decoding thread makes every call to nope.media, which can block: it
 * decodes the chunks of audio of the media ahead of the reader and queues
 * them as blocks. The reader, the render thread, takes the blocks from that
 * queue, and hands back those it is done with for the decoding thread to
 * release them: it never waits on it, takes no lock and frees nothing.
 *
 * The reader knows the frame of the media it needs next, its cursor. Audio
 * not decoded in time plays as silence, and the audio then late for the
 * cursor is skipped, for the media to stay in time. Audio the queue cannot
 * provide, as after a seek, is asked of the decoding thread by a request:
 * the blocks it queues once it has served it carry its number, the older
 * ones are dropped.
 *
 *   nope.media -> decoder -> blocks -> reader -> audio
 *                    ^                   |
 *                    '---- released -----'
 *                    '---- requests -----'
 *
 * Looping over a part of the media, nope.media goes round it: the decoder
 * numbers the passes over it, from where the chunks go back before the
 * previous one, and the reader going round the loop takes the blocks of its
 * next pass.
 */

#define QUEUE_CAPACITY 64  /* blocks */
#define READ_AHEAD_MS 250  /* audio decoded ahead of the reader */
/* A chunk starting before the end of the previous one, or after it by less
 * than this, follows it: timestamps can be less precise than a frame */
#define CONTIGUOUS_MS 2
#define MAX_GAP_MS 100     /* audio further ahead of the cursor is asked for */
#define MAX_LATE_MS 500    /* audio further behind the cursor is asked for rather than skipped */
#define FIRST_REQUEST 1    /* the position of the creation, nope.media's own when it starts */

struct chunk {
    const float *samples;    /* interleaved stereo frames */
    int64_t position;        /* of its first frame in the media */
    uint32_t nb_frames;
    struct nmd_frame *frame;
};

struct block {
    uint32_t request;        /* number of the request the decoder had served when it got the chunk */
    uint32_t pass;           /* pass of the loop since that request */
    int status;              /* NAE_OK for PCM, NAE_EOF or an error otherwise */
    struct chunk chunk;
};

struct nae_media {
    struct nae_engine *engine;
    uint32_t sample_rate;
    bool blocking;
    uint32_t refs;           /* control thread only */
    bool source_created;
    int64_t length;

    struct nmd_ctx *decoder; /* the decoding thread's, once started */

    struct spsc blocks;      /* decoder -> reader */
    struct spsc released;    /* reader -> decoder */
    nae_sem wake;            /* the decoder: blocks released, a request, the loop changed, stopping */
    nae_sem queued;          /* a blocking reader: a block queued */
    bool sems_ready;

    /* The last request, published by the reader: the sequence is odd while it is written */
    _Atomic uint32_t request_sequence;
    _Atomic uint32_t request_number;
    _Atomic int64_t request_position;

    _Atomic int64_t loop_start;
    _Atomic int64_t loop_end;
    _Atomic uint32_t loop_revision;

    _Atomic bool stop;
    nae_thread thread;
    bool thread_started;

    /* Reader side */
    uint32_t number;         /* of the last request: the blocks of the earlier ones are dropped */
    uint32_t pass;           /* pass of the loop read */
    _Atomic int64_t cursor;  /* the frame of the media read next, written by the reader only */
    bool has_block;
    struct block block;      /* being read */
    int64_t block_read;      /* frames read from it */
    int64_t dropped;         /* frames of the previous passes dropped since going round the loop */
    int end_status;

    _Atomic int64_t silent_frames;
    _Atomic int64_t skipped_frames;
    _Atomic int64_t requests;
    _Atomic int64_t errors;
};

static int64_t frames_in(const struct nae_media *m, int64_t ms)
{
    return ms * m->sample_rate / 1000;
}

static double seconds(const struct nae_media *m, int64_t frames)
{
    return (double)frames / m->sample_rate;
}

/* nope.media, from the decoding thread */

/* The next chunk: 0, 1 at the end of the media, < 0 on error. Can block */
static int decoder_next(struct nae_media *m, struct chunk *chunk)
{
    struct nmd_frame *frame = NULL;
    int ret;
    do {
        ret = nmd_get_next_frame(m->decoder, &frame);
    } while (ret == NMD_RET_UNCHANGED);
    if (ret == NMD_ERR_EOF)
        return 1;
    if (ret != NMD_RET_NEWFRAME)
        return ret < 0 ? ret : -1;
    if (frame->pix_fmt != NMD_SMPFMT_FLT || !frame->datap[0] || frame->nb_samples <= 0) {
        nmd_frame_releasep(&frame);
        return -1;
    }
    const int64_t us = frame->ms > 0 ? frame->ms : 0;
    *chunk = (struct chunk){
        .samples = (const float *)frame->datap[0],
        .position = (us * m->sample_rate + 500000) / 1000000,
        .nb_frames = (uint32_t)frame->nb_samples,
        .frame = frame,
    };
    return 0;
}

static void release_chunk(struct chunk *chunk)
{
    nmd_frame_releasep(&chunk->frame);
}

static void release_block(struct block *block)
{
    if (block->status == NAE_OK)
        release_chunk(&block->chunk);
}

static void count_error(struct nae_media *m, int ret)
{
    if (ret < 0)
        atomic_fetch_add_explicit(&m->errors, 1, memory_order_relaxed);
}

static void publish_request(struct nae_media *m, uint32_t number, int64_t position)
{
    const uint32_t sequence = atomic_load_explicit(&m->request_sequence, memory_order_relaxed);
    atomic_store_explicit(&m->request_sequence, sequence + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&m->request_number, number, memory_order_relaxed);
    atomic_store_explicit(&m->request_position, position, memory_order_relaxed);
    atomic_store_explicit(&m->request_sequence, sequence + 2, memory_order_release);
}

static void read_request(struct nae_media *m, uint32_t *number, int64_t *position)
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

static int64_t loop_length(const struct nae_media *m);

/* The decoding thread's state */
struct decoder {
    struct nae_media *media;
    uint32_t served;         /* number of the request served */
    uint32_t pass;           /* pass of the loop since */
    size_t outstanding;      /* blocks queued and not released yet */
    int64_t queued;          /* their frames */
    bool has_last;           /* a chunk of the pass, from last_start to last_end */
    int64_t last_start;
    int64_t last_end;
    bool has_held;           /* the first chunk after a seek or at the start of a pass, held back */
    uint32_t held_pass;
    struct chunk held;
};

static void push_block(struct decoder *d, const struct block *block)
{
    struct nae_media *m = d->media;
    /* Room: fewer blocks are out than the queue holds */
    spsc_push(&m->blocks, block);
    d->outstanding++;
    if (block->status == NAE_OK)
        d->queued += block->chunk.nb_frames;
    if (m->blocking)
        nae_sem_post(&m->queued);
}

static void push_chunk(struct decoder *d, uint32_t pass, const struct chunk *chunk)
{
    const struct block block = {.request = d->served, .pass = pass, .chunk = *chunk};
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
 * Queues a chunk decoded. The first one after a seek or at the start of a
 * pass is shown from there by nope.media, while it starts up to a chunk
 * earlier: it is held back until the next one, which tells where it really
 * starts, for the reader to skip what comes before.
 */
static void queue_chunk(struct decoder *d, struct chunk *chunk)
{
    const int64_t contiguous = frames_in(d->media, CONTIGUOUS_MS);

    /* Back before the start of the previous chunk: the start of the next pass of the loop */
    const bool next_pass = d->has_last && chunk->position + contiguous < d->last_start;

    if (d->has_held && !next_pass && chunk->position < d->last_end &&
        chunk->position >= (int64_t)d->held.nb_frames) {
        d->held.position = chunk->position - d->held.nb_frames;
        d->last_end = chunk->position;
    }
    push_held(d);

    if (next_pass) {
        d->pass++;
        d->has_last = false;
    }
    if (!d->has_last) {
        d->has_held = true;
        d->held_pass = d->pass;
        d->held = *chunk;
        d->has_last = true;
        d->last_start = chunk->position;
        d->last_end = chunk->position + chunk->nb_frames;
        return;
    }
    if (chunk->position < d->last_end + contiguous)
        chunk->position = d->last_end;
    d->last_start = chunk->position;
    d->last_end = chunk->position + chunk->nb_frames;
    push_chunk(d, d->pass, chunk);
}

static void *run_decoder(void *arg)
{
    struct nae_media *m = arg;
    nae_thread_set_name("nae/media");
    const int64_t read_ahead = frames_in(m, READ_AHEAD_MS);
    const size_t capacity = spsc_capacity(&m->blocks);

    struct decoder d = {
        .media = m,
        /* Not the request number of now: a request made before the thread
         * started would be taken as served, nope.media not moving to it */
        .served = FIRST_REQUEST,
    };
    uint32_t loop_revision = 0;
    bool at_end = false;
    bool end_once = false;   /* the end of the media, nothing decoded since */

    while (!atomic_load(&m->stop)) {
        struct block block;
        while (spsc_pop(&m->released, &block)) {
            if (block.status == NAE_OK && block.request == d.served)
                d.queued -= block.chunk.nb_frames;
            release_block(&block);
            d.outstanding--;
        }

        const uint32_t revision = atomic_load_explicit(&m->loop_revision, memory_order_acquire);
        if (revision != loop_revision) {
            const int64_t start = atomic_load(&m->loop_start);
            const int64_t end = atomic_load(&m->loop_end);
            count_error(m, nmd_set_loop_range(m->decoder, seconds(m, start), seconds(m, end)));
            loop_revision = revision;
            /* Read to its end before the loop was set, as a short media once
             * opened: nope.media goes round it from there */
            if (end > start)
                at_end = false;
        }

        uint32_t number;
        int64_t position;
        read_request(m, &number, &position);
        if (number != d.served) {
            if (d.has_held) {
                release_chunk(&d.held);
                d.has_held = false;
            }
            /* From the end of the loop, as a source past the end of its range
             * is positioned: what is read next is the pass the reader moves
             * to when the transport wraps, from the start of the loop
             * (nope.media sought to the end of its loop plays on past it) */
            const int64_t loop_start = atomic_load(&m->loop_start);
            const bool next_pass = loop_length(m) && position >= atomic_load(&m->loop_end);
            if (next_pass)
                position = loop_start;
            if (position) {
                count_error(m, nmd_seek(m->decoder, seconds(m, position)));
            } else {
                /* nope.media seeking to the start of the media loses its first
                 * frame (AAC decodes a frame with the one before): playing it
                 * from its start again instead, with its loop range */
                nmd_stop(m->decoder);
                count_error(m, nmd_start(m->decoder));
                count_error(m, nmd_set_loop_range(m->decoder, seconds(m, atomic_load(&m->loop_start)),
                                                  seconds(m, atomic_load(&m->loop_end))));
            }
            d.served = number;
            d.pass = next_pass;
            d.has_last = false;
            /* The audio queued for the earlier requests, which the reader
             * drops, decoded ahead no more: the source gets its data though
             * it is not read meanwhile, as when paused */
            d.queued = 0;
            at_end = false;
        }

        /* Room for a chunk and the one held back */
        if (at_end || d.outstanding + 2 > capacity || d.queued >= read_ahead) {
            nae_sem_wait(&m->wake);
            continue;
        }

        struct chunk chunk = {0};
        const int ret = decoder_next(m, &chunk);

        /* A request coming while nope.media was blocked */
        read_request(m, &number, &position);
        if (number != d.served || atomic_load(&m->stop)) {
            if (ret == 0)
                release_chunk(&chunk);
            continue;
        }

        if (ret == 0 && loop_length(m) && chunk.position >= atomic_load(&m->loop_end)) {
            /* Past the end of the loop, decoded before nope.media knew of it:
             * never read, it would take the room of the next pass */
            release_chunk(&chunk);
            continue;
        }

        /* The end of a short media read ahead before nope.media knew of the
         * loop: asked again, it goes round it */
        if (ret == 1 && loop_length(m) && !end_once) {
            end_once = true;
            continue;
        }
        end_once = false;

        if (ret != 0) {
            count_error(m, ret);
            push_held(&d);
            push_block(&d, &(struct block){.request = d.served, .pass = d.pass, .status = ret < 0 ? NAE_ERROR_IO : NAE_EOF});
            at_end = true;
            continue;
        }
        queue_chunk(&d, &chunk);
    }

    /* The reader is done: its blocks are released here too */
    if (d.has_held)
        release_chunk(&d.held);
    struct block block;
    while (spsc_pop(&m->released, &block))
        release_block(&block);
    while (spsc_pop(&m->blocks, &block))
        release_block(&block);
    return NULL;
}

/* Reader side */

static int64_t cursor_of(const struct nae_media *m)
{
    return atomic_load_explicit(&m->cursor, memory_order_relaxed);
}

static void move_cursor(struct nae_media *m, int64_t position)
{
    atomic_store_explicit(&m->cursor, position, memory_order_relaxed);
}

static void give_back(struct nae_media *m, const struct block *block)
{
    /* Room: the queues are as large, and the decoder queues no more blocks than they hold */
    spsc_push(&m->released, block);
    nae_sem_post(&m->wake);
}

static void drop_block(struct nae_media *m)
{
    if (m->has_block) {
        give_back(m, &m->block);
        m->has_block = false;
    }
}

/* Asks the decoder for the audio from the cursor: the blocks queued until then are dropped */
static void request(struct nae_media *m)
{
    drop_block(m);
    /* The audio queued for the positions left goes back at once: nothing may
     * read it meanwhile (paused, scrubbing), and it would take the room of
     * the new position's */
    struct block block;
    while (spsc_pop(&m->blocks, &block))
        give_back(m, &block);
    m->number++;
    m->pass = 0;
    m->end_status = NAE_OK;
    publish_request(m, m->number, cursor_of(m));
    nae_sem_post(&m->wake);
    atomic_fetch_add_explicit(&m->requests, 1, memory_order_relaxed);
}

static int64_t loop_length(const struct nae_media *m)
{
    const int64_t start = atomic_load_explicit(&m->loop_start, memory_order_relaxed);
    const int64_t end = atomic_load_explicit(&m->loop_end, memory_order_relaxed);
    return end > start ? end - start : 0;
}

/* Takes the next block of the pass read, false if none is queued yet */
static bool take_block(struct nae_media *m)
{
    struct block block;
    while (spsc_pop(&m->blocks, &block)) {
        /* Before the last request: the audio of a position left */
        if (block.request != m->number) {
            give_back(m, &block);
            continue;
        }
        /* The rest of a pass the reader went round the loop from */
        if (block.pass < m->pass) {
            if (block.status == NAE_OK)
                m->dropped += block.chunk.nb_frames;
            give_back(m, &block);
            /* Not going round the loop, as when it is set for the media late */
            if (m->dropped > loop_length(m) + frames_in(m, MAX_LATE_MS)) {
                request(m);
                return false;
            }
            continue;
        }
        /* nope.media went round the loop before the reader: the rest of the pass is not coming */
        if (block.pass > m->pass) {
            give_back(m, &block);
            request(m);
            return false;
        }
        m->block = block;
        m->has_block = true;
        m->block_read = 0;
        return true;
    }
    return false;
}

static int media_read(void *user, int64_t frame, uint32_t frames, float *output)
{
    struct nae_media *m = user;
    const int64_t max_gap = frames_in(m, MAX_GAP_MS);
    const int64_t max_late = frames_in(m, MAX_LATE_MS);

    /* Read from elsewhere than the cursor, as after the range of the source changed */
    if (frame != cursor_of(m)) {
        move_cursor(m, frame);
        request(m);
    }

    int64_t cursor = frame;
    int64_t done = 0;
    bool silent = false;
    while (done < frames && !m->end_status) {
        if (!m->has_block)
            move_cursor(m, cursor); /* asking for the audio from the cursor if the queue cannot provide it */
        if (!m->has_block && !take_block(m)) {
            if (m->blocking) {
                nae_sem_wait(&m->queued);
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
        if (block->status != NAE_OK) {
            m->end_status = block->status;
            drop_block(m);
            break;
        }

        const int64_t start = block->chunk.position;
        const int64_t end = start + block->chunk.nb_frames;
        if (cursor >= end) {
            if (cursor - end > max_late) {
                move_cursor(m, cursor);
                request(m);
                continue;
            }
            atomic_fetch_add_explicit(&m->skipped_frames, end - start - m->block_read, memory_order_relaxed);
            drop_block(m);
            continue;
        }
        if (cursor < start) {
            if (start - cursor > max_gap) {
                move_cursor(m, cursor);
                request(m);
                continue;
            }
            /* A gap in the media */
            const int64_t count = start - cursor < frames - done ? start - cursor : frames - done;
            cursor += count;
            done += count;
            continue;
        }

        const int64_t offset = cursor - start;
        if (m->block_read == 0 && offset > 0)
            atomic_fetch_add_explicit(&m->skipped_frames, offset, memory_order_relaxed);
        const int64_t count = end - cursor < frames - done ? end - cursor : frames - done;
        memcpy(output + 2 * done, block->chunk.samples + 2 * offset, (size_t)count * 2 * sizeof(*output));
        cursor += count;
        done += count;
        m->block_read += count + (m->block_read == 0 ? offset : 0);
        if (cursor == end)
            drop_block(m);
    }

    /* Past the end of the media the cursor goes on, the reads being silent */
    move_cursor(m, m->end_status ? frame + frames : cursor);
    if (m->end_status < 0)
        return m->end_status;
    if (silent)
        return NAE_AGAIN;
    return done < frames ? NAE_EOF : NAE_OK;
}

static void media_seek(void *user, int64_t frame, int wrap)
{
    struct nae_media *m = user;
    if (wrap && loop_length(m)) {
        /* Going round the loop: onto the audio of the next pass */
        drop_block(m);
        move_cursor(m, frame);
        m->pass++;
        m->dropped = 0;
        m->end_status = NAE_OK;
        return;
    }
    if (frame == cursor_of(m) && !m->end_status)
        return;
    move_cursor(m, frame);
    request(m);
}

static void media_destroy(void *user)
{
    struct nae_media *m = user;
    nae_media_unrefp(&m);
}

/* Control thread */

int nae_media_create(struct nae_engine *engine, const char *filename,
                     const struct nae_media_params *params, struct nae_media **media)
{
    if (!media)
        return NAE_ERROR_INVALID;
    *media = NULL;
    const struct nae_config *config = nae_engine_get_config(engine);
    const struct nae_media_params defaults = {0};
    if (!params)
        params = &defaults;
    if (!config || config->channels != 2 || !filename || params->start < 0)
        return NAE_ERROR_INVALID;
    struct nae_media *m = calloc(1, sizeof(*m));
    if (!m)
        return NAE_ERROR_MEMORY;
    m->refs = 1;
    m->engine = engine;
    m->sample_rate = config->sample_rate;
    m->blocking = params->blocking;
    m->number = FIRST_REQUEST;
    atomic_init(&m->request_sequence, 0);
    atomic_init(&m->request_number, FIRST_REQUEST);
    atomic_init(&m->request_position, params->start);
    atomic_init(&m->loop_start, 0);
    atomic_init(&m->loop_end, 0);
    atomic_init(&m->loop_revision, 0);
    atomic_init(&m->stop, false);
    atomic_init(&m->cursor, params->start);
    atomic_init(&m->silent_frames, 0);
    atomic_init(&m->skipped_frames, 0);
    atomic_init(&m->requests, 0);
    atomic_init(&m->errors, 0);
    if (!atomic_is_lock_free(&m->cursor) || !atomic_is_lock_free(&m->request_position)) {
        nae_media_unrefp(&m);
        return NAE_ERROR_UNSUPPORTED;
    }
    if (spsc_init(&m->blocks, QUEUE_CAPACITY, sizeof(struct block)) < 0 ||
        spsc_init(&m->released, QUEUE_CAPACITY, sizeof(struct block)) < 0) {
        nae_media_unrefp(&m);
        return NAE_ERROR_MEMORY;
    }
    if (nae_sem_init(&m->wake) < 0) {
        nae_media_unrefp(&m);
        return NAE_ERROR_MEMORY;
    }
    if (nae_sem_init(&m->queued) < 0) {
        nae_sem_destroy(&m->wake);
        nae_media_unrefp(&m);
        return NAE_ERROR_MEMORY;
    }
    m->sems_ready = true;

    m->decoder = nmd_create(filename);
    if (!m->decoder) {
        nae_media_unrefp(&m);
        return NAE_ERROR_MEMORY;
    }
    char filters[128];
    snprintf(filters, sizeof(filters), "%saformat=sample_fmts=flt:channel_layouts=stereo:sample_rates=%u",
             params->normalize ? "dynaudnorm=f=150:g=15," : "", m->sample_rate);
    if (nmd_set_option(m->decoder, "avselect", NMD_SELECT_AUDIO) < 0 ||
        nmd_set_option(m->decoder, "audio_texture", 0) < 0 ||
        nmd_set_option(m->decoder, "auto_hwaccel", 0) < 0 ||
        nmd_set_option(m->decoder, "filters", filters) < 0) {
        nae_media_unrefp(&m);
        return NAE_ERROR_IO;
    }
    struct nmd_info info;
    if ((params->start && nmd_seek(m->decoder, seconds(m, params->start)) < 0) ||
        nmd_start(m->decoder) < 0 || nmd_get_info(m->decoder, &info) < 0) {
        nae_media_unrefp(&m);
        return NAE_ERROR_IO;
    }
    if (info.duration > 0) {
        const double length = round(info.duration * m->sample_rate);
        if (!isfinite(length) || length >= 0x1p63) {
            nae_media_unrefp(&m);
            return NAE_ERROR_UNSUPPORTED;
        }
        m->length = (int64_t)length;
    }

    /* From now on, nope.media is used by the decoding thread only */
    if (nae_thread_create(&m->thread, run_decoder, m) < 0) {
        nae_media_unrefp(&m);
        return NAE_ERROR_MEMORY;
    }
    m->thread_started = true;
    *media = m;
    return NAE_OK;
}

int64_t nae_media_get_length(const struct nae_media *m)
{
    return m ? m->length : 0;
}

int nae_media_source(struct nae_media *m, int64_t start, int64_t offset, int64_t duration, struct nae_node **node)
{
    if (!m || m->source_created)
        return NAE_ERROR_INVALID;
    const struct nae_source source = {
        .user = m, .read = media_read, .seek = media_seek, .destroy = media_destroy,
        .timeline_start = start, .source_start = offset, .duration = duration,
    };
    int ret = nae_source_create(m->engine, &source, node);
    if (ret == NAE_OK) {
        m->refs++;
        m->source_created = true;
    }
    return ret;
}

int nae_media_set_loop(struct nae_media *m, int64_t start, int64_t end)
{
    if (!m || start < 0 || end < 0)
        return NAE_ERROR_INVALID;
    if (end <= start)
        start = end = 0;
    if (start == atomic_load(&m->loop_start) && end == atomic_load(&m->loop_end))
        return NAE_OK;
    atomic_store(&m->loop_start, start);
    atomic_store(&m->loop_end, end);
    atomic_fetch_add_explicit(&m->loop_revision, 1, memory_order_release);
    nae_sem_post(&m->wake);
    return NAE_OK;
}

void nae_media_get_stats(const struct nae_media *m, struct nae_media_stats *stats)
{
    if (!m || !stats)
        return;
    *stats = (struct nae_media_stats){
        .silent_frames = atomic_load_explicit(&m->silent_frames, memory_order_relaxed),
        .skipped_frames = atomic_load_explicit(&m->skipped_frames, memory_order_relaxed),
        .requests = atomic_load_explicit(&m->requests, memory_order_relaxed),
        .errors = atomic_load_explicit(&m->errors, memory_order_relaxed),
    };
}

void nae_media_unrefp(struct nae_media **mp)
{
    if (!mp || !*mp)
        return;
    struct nae_media *m = *mp;
    *mp = NULL;
    if (--m->refs)
        return;
    if (m->thread_started) {
        if (m->has_block)
            spsc_push(&m->released, &m->block);
        atomic_store(&m->stop, true);
        nae_sem_post(&m->wake);
        nae_thread_join(m->thread);
    }
    nmd_freep(&m->decoder);
    if (m->sems_ready) {
        nae_sem_destroy(&m->wake);
        nae_sem_destroy(&m->queued);
    }
    spsc_uninit(&m->blocks);
    spsc_uninit(&m->released);
    free(m);
}
