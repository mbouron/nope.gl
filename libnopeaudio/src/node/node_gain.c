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

#include "nopeaudio/nopeaudio.h"
#include "engine_internal.h"
#include "internal.h"
#include "libnopeutil/memory.h"
#include "libnopeutil/utils.h"

/*
 * A gain following a target which can be set from any thread: the render
 * thread ramps to it over ramp_frames frames
 */
struct gain {
    _Atomic uint32_t target_bits;      /* target set, as float bits */
    uint32_t ramp_frames;
    /* Render thread only */
    uint32_t remaining;
    float target;
    float current;
};

static float gain_target(struct gain *g)
{
    return nau_bits_float(atomic_load_explicit(&g->target_bits, memory_order_relaxed));
}

/* Reset gain smoothing to the current target when playback jumps or resumes. */
static void gain_reset(void *user, int64_t frame)
{
    struct gain *g = user;
    g->current = g->target = gain_target(g);
    g->remaining = 0;
}

static int gain_process(void *user, const struct nau_block *p)
{
    struct gain *g = user;
    const float target = gain_target(g);
    if (target != g->target) {
        g->target = target;
        g->remaining = g->ramp_frames;
    }

    /*
     * Keep ramp state in local variables so output writes cannot force
     * a reload for every sample.
     */
    const float *restrict src = p->inputs[0];
    float *restrict dst = p->output;
    const uint32_t channels = p->channels;
    float current = g->current;
    uint32_t remaining = g->remaining;

    /* The frames of the ramp, then the target for the rest of the block */
    const uint32_t ramp_frames = NPRIV_MIN(remaining, p->frames);
    for (uint32_t i = 0; i < ramp_frames; i++) {
        current += (target - current) / (float)remaining;
        remaining--;
        for (uint32_t c = 0; c < channels; c++)
            dst[(size_t)i * channels + c] = src[(size_t)i * channels + c] * current;
    }
    if (!remaining)
        current = target;
    const size_t samples = (size_t)p->frames * channels;
    for (size_t i = (size_t)ramp_frames * channels; i < samples; i++)
        dst[i] = src[i] * current;

    g->current = current;
    g->remaining = remaining;
    return NAU_OK;
}

int nau_gain_init(struct nau_node *node, struct nau_engine *s, float value, uint32_t ramp_frames)
{
    if (!isfinite(value) || value < 0)
        return NAU_ERROR_INVALID;

    struct gain *g = npriv_try_calloc(1, sizeof(*g));
    if (!g)
        return NAU_ERROR_MEMORY;
    atomic_init(&g->target_bits, nau_float_bits(value));
    if (!atomic_is_lock_free(&g->target_bits)) {
        npriv_freep(&g);
        return NAU_ERROR_UNSUPPORTED;
    }
    g->ramp_frames = ramp_frames;

    const struct nau_processor processor = {
        .input_count = 1,
        .user        = g,
        .process     = gain_process,
        .reset       = gain_reset,
        .destroy     = npriv_free,
    };
    int ret = nau_node_init(node, s, &processor);
    if (ret < 0)
        npriv_freep(&g);
    return ret;
}

int nau_gain_set(struct nau_node *node, float gain)
{
    struct gain *g = nau_node_get_user(node, gain_process);
    if (!g)
        return NAU_ERROR_INVALID;

    if (!isfinite(gain) || gain < 0)
        return NAU_ERROR_INVALID;

    atomic_store_explicit(&g->target_bits, nau_float_bits(gain), memory_order_relaxed);
    return NAU_OK;
}
