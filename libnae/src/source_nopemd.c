/* SPDX-License-Identifier: Apache-2.0 */
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nopemd.h>
#include "nae/nopemd.h"

struct pcm_block {
    int64_t first;
    uint32_t frames;
    float *samples;
};

struct nae_media {
    struct nae_engine *engine;
    struct nae_config config;
    struct nmd_ctx *decoder;
    struct nmd_frame *frame;       /* Producer only; never released in callback. */
    uint32_t frame_offset;
    uint32_t refs;                 /* Control thread only. */
    bool source_created;
    uint32_t capacity;
    struct pcm_block *blocks;
    float *samples;
    _Atomic uint32_t read;
    _Atomic uint32_t write;
    _Atomic int terminal;          /* EOF or a decode error, once all data is queued. */
};

int nae_media_create(struct nae_engine *engine, const char *filename,
                     uint32_t capacity, struct nae_media **media)
{
    if (!media)
        return NAE_ERROR_INVALID;
    *media = NULL;
    const struct nae_config *config = nae_engine_get_config(engine);
    /* Power-of-two capacity keeps slot indexing valid across uint32 wrap. */
    if (!config || config->channels != 2 || !filename || capacity < 2 ||
        capacity > 4096 || (capacity & (capacity - 1)))
        return NAE_ERROR_INVALID;
    struct nae_media *s = calloc(1, sizeof(*s));
    if (!s)
        return NAE_ERROR_MEMORY;
    s->refs = 1;
    s->engine = engine;
    s->config = *config;
    s->capacity = capacity;
    atomic_init(&s->read, 0);
    atomic_init(&s->write, 0);
    atomic_init(&s->terminal, 0);
    if (!atomic_is_lock_free(&s->read) || !atomic_is_lock_free(&s->write) ||
        !atomic_is_lock_free(&s->terminal)) {
        nae_media_unrefp(&s);
        return NAE_ERROR_UNSUPPORTED;
    }
    const size_t samples = (size_t)config->block_frames * 2;
    if (samples > SIZE_MAX / sizeof(float) / capacity) {
        nae_media_unrefp(&s);
        return NAE_ERROR_MEMORY;
    }
    s->blocks = calloc(capacity, sizeof(*s->blocks));
    s->samples = calloc(capacity * samples, sizeof(*s->samples));
    s->decoder = nmd_create(filename);
    if (!s->blocks || !s->samples || !s->decoder) {
        nae_media_unrefp(&s);
        return NAE_ERROR_MEMORY;
    }
    for (uint32_t i = 0; i < capacity; i++)
        s->blocks[i].samples = s->samples + i * samples;
    char filters[64];
    snprintf(filters, sizeof(filters), "aresample=%u", config->sample_rate);
    if (nmd_set_option(s->decoder, "avselect", NMD_SELECT_AUDIO) < 0 ||
        nmd_set_option(s->decoder, "audio_texture", 0) < 0 ||
        nmd_set_option(s->decoder, "auto_hwaccel", 0) < 0 ||
        nmd_set_option(s->decoder, "filters", filters) < 0) {
        nae_media_unrefp(&s);
        return NAE_ERROR_IO;
    }
    *media = s;
    return NAE_OK;
}

int nae_media_pump(struct nae_media *s)
{
    if (!s)
        return NAE_ERROR_INVALID;
    int terminal = atomic_load_explicit(&s->terminal, memory_order_relaxed);
    if (terminal)
        return terminal;
    const uint32_t w = atomic_load_explicit(&s->write, memory_order_relaxed);
    const uint32_t r = atomic_load_explicit(&s->read, memory_order_acquire);
    if (w - r == s->capacity)
        return NAE_AGAIN;
    if (!s->frame) {
        const int ret = nmd_get_next_frame(s->decoder, &s->frame);
        if (ret != NMD_RET_NEWFRAME) {
            if (ret == NMD_RET_UNCHANGED)
                return NAE_AGAIN;
            terminal = ret == NMD_ERR_EOF ? NAE_EOF : NAE_ERROR_IO;
            atomic_store_explicit(&s->terminal, terminal, memory_order_release);
            return terminal;
        }
        s->frame_offset = 0;
        if (s->frame->pix_fmt != NMD_SMPFMT_FLT || !s->frame->datap[0] || s->frame->nb_samples <= 0) {
            nmd_frame_releasep(&s->frame);
            atomic_store_explicit(&s->terminal, NAE_ERROR_UNSUPPORTED, memory_order_release);
            return NAE_ERROR_UNSUPPORTED;
        }
    }
    const uint32_t left = (uint32_t)s->frame->nb_samples - s->frame_offset;
    const uint32_t n = left < s->config.block_frames ? left : s->config.block_frames;
    const long double position = (long double)s->frame->ts * s->config.sample_rate;
    if (!isfinite(position) || position < (long double)INT64_MIN + 1 ||
        position > (long double)INT64_MAX - s->frame->nb_samples - 1) {
        nmd_frame_releasep(&s->frame);
        atomic_store_explicit(&s->terminal, NAE_ERROR_IO, memory_order_release);
        return NAE_ERROR_IO;
    }
    struct pcm_block *block = &s->blocks[w & (s->capacity - 1)];
    block->first = (int64_t)roundl(position) + s->frame_offset;
    block->frames = n;
    memcpy(block->samples, (const float *)s->frame->datap[0] + (size_t)s->frame_offset * 2,
           (size_t)n * 2 * sizeof(float));
    s->frame_offset += n;
    if (s->frame_offset == (uint32_t)s->frame->nb_samples)
        nmd_frame_releasep(&s->frame);
    atomic_store_explicit(&s->write, w + 1, memory_order_release);
    return NAE_OK;
}

static int media_read(void *user, int64_t first, uint32_t frames, float *output)
{
    struct nae_media *s = user;
    uint32_t r = atomic_load_explicit(&s->read, memory_order_relaxed);
    /* Snapshot the producer: processing work is bounded by queue capacity,
     * even if the producer refills consumed slots during this call. */
    const uint32_t w = atomic_load_explicit(&s->write, memory_order_acquire);
    const int64_t end = first + frames;
    int64_t cursor = first;
    bool missing = false;
    while (r != w && cursor < end) {
        const struct pcm_block *b = &s->blocks[r & (s->capacity - 1)];
        const int64_t block_end = b->first + b->frames;
        if (block_end <= cursor) {
            r++;
            continue;
        }
        if (b->first > cursor) {
            missing = true;
            cursor = b->first < end ? b->first : end;
        }
        if (cursor >= end)
            break;
        const int64_t hi = block_end < end ? block_end : end;
        memcpy(output + (size_t)(cursor - first) * 2,
               b->samples + (size_t)(cursor - b->first) * 2,
               (size_t)(hi - cursor) * 2 * sizeof(float));
        cursor = hi;
        if (cursor == block_end)
            r++;
    }
    atomic_store_explicit(&s->read, r, memory_order_release);
    if (cursor < end) {
        const int terminal = atomic_load_explicit(&s->terminal, memory_order_acquire);
        if (r == w && terminal) {
            /* A terminal store may follow new PCM after our write snapshot.
             * Recheck write before treating the unfilled interval as EOF. */
            if (atomic_load_explicit(&s->write, memory_order_acquire) == w)
                return terminal;
        }
        missing = true;
    }
    return missing ? NAE_AGAIN : NAE_OK;
}

static void media_destroy(void *user)
{
    struct nae_media *s = user;
    nae_media_unrefp(&s);
}

int nae_media_source(struct nae_media *s, int64_t start, int64_t offset,
                     int64_t duration, struct nae_node **node)
{
    if (!s || s->source_created)
        return NAE_ERROR_INVALID;
    const struct nae_source source = {
        .user = s, .read = media_read, .destroy = media_destroy,
        .timeline_start = start, .source_start = offset, .duration = duration,
    };
    int ret = nae_source_create(s->engine, &source, node);
    if (ret == NAE_OK) {
        s->refs++;
        s->source_created = true;
    }
    return ret;
}

uint32_t nae_media_queued(const struct nae_media *s)
{
    if (!s)
        return 0;
    const uint32_t r = atomic_load_explicit(&s->read, memory_order_acquire);
    const uint32_t w = atomic_load_explicit(&s->write, memory_order_acquire);
    const uint32_t queued = w - r;
    return queued < s->capacity ? queued : s->capacity;
}

int nae_media_seek(struct nae_media *s, int64_t frame)
{
    /* nope.media's seek API converts seconds to signed microseconds. Leave
     * headroom for its internal arithmetic, matching its loop-range limit. */
    if (!s || frame < 0 || frame > (INT64_MAX / 4 / 1000000) * s->config.sample_rate)
        return NAE_ERROR_INVALID;
    nmd_frame_releasep(&s->frame);
    s->frame_offset = 0;
    atomic_store_explicit(&s->read, 0, memory_order_relaxed);
    atomic_store_explicit(&s->write, 0, memory_order_relaxed);
    atomic_store_explicit(&s->terminal, 0, memory_order_relaxed);
    const int ret = nmd_seek(s->decoder, (double)frame / s->config.sample_rate);
    if (ret < 0) {
        atomic_store_explicit(&s->terminal, NAE_ERROR_IO, memory_order_relaxed);
        return NAE_ERROR_IO;
    }
    return NAE_OK;
}

void nae_media_unrefp(struct nae_media **sp)
{
    if (!sp || !*sp)
        return;
    struct nae_media *s = *sp;
    *sp = NULL;
    if (--s->refs)
        return;
    nmd_frame_releasep(&s->frame);
    nmd_freep(&s->decoder);
    free(s->blocks);
    free(s->samples);
    free(s);
}
