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
 * Sidechain ducker: the gain of the main input (input 0) goes down while the
 * key (input 1) is heard, and comes back up afterwards.
 *
 * The state advances one frame at a time: the output does not depend on how
 * the rendering is split into blocks.
 */

#include <math.h>
#include <stdbool.h>

#include "nopeaudio/nopeaudio.h"
#include "effect/effect.h"
#include "utils/utils.h"

static const struct nau_param_info ducker_params[] = {
    [NAU_DUCKER_THRESHOLD] = {"threshold", 0.00001f, 1.f, 0.031622777f, NAU_PARAM_LOG},
    [NAU_DUCKER_VOLUME] = {"volume", 0.f, 1.f, 0.25118864f, NAU_PARAM_LINEAR},
    [NAU_DUCKER_ATTACK] = {"attack", 0.f, 10.f, 0.05f, NAU_PARAM_LINEAR},
    [NAU_DUCKER_HOLD] = {"hold", 0.f, 10.f, 0.2f, NAU_PARAM_LINEAR},
    [NAU_DUCKER_RELEASE] = {"release", 0.f, 10.f, 0.4f, NAU_PARAM_LINEAR},
};

struct ducker {
    bool settled;           /* false until the first frame rendered after a jump */
    uint32_t hold_left;     /* frames left before the main input is released */
    double depth;           /* 0: main input at its own volume, 1: fully ducked */
};

/* Level of the key at a frame: the peak of its channels */
static float key_level(const float *frame, uint32_t channels)
{
    float level = 0.f;
    for (uint32_t c = 0; c < channels; c++)
        level = fmaxf(level, fabsf(frame[c]));
    return level;
}

/* Change of the depth per frame, for a ramp of `seconds` */
static double depth_step(float seconds, uint32_t sample_rate)
{
    const double frames = round((double)seconds * sample_rate);
    return frames >= 1. ? 1. / frames : 1.;
}

static void ducker_render(void *state, const float *params, const struct nau_block *p)
{
    struct ducker *d = state;
    const float threshold = params[NAU_DUCKER_THRESHOLD];
    const float volume = params[NAU_DUCKER_VOLUME];
    const double attack_step = depth_step(params[NAU_DUCKER_ATTACK], p->sample_rate);
    const double release_step = depth_step(params[NAU_DUCKER_RELEASE], p->sample_rate);
    const uint32_t hold = (uint32_t)lround((double)params[NAU_DUCKER_HOLD] * p->sample_rate);
    /* The hold may have been shortened */
    if (d->hold_left > hold)
        d->hold_left = hold;
    for (uint32_t i = 0; i < p->frames; i++) {
        const size_t first = (size_t)i * p->channels;
        const float level = key_level(p->inputs[1] + first, p->channels);
        bool ducked = true;
        if (level >= threshold)
            d->hold_left = hold;
        else if (d->hold_left)
            d->hold_left--;
        else
            ducked = false;
        if (!d->settled) {
            d->depth = ducked ? 1. : 0.;
            d->settled = true;
        } else if (ducked) {
            d->depth = fmin(d->depth + attack_step, 1.);
        } else {
            d->depth = fmax(d->depth - release_step, 0.);
        }
        const float gain = (float)(1. + ((double)volume - 1.) * d->depth);
        for (uint32_t c = 0; c < p->channels; c++)
            p->output[first + c] = p->inputs[0][first + c] * gain;
    }
}

/* After a jump, the history of the key is unknown: the first frame rendered
 * sets the depth at once (see `settled`), instead of ramping to it */
static void ducker_reset(void *state)
{
    struct ducker *d = state;
    d->settled = false;
    d->hold_left = 0;
    d->depth = 0.;
}

const struct nau_effect_class nau_effect_ducker = {
    .info = {
        .name = "ducker",
        .params = ducker_params,
        .param_count = NAU_ARRAY_COUNT(ducker_params),
        .sidechain = 1,
    },
    .state_size = sizeof(struct ducker),
    .reset = ducker_reset,
    .render = ducker_render,
};
