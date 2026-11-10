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

#include <string.h>

#include "nopeaudio/nopeaudio.h"
#include "engine_internal.h"
#include "ramp.h"
#include "utils/memory.h"
#include "utils/utils.h"

/* Ramps of a fader: those set on the node, or their copy in a plan */
struct volume_ramps {
    struct nau_ramp *items;
    uint32_t count;
};

static void fader_destroy(void *user)
{
    struct volume_ramps *set = user;
    nau_freep(&set->items);
    nau_freep(&set);
}

static void *fader_take(void *user)
{
    const struct volume_ramps *set = user;
    struct volume_ramps *copy = nau_try_calloc(1, sizeof(*copy));
    if (!copy)
        return NULL;
    copy->items = nau_try_calloc(set->count + 1, sizeof(*set->items));
    if (!copy->items) {
        nau_freep(&copy);
        return NULL;
    }
    if (set->count)
        memcpy(copy->items, set->items, set->count * sizeof(*set->items));
    copy->count = set->count;
    return copy;
}

static const struct nau_snapshot_callbacks fader_snapshot_callbacks = {
    .take    = fader_take,
    .release = fader_destroy,
};

/*
 * Split the block at ramp boundaries. Evaluate volume per frame inside a
 * ramp, and use a single volume for each constant span.
 */
static int fader_process(void *user, const struct nau_block *p)
{
    const struct volume_ramps *set = user;
    const float *restrict src = p->inputs[0];
    float *restrict dst = p->output;
    const uint32_t channels = p->channels;
    const int64_t start = p->frame - p->origin;
    uint32_t next = nau_ramps_started(set->items, set->count, start);
    for (uint32_t i = 0; i < p->frames;) {
        const int64_t frame = start + i;
        while (next < set->count && set->items[next].frame <= frame)
            next++;
        /*
         * Compare with frame + n before subtracting frame: frame can be
         * negative, so a difference with a far-future ramp could overflow.
         * The origin is non-negative, so frame + n <= p->frame + p->frames,
         * which the engine keeps within range.
         */
        int64_t n = p->frames - i;
        if (next < set->count && set->items[next].frame < frame + n)
            n = set->items[next].frame - frame;
        const struct nau_ramp *k = next ? &set->items[next - 1] : NULL;
        if (k && frame < k->frame + k->length) {
            /* Within a ramp, up to its end */
            n = NAU_MIN(n, k->frame + k->length - frame);
            const float from = next > 1 ? set->items[next - 2].value : 1.f;
            for (uint32_t j = i; j < i + (uint32_t)n; j++) {
                const float progress = (float)((double)(start + j - k->frame) / (double)k->length);
                const float volume = nau_ramp_volume(k->shape, k->curvature, from, k->value, progress);
                for (uint32_t c = 0; c < channels; c++)
                    dst[(size_t)j * channels + c] = src[(size_t)j * channels + c] * volume;
            }
        } else {
            const float volume = k ? k->value : 1.f;
            const size_t end = (size_t)(i + (uint32_t)n) * channels;
            for (size_t j = (size_t)i * channels; j < end; j++)
                dst[j] = src[j] * volume;
        }
        i += (uint32_t)n;
    }
    return NAU_OK;
}

int nau_fader_init(struct nau_node *node, struct nau_engine *s)
{
    struct volume_ramps *set = nau_try_calloc(1, sizeof(*set));
    if (!set)
        return NAU_ERROR_MEMORY;

    const struct nau_processor processor = {
        .input_count = 1,
        .user        = set,
        .process     = fader_process,
        .destroy     = fader_destroy,
    };
    int ret = nau_node_init_with_snapshot_callbacks(node, s, &processor, &fader_snapshot_callbacks);
    if (ret < 0)
        nau_freep(&set);
    return ret;
}

int nau_fader_set_ramps(struct nau_node *node, const struct nau_ramp *ramps, uint32_t count)
{
    struct volume_ramps *set = nau_node_get_user(node, fader_process);
    if (!set)
        return NAU_ERROR_INVALID;

    if (!nau_ramps_valid(ramps, count))
        return NAU_ERROR_INVALID;

    for (uint32_t i = 0; i < count; i++)
        if (ramps[i].value < 0.f)
            return NAU_ERROR_INVALID;
    struct nau_ramp *items = NULL;
    if (count) {
        items = nau_try_memdup(ramps, count * sizeof(*items));
        if (!items)
            return NAU_ERROR_MEMORY;
    }
    nau_freep(&set->items);
    set->items = items;
    set->count = count;
    return NAU_OK;
}
