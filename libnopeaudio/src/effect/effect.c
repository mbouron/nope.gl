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

/*
 * Effect parameter handling, shared by all effect types (see effect.h).
 *
 * Each parameter has a target set from any thread and optional automation
 * set from the control thread. The render thread evaluates both in chunks
 * of NAU_EFFECT_CHUNK frames:
 *
 *    target --> knob (ramped value) --.
 *                                     +--> automation --> render()
 *    ramps copied at commit ----------'
 *
 * The knob supplies the value before the first automation ramp and the
 * starting value of that ramp.
 */

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "nopeaudio/nopeaudio.h"
#include "effect/effect.h"
#include "engine_internal.h"
#include "internal.h"
#include "ramp.h"
#include "utils/memory.h"
#include "utils/utils.h"

static const struct nau_effect_class *const classes[] = {
    [NAU_EFFECT_PASSTHROUGH] = &nau_effect_passthrough,
    [NAU_EFFECT_LOWPASS] = &nau_effect_lowpass,
};

static const struct nau_effect_class *get_class(enum nau_effect_type type)
{
    return (size_t)type < NAU_ARRAY_COUNT(classes) ? classes[type] : NULL;
}

const struct nau_effect_info *nau_effect_get_info(enum nau_effect_type type)
{
    const struct nau_effect_class *cls = get_class(type);
    return cls ? &cls->info : NULL;
}

/* Automation of a parameter */
struct lane {
    struct nau_ramp *items;
    uint32_t count;
};

/* Value of a parameter ramping to its target, one step per chunk */
struct knob {
    float current;
    float target;
    uint32_t left;                  /* chunks left before reaching the target */
};

struct effect {
    const struct nau_effect_class *cls;
    uint32_t ramp_chunks;           /* length of a parameter change, in chunks */
    /* Range rendered */
    float min[NAU_EFFECT_MAX_PARAMS];
    float max[NAU_EFFECT_MAX_PARAMS];
    /* Targets set from any thread, stored as float bits */
    _Atomic uint32_t target_bits[NAU_EFFECT_MAX_PARAMS];
    /* Automation; control thread only */
    struct lane lanes[NAU_EFFECT_MAX_PARAMS];
    /* Render thread only */
    struct knob knobs[NAU_EFFECT_MAX_PARAMS];
    void *state;
};

/* Snapshot of a commit: a copy of the automation, and the effect it applies to */
struct effect_snapshot {
    struct effect *effect;
    struct lane lanes[NAU_EFFECT_MAX_PARAMS];
};

static void snapshot_release(void *user)
{
    struct effect_snapshot *snapshot = user;
    for (uint32_t i = 0; i < NAU_EFFECT_MAX_PARAMS; i++)
        nau_freep(&snapshot->lanes[i].items);
    nau_freep(&snapshot);
}

static void *snapshot_take(void *user)
{
    struct effect *e = user;
    struct effect_snapshot *snapshot = nau_try_calloc(1, sizeof(*snapshot));
    if (!snapshot)
        return NULL;
    snapshot->effect = e;
    for (uint32_t i = 0; i < e->cls->info.param_count; i++) {
        const struct lane *lane = &e->lanes[i];
        if (!lane->count)
            continue;
        snapshot->lanes[i].items = nau_try_memdup(lane->items, lane->count * sizeof(*lane->items));
        if (!snapshot->lanes[i].items) {
            snapshot_release(snapshot);
            return NULL;
        }
        snapshot->lanes[i].count = lane->count;
    }
    return snapshot;
}

static const struct nau_snapshot_callbacks effect_snapshot_callbacks = {
    .take    = snapshot_take,
    .release = snapshot_release,
};

static float clamp_param(const struct effect *e, uint32_t param, float value)
{
    return fminf(fmaxf(value, e->min[param]), e->max[param]);
}

static float param_target(const struct effect *e, uint32_t param)
{
    const float value = nau_bits_float(atomic_load_explicit(&e->target_bits[param], memory_order_relaxed));
    return clamp_param(e, param, value);
}

/*
 * Stepped parameters change immediately; other parameters ramp to their
 * target from the next chunk boundary.
 */
static void knobs_follow(struct effect *e)
{
    const struct nau_effect_info *info = &e->cls->info;
    for (uint32_t i = 0; i < info->param_count; i++) {
        struct knob *k = &e->knobs[i];
        const float target = param_target(e, i);
        if (target == k->target)
            continue;
        k->target = target;
        k->left = info->params[i].scale == NAU_PARAM_STEPPED ? 0 : e->ramp_chunks;
        if (!k->left)
            k->current = target;
    }
}

/*
 * Advance the ramps of the knobs by one chunk, along the scale of their
 * parameters
 */
static void knobs_step(struct effect *e)
{
    const struct nau_effect_info *info = &e->cls->info;
    for (uint32_t i = 0; i < info->param_count; i++) {
        struct knob *k = &e->knobs[i];
        if (!k->left) {
            k->current = k->target;
            continue;
        }
        const float left = (float)k->left--;
        if (info->params[i].scale == NAU_PARAM_LOG) {
            const float current = log2f(k->current);
            k->current = exp2f(current + (log2f(k->target) - current) / left);
        } else {
            k->current += (k->target - k->current) / left;
        }
    }
}

/*
 * Get the value of a parameter at `frame`, the first frame of a chunk. `next`
 * is the number of ramps starting at or before `frame`.
 */
static float param_value(const struct effect *e, const struct lane *lane, uint32_t param, int64_t frame,
                         uint32_t next)
{
    const float knob = e->knobs[param].current;
    if (!next)
        return knob;
    const struct nau_ramp *k = &lane->items[next - 1];
    const float to = clamp_param(e, param, k->value);
    if (frame >= k->frame + k->length)
        return to;
    const float from = next > 1 ? clamp_param(e, param, lane->items[next - 2].value) : knob;
    const float progress = (float)((double)(frame - k->frame) / (double)k->length);
    return nau_ramp_param(&e->cls->info.params[param], k->shape, k->curvature, from, to, progress);
}

/* Round down to a chunk boundary, including before the node's origin. */
static int64_t chunk_of(int64_t frame)
{
    return frame - ((frame % NAU_EFFECT_CHUNK) + NAU_EFFECT_CHUNK) % NAU_EFFECT_CHUNK;
}

static int effect_process(void *user, const struct nau_block *p)
{
    const struct effect_snapshot *snapshot = user;
    struct effect *e = snapshot->effect;
    const struct nau_effect_info *info = &e->cls->info;
    knobs_follow(e);
    /*
     * Evaluate automation at chunk boundaries relative to the node's origin,
     * even if this block starts inside a chunk. Splitting a render call must
     * not shift the automation.
     */
    const int64_t start = p->frame - p->origin;
    uint32_t next[NAU_EFFECT_MAX_PARAMS];
    for (uint32_t j = 0; j < info->param_count; j++)
        next[j] = nau_ramps_started(snapshot->lanes[j].items, snapshot->lanes[j].count, chunk_of(start));
    for (uint32_t i = 0; i < p->frames;) {
        const int64_t frame = start + i;
        const int64_t chunk = chunk_of(frame);
        if (frame == chunk)
            knobs_step(e);
        float params[NAU_EFFECT_MAX_PARAMS];
        for (uint32_t j = 0; j < info->param_count; j++) {
            const struct lane *lane = &snapshot->lanes[j];
            while (next[j] < lane->count && lane->items[next[j]].frame <= chunk)
                next[j]++;
            /* Interpolating along a log scale can end a few ulps past the range */
            params[j] = clamp_param(e, j, param_value(e, lane, j, chunk, next[j]));
        }
        /*
         * frame - chunk is within [0, NAU_EFFECT_CHUNK): no overflow near the
         * end of the timeline
         */
        uint32_t n = NAU_EFFECT_CHUNK - (uint32_t)(frame - chunk);
        if (n > p->frames - i)
            n = p->frames - i;
        const size_t offset = (size_t)i * p->channels;
        const float *inputs[2];
        for (uint32_t k = 0; k < p->input_count; k++)
            inputs[k] = p->inputs[k] + offset;
        const struct nau_block chunk_block = {
            .frame       = p->frame + i,
            .frames      = n,
            .channels    = p->channels,
            .sample_rate = p->sample_rate,
            .input_count = p->input_count,
            .inputs      = inputs,
            .output      = p->output + offset,
            .origin      = p->origin,
        };
        e->cls->render(e->state, params, &chunk_block);
        i += n;
    }
    return NAU_OK;
}

/* Reset parameter smoothing to the current targets; keep the automation. */
static void effect_reset(void *user, int64_t frame)
{
    struct effect *e = user;
    for (uint32_t i = 0; i < e->cls->info.param_count; i++) {
        struct knob *k = &e->knobs[i];
        k->current = k->target = param_target(e, i);
        k->left = 0;
    }
    if (e->cls->reset)
        e->cls->reset(e->state);
}

static void effect_destroy(void *user)
{
    struct effect *e = user;
    for (uint32_t i = 0; i < NAU_EFFECT_MAX_PARAMS; i++)
        nau_freep(&e->lanes[i].items);
    nau_freep(&e->state);
    nau_freep(&e);
}

int nau_effect_init(struct nau_node *node, struct nau_engine *s, enum nau_effect_type type, uint32_t ramp_frames)
{
    const struct nau_effect_class *cls = get_class(type);
    if (!cls)
        return NAU_ERROR_INVALID;
    nau_assert(cls->info.param_count <= NAU_EFFECT_MAX_PARAMS);
    const struct nau_engine_config *config = nau_engine_get_config(s);
    if (!config)
        return NAU_ERROR_INVALID;
    struct effect *e = nau_try_calloc(1, sizeof(*e));
    if (!e)
        return NAU_ERROR_MEMORY;
    e->cls = cls;
    e->ramp_chunks = ramp_frames / NAU_EFFECT_CHUNK + (ramp_frames % NAU_EFFECT_CHUNK != 0);
    if (cls->state_size) {
        e->state = nau_try_calloc(1, cls->state_size);
        if (!e->state) {
            effect_destroy(e);
            return NAU_ERROR_MEMORY;
        }
    }
    const uint32_t sample_rate = config->sample_rate;
    for (uint32_t i = 0; i < cls->info.param_count; i++) {
        const struct nau_param_info *param = &cls->info.params[i];
        e->min[i] = param->min;
        e->max[i] = param->max;
        if (cls->limit)
            cls->limit(i, sample_rate, &e->min[i], &e->max[i]);
        atomic_init(&e->target_bits[i], nau_float_bits(param->default_value));
        e->knobs[i].current = e->knobs[i].target = param_target(e, i);
    }
    if (!atomic_is_lock_free(&e->target_bits[0])) {
        effect_destroy(e);
        return NAU_ERROR_UNSUPPORTED;
    }
    const struct nau_processor processor = {
        .input_count = 1,
        .user        = e,
        .process     = effect_process,
        .reset       = effect_reset,
        .destroy     = effect_destroy,
    };
    int ret = nau_node_init_with_snapshot_callbacks(node, s, &processor, &effect_snapshot_callbacks);
    if (ret < 0)
        effect_destroy(e);
    return ret;
}

static bool value_valid(const struct nau_param_info *param, float value)
{
    return isfinite(value) && value >= param->min && value <= param->max &&
           (param->scale != NAU_PARAM_STEPPED || value == truncf(value));
}

int nau_effect_set_param(struct nau_node *node, uint32_t param, float value)
{
    struct effect *e = nau_node_get_user(node, effect_process);
    if (!e)
        return NAU_ERROR_INVALID;

    if (param >= e->cls->info.param_count)
        return NAU_ERROR_INVALID;

    if (!value_valid(&e->cls->info.params[param], value))
        return NAU_ERROR_INVALID;

    atomic_store_explicit(&e->target_bits[param], nau_float_bits(value), memory_order_relaxed);
    return NAU_OK;
}

static bool automation_valid(const struct effect *e, uint32_t param, const struct nau_ramp *ramps, uint32_t count)
{
    if (param >= e->cls->info.param_count)
        return false;

    if (!nau_ramps_valid(ramps, count))
        return false;

    for (uint32_t i = 0; i < count; i++)
        if (!value_valid(&e->cls->info.params[param], ramps[i].value))
            return false;
    return true;
}

static void swap_lane(struct lane *lane, struct nau_ramp **items, uint32_t *count)
{
    struct nau_ramp *previous = lane->items;
    const uint32_t previous_count = lane->count;
    lane->items = *items;
    lane->count = *count;
    *items = previous;
    *count = previous_count;
}

int nau_effect_set_automation(struct nau_node *node, uint32_t param, const struct nau_ramp *ramps, uint32_t count)
{
    struct effect *e = nau_node_get_user(node, effect_process);
    if (!e)
        return NAU_ERROR_INVALID;

    if (!automation_valid(e, param, ramps, count))
        return NAU_ERROR_INVALID;

    struct nau_ramp *items = NULL;
    if (count) {
        items = nau_try_memdup(ramps, count * sizeof(*items));
        if (!items)
            return NAU_ERROR_MEMORY;
    }
    swap_lane(&e->lanes[param], &items, &count);
    nau_freep(&items);
    return NAU_OK;
}

int nau_effect_swap_automation(struct nau_node *node, uint32_t param, struct nau_ramp **ramps, uint32_t *count)
{
    struct effect *e = nau_node_get_user(node, effect_process);
    if (!e || !ramps || !count)
        return NAU_ERROR_INVALID;

    if (!automation_valid(e, param, *ramps, *count))
        return NAU_ERROR_INVALID;

    swap_lane(&e->lanes[param], ramps, count);
    return NAU_OK;
}
