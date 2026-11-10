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
 * Low-pass filter, made of one (12 dB per octave) or two (24 dB per octave)
 * state variable filters using trapezoidal integrators (Andrew Simper,
 * "Linear Trapezoidal Integrated State Variable Filter", Cytomic, 2013).
 *
 * This structure stays stable while its coefficients change, which the ramps
 * of the cutoff require. Each stage has the response of the analog filter,
 * with its frequencies warped by the bilinear transform.
 */

#include <math.h>
#include <string.h>

#include "nopeaudio/nopeaudio.h"
#include "effect/effect.h"
#include "internal.h"
#include "utils/utils.h"

#define MIN_CUTOFF 10.f
#define MAX_CUTOFF 0.49f        /* fraction of the sample rate: the filter is open */
#define MIN_RESONANCE 0.1f
#define MAX_RESONANCE 40.f
/*
 * Q of the two stages of the 24 dB per octave filter. With a flat resonance,
 * they are those of a 4th order Butterworth filter: 1 / (2 cos(pi/8)) and
 * 1 / (2 cos(3pi/8)) = 1.30656296. The Q of the second stage is proportional
 * to the resonance, so that the gain at the cutoff (Q1 x Q2) is the resonance.
 */
#define Q1_24DB 0.54119610
#define Q2_24DB 1.84775907      /* multiplied by the resonance */

static const struct nau_param_info lowpass_params[] = {
    [NAU_LOWPASS_CUTOFF] = {"cutoff", MIN_CUTOFF, NAU_LOWPASS_OPEN, NAU_LOWPASS_OPEN, NAU_PARAM_LOG},
    [NAU_LOWPASS_RESONANCE] = {"resonance", MIN_RESONANCE, MAX_RESONANCE, NAU_LOWPASS_FLAT, NAU_PARAM_LINEAR},
    [NAU_LOWPASS_SLOPE] = {"slope", NAU_LOWPASS_12DB, NAU_LOWPASS_24DB, NAU_LOWPASS_24DB, NAU_PARAM_STEPPED},
    [NAU_LOWPASS_AMOUNT] = {"amount", 0.f, 4.f, 1.f, NAU_PARAM_LINEAR},
    [NAU_LOWPASS_RESONANCE_SCALE] = {"resonance_scale", 0.f, 100.f, 1.f, NAU_PARAM_LINEAR},
};

/* Coefficients of a state variable filter */
struct svf {
    float a1, a2, a3;
};

struct lowpass {
    /* Those of the current coefficients, a cutoff of 0 before the first update */
    int slope;
    float cutoff;
    float resonance;
    struct svf stages[2];
    float state[NAU_MAX_CHANNELS][2][2];    /* integrators, per channel and stage */
};

static float open_cutoff(uint32_t sample_rate)
{
    return fmaxf(fminf(MAX_CUTOFF * (float)sample_rate, NAU_LOWPASS_OPEN), MIN_CUTOFF);
}

static void lowpass_limit(uint32_t param, uint32_t sample_rate, float *min, float *max)
{
    if (param == NAU_LOWPASS_CUTOFF)
        *max = open_cutoff(sample_rate);
}

static struct svf svf_coefficients(double g, double q)
{
    const double a1 = 1. / (1. + g * (g + 1. / q));
    return (struct svf){(float)a1, (float)(g * a1), (float)(g * g * a1)};
}

/* Zero the tiny values. In silence, the integrators would otherwise decay
 * down to denormals, which are slow on some CPUs. */
static float flush_tiny(float x)
{
    return fabsf(x) < 1e-20f ? 0.f : x;
}

static float svf_lowpass(const struct svf *c, float *ic, float v0)
{
    const float v3 = v0 - ic[1];
    const float v1 = c->a1 * ic[0] + c->a2 * v3;
    const float v2 = ic[1] + c->a2 * ic[0] + c->a3 * v3;
    ic[0] = flush_tiny(2.f * v1 - ic[0]);
    ic[1] = flush_tiny(2.f * v2 - ic[1]);
    return v2;
}

static void update_coefficients(struct lowpass *f, int slope, float cutoff, float resonance, uint32_t sample_rate)
{
    if (slope == f->slope && cutoff == f->cutoff && resonance == f->resonance)
        return;
    /* The second stage joins in from silence */
    if (slope != f->slope && slope == NAU_LOWPASS_24DB)
        for (uint32_t c = 0; c < NAU_MAX_CHANNELS; c++)
            memset(f->state[c][1], 0, sizeof(f->state[c][1]));
    f->slope = slope;
    f->cutoff = cutoff;
    f->resonance = resonance;
    const double g = tan(NAU_PI_F64 * (double)cutoff / (double)sample_rate);
    if (slope == NAU_LOWPASS_12DB) {
        f->stages[0] = svf_coefficients(g, (double)resonance);
    } else {
        f->stages[0] = svf_coefficients(g, Q1_24DB);
        f->stages[1] = svf_coefficients(g, Q2_24DB * (double)resonance);
    }
}

static void lowpass_render(void *state, const float *params, const struct nau_block *p)
{
    struct lowpass *f = state;
    const float open = open_cutoff(p->sample_rate);
    float cutoff = params[NAU_LOWPASS_CUTOFF];
    float resonance = params[NAU_LOWPASS_RESONANCE];
    /* The amount scales the distance, in octaves, between the cutoff and
     * the open filter */
    const float amount = params[NAU_LOWPASS_AMOUNT];
    if (amount != 1.f) {
        const double octaves = (double)amount * log2((double)cutoff / (double)open);
        cutoff = fminf(fmaxf((float)((double)open * exp2(octaves)), MIN_CUTOFF), open);
    }
    const float scale = params[NAU_LOWPASS_RESONANCE_SCALE];
    if (scale != 1.f)
        resonance = fminf(fmaxf(resonance * scale, MIN_RESONANCE), MAX_RESONANCE);
    const int slope = (int)params[NAU_LOWPASS_SLOPE];
    update_coefficients(f, slope, cutoff, resonance, p->sample_rate);
    const uint32_t stage_count = slope == NAU_LOWPASS_24DB ? 2 : 1;
    for (uint32_t i = 0; i < p->frames; i++) {
        for (uint32_t c = 0; c < p->channels; c++) {
            const size_t index = (size_t)i * p->channels + c;
            float v = p->inputs[0][index];
            for (uint32_t stage = 0; stage < stage_count; stage++)
                v = svf_lowpass(&f->stages[stage], f->state[c][stage], v);
            p->output[index] = v;
        }
    }
}

/* After a jump, the filter starts from silence */
static void lowpass_reset(void *state)
{
    struct lowpass *f = state;
    memset(f->state, 0, sizeof(f->state));
}

const struct nau_effect_class nau_effect_lowpass = {
    .info = {
        .name = "lowpass",
        .params = lowpass_params,
        .param_count = NAU_ARRAY_COUNT(lowpass_params),
    },
    .state_size = sizeof(struct lowpass),
    .limit = lowpass_limit,
    .reset = lowpass_reset,
    .render = lowpass_render,
};
