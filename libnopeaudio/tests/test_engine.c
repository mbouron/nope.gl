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

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nopeaudio/nopeaudio.h"
#include "utils/utils.h"

struct signal {
    int calls, resets, destroyed;
    float value;
};

static int constant(void *user, const struct nau_block *p)
{
    struct signal *s = user;
    s->calls++;
    for (size_t i = 0; i < (size_t)p->frames * p->channels; i++)
        p->output[i] = s->value;
    return NAU_OK;
}

static void reset(void *user, int64_t frame)
{
    ((struct signal *)user)->resets++;
}

static void destroy(void *user)
{
    ((struct signal *)user)->destroyed++;
}

static struct nau_node *signal_node(struct nau_engine *e, struct signal *s)
{
    struct nau_node *n = NULL;
    struct nau_processor p = {.user = s, .process = constant, .reset = reset, .destroy = destroy};
    n = nau_node_create();
    assert(n);
    assert(nau_node_init(n, e, &p) == NAU_OK);
    return n;
}

static void close_to(float a, float b)
{
    assert(fabsf(a - b) < 0.00001f);
}

static void test_mix_edit(void)
{
    struct nau_engine *e = NULL;
    struct nau_engine_config config = {.sample_rate = 48000, .channels = 2, .block_frames = 4};
    e = nau_engine_create();
    assert(e);
    assert(nau_engine_init(e, &config) == NAU_OK);
    struct signal a = {.value = 1}, b = {.value = 2};
    struct nau_node *na = signal_node(e, &a), *nb = signal_node(e, &b), *gain = NULL, *mix = NULL;
    gain = nau_node_create();
    assert(gain);
    assert(nau_gain_init(gain, e, 0.5f, 4) == NAU_OK);
    mix = nau_node_create();
    assert(mix);
    assert(nau_mix_init(mix, e, 2) == NAU_OK);
    /* Deliberately not topologically ordered; na fans out twice. */
    uint32_t mix_inputs[] = {1, 2}, gain_inputs[] = {2};
    struct nau_route routes[] = {{mix, mix_inputs}, {gain, gain_inputs}, {na, NULL}, {nb, NULL}};
    assert(nau_engine_commit(e, routes, 4, 0) == NAU_OK);
    assert(nau_engine_commit(e, routes, 4, 0) == NAU_AGAIN);
    float out[22];
    assert(nau_engine_render(e, 0, 3, out) == NAU_OK);
    for (int i = 0; i < 6; i++)
        close_to(out[i], 1.5f);
    assert(a.calls == 1 && b.calls == 0); /* Reachability and fan-out. */
    assert(nau_gain_set(gain, 1) == NAU_OK);
    assert(nau_engine_render(e, 3, 2, out) == NAU_OK);
    close_to(out[0], 1.625f);
    close_to(out[2], 1.75f);
    /* Preserve a half-completed fader ramp across a topology replacement. */
    mix_inputs[1] = 3;
    assert(nau_engine_commit(e, routes, 4, 0) == NAU_OK);
    nau_node_unrefp(&na);
    nau_node_unrefp(&nb);
    nau_node_unrefp(&gain);
    nau_node_unrefp(&mix);
    assert(nau_engine_render(e, 5, 11, out) == NAU_OK);
    close_to(out[0], 2.875f);
    close_to(out[2], 3);
    for (int i = 4; i < 22; i++)
        close_to(out[i], 3);
    assert(a.resets == 1 && b.resets == 1);
    assert(nau_engine_collect(e) == NAU_OK);
    assert(a.destroyed == 0 && b.destroyed == 0);
    assert(nau_engine_render(e, 100, 1, out) == NAU_OK);
    assert(a.resets == 2 && b.resets == 2);
    assert(nau_engine_commit(e, NULL, 0, 0) == NAU_OK);
    assert(nau_engine_render(e, 101, 1, out) == NAU_OK);
    close_to(out[0], 0);
    assert(a.destroyed == 0); /* No destructors on render thread. */
    assert(nau_engine_collect(e) == NAU_OK);
    assert(a.destroyed == 1 && b.destroyed == 1);
    nau_engine_freep(&e);
}

static int sample_numbers(void *user, int64_t first, uint32_t frames, float *out)
{
    for (uint32_t i = 0; i < frames; i++)
        out[i] = (float)(first + i);
    return NAU_OK;
}

static void test_schedule(void)
{
    struct nau_engine *e = NULL;
    struct nau_engine_config c = {48000, 1, 4};
    e = nau_engine_create();
    assert(e);
    assert(nau_engine_init(e, &c) == NAU_OK);
    struct nau_source s = {.read = sample_numbers, .timeline_start = 3, .source_start = 17, .duration = 5};
    struct nau_node *n = NULL;
    n = nau_node_create();
    assert(n);
    assert(nau_source_init(n, e, &s) == NAU_OK);
    struct nau_route r = {n, NULL};
    assert(nau_engine_commit(e, &r, 1, 0) == NAU_OK);
    float out[11];
    assert(nau_engine_render(e, 0, 11, out) == NAU_OK);
    const float expected[] = {0, 0, 0, 17, 18, 19, 20, 21, 0, 0, 0};
    assert(!memcmp(out, expected, sizeof(out)));
    assert(nau_source_set_range(n, 0, 21, 11) == NAU_OK);
    /* Editing the authoring range cannot mutate a published plan. */
    assert(nau_engine_render(e, 0, 11, out) == NAU_OK);
    assert(!memcmp(out, expected, sizeof(out)));
    assert(nau_engine_commit(e, &r, 1, 0) == NAU_OK);
    assert(nau_engine_render(e, 0, 11, out) == NAU_OK);
    for (uint32_t i = 0; i < 11; i++)
        close_to(out[i], (float)(21 + i));
    nau_node_unrefp(&n);
    nau_engine_freep(&e);
}

static void test_validation(void)
{
    struct nau_engine *e = NULL;
    struct nau_engine_config c = {48000, 2, 4};
    e = nau_engine_create();
    assert(e);
    assert(nau_engine_init(e, &c) == NAU_OK);
    struct nau_node *a = NULL, *b = NULL;
    a = nau_node_create();
    assert(a);
    assert(nau_gain_init(a, e, NAN, 1) == NAU_ERROR_INVALID);
    assert(nau_gain_init(a, e, 1, 1) == NAU_OK);
    b = nau_node_create();
    assert(b);
    assert(nau_gain_init(b, e, 1, 1) == NAU_OK);
    uint32_t ia[] = {1}, ib[] = {0};
    struct nau_route r[] = {{a, ia}, {b, ib}};
    assert(nau_engine_commit(e, r, 2, 0) == NAU_ERROR_INVALID); /* Cycle. */
    ia[0] = 7;
    assert(nau_engine_commit(e, r, 2, 0) == NAU_ERROR_INVALID);
    ia[0] = NAU_INPUT_NONE;
    r[1].node = a;
    assert(nau_engine_commit(e, r, 2, 0) == NAU_ERROR_INVALID); /* Same instance twice. */
    r[1].node = b;
    assert(nau_engine_commit(e, r, 2, 1) == NAU_OK);
    nau_node_unrefp(&a);
    nau_node_unrefp(&b);
    nau_engine_freep(&e); /* Also frees an unadopted plan. */
}

static int ones(void *user, const struct nau_block *p)
{
    for (uint32_t i = 0; i < p->frames * p->channels; i++)
        p->output[i] = 1;
    return NAU_OK;
}

static void test_ramp_shapes(void)
{
    /* Linear, the progress kept within [0, 1] */
    close_to(nau_ramp_volume(NAU_RAMP_LINEAR, 0, 0, 1, 0.25f), 0.25f);
    close_to(nau_ramp_volume(NAU_RAMP_LINEAR, 0, 0.2f, 1, -1), 0.2f);
    close_to(nau_ramp_volume(NAU_RAMP_LINEAR, 0, 0.2f, 1, 2), 1);
    /* Linear in dB: halfway from 0 to -20 dB is -10 dB */
    close_to(nau_ramp_volume(NAU_RAMP_DB, 0, 1, 0.1f, 0.5f), sqrtf(0.1f));
    /* Equal power: a fade in and a fade out crossing keep the power */
    for (int i = 0; i <= 10; i++) {
        const float p = (float)i / 10;
        const float in = nau_ramp_volume(NAU_RAMP_EQUAL_POWER, 0, 0, 1, p);
        const float out = nau_ramp_volume(NAU_RAMP_EQUAL_POWER, 0, 1, 0, p);
        close_to(in * in + out * out, 1);
    }
    /* S-curve: symmetric, easing in and out */
    close_to(nau_ramp_volume(NAU_RAMP_S_CURVE, 0, 0, 1, 0.5f), 0.5f);
    close_to(nau_ramp_volume(NAU_RAMP_S_CURVE, 0, 0, 1, 0.25f), 0.15625f);
    close_to(nau_ramp_volume(NAU_RAMP_S_CURVE, 0, 2, 0, 0.75f), 2 * 0.15625f);
    /* Curvature: the target reached earlier when positive, later when negative */
    close_to(nau_ramp_volume(NAU_RAMP_LINEAR, 1, 0, 1, 0.5f), powf(0.5f, 0.125f));
    close_to(nau_ramp_volume(NAU_RAMP_LINEAR, -1, 0, 1, 0.5f), powf(0.5f, 8));
    close_to(nau_ramp_volume(NAU_RAMP_LINEAR, 1, 1, 0, 0.5f), 1 - powf(0.5f, 0.125f));
    /* The ends, whatever the shape */
    for (int shape = NAU_RAMP_LINEAR; shape <= NAU_RAMP_S_CURVE; shape++) {
        close_to(nau_ramp_volume((enum nau_ramp_shape)shape, 0.5f, 0.3f, 0.8f, 0), 0.3f);
        close_to(nau_ramp_volume((enum nau_ramp_shape)shape, 0.5f, 0.3f, 0.8f, 1), 0.8f);
    }
}

static void test_db_ramps(void)
{
    /* Silence and constant quiet gains must remain unchanged. */
    assert(nau_ramp_volume(NAU_RAMP_DB, 0, 0, 0, 0.5f) == 0);
    assert(nau_ramp_volume(NAU_RAMP_DB, 0, 0.0001f, 0.0001f, 0.5f) == 0.0001f);
    /* Gains up to 60 dB apart interpolate in dB, however quiet. */
    close_to(nau_ramp_volume(NAU_RAMP_DB, 0, 1e-6f, 1e-4f, 0.5f) / 1e-5f, 1);
    close_to(nau_ramp_volume(NAU_RAMP_DB, 0, 0.001f, 1, 0.5f), sqrtf(0.001f));
    /* Further apart, as silence, along the curve of a 60 dB ramp: (1000^p - 1) / 999 */
    close_to(nau_ramp_volume(NAU_RAMP_DB, 0, 0, 1, 0.5f), 0.03065343f);
    close_to(nau_ramp_volume(NAU_RAMP_DB, 0, 1, 0, 0.5f), 0.03065343f);
    close_to(nau_ramp_volume(NAU_RAMP_DB, 0, 0, 2, 0.5f), 2 * 0.03065343f);
    /* Close to silence as from silence: no jump as a gain reaches zero */
    close_to(nau_ramp_volume(NAU_RAMP_DB, 0, 1e-9f, 1, 0.5f), 0.03065343f);
    close_to(nau_ramp_volume(NAU_RAMP_DB, 0, 1, 1e-9f, 0.5f), 0.03065343f);
    close_to(nau_ramp_volume(NAU_RAMP_DB, 0, 1e-4f, 1, 0.5f), 0.03075f);
    const float rising = nau_ramp_volume(NAU_RAMP_DB, 0, 0, 1, 1e-6f);
    const float falling = nau_ramp_volume(NAU_RAMP_DB, 0, 1, 0, 1 - 1e-6f);
    assert(rising > 0 && rising < 1e-7f);
    assert(falling > 0 && falling < 1e-7f);

    const float endpoints[][2] = {
        {0, 0}, {1e-4f, 1e-4f}, {1e-6f, 1e-4f},
        {0, 1e-4f}, {0, 1}, {0, FLT_MAX}, {1e-9f, 1}, {FLT_MIN, FLT_MAX},
    };
    for (size_t i = 0; i < sizeof(endpoints) / sizeof(*endpoints); i++) {
        for (int reverse = 0; reverse < 2; reverse++) {
            const float from = endpoints[i][reverse], to = endpoints[i][1 - reverse];
            for (int bend = -1; bend <= 1; bend++) {
                const float curvature = (float)bend;
                /* Exact endpoints, including clamped progress. */
                assert(nau_ramp_volume(NAU_RAMP_DB, curvature, from, to, -1) == from);
                assert(nau_ramp_volume(NAU_RAMP_DB, curvature, from, to, 0) == from);
                assert(nau_ramp_volume(NAU_RAMP_DB, curvature, from, to, 1) == to);
                assert(nau_ramp_volume(NAU_RAMP_DB, curvature, from, to, 2) == to);
                float previous = from;
                for (int j = 1; j <= 32; j++) {
                    const float v = nau_ramp_volume(NAU_RAMP_DB, curvature, from, to, (float)j / 32);
                    assert(isfinite(v));
                    assert(v >= fminf(from, to) && v <= fmaxf(from, to));
                    assert(to >= from ? v >= previous : v <= previous);
                    previous = v;
                }
            }
        }
    }
}

static void test_fader(void)
{
    struct nau_engine *e = NULL;
    const struct nau_engine_config c = {48000, 2, 16};
    e = nau_engine_create();
    assert(e);
    assert(nau_engine_init(e, &c) == NAU_OK);
    struct nau_node *one = NULL, *fader = NULL;
    const struct nau_processor p = {.process = ones};
    one = nau_node_create();
    assert(one);
    assert(nau_node_init(one, e, &p) == NAU_OK);
    fader = nau_node_create();
    assert(fader);
    assert(nau_fader_init(fader, e) == NAU_OK);
    uint32_t in[] = {0};
    const struct nau_route r[] = {{one, NULL}, {fader, in}};
    assert(nau_engine_commit(e, r, 2, 1) == NAU_OK);
    float out[2 * 40];
    assert(nau_engine_render(e, 0, 40, out) == NAU_OK);
    for (int i = 0; i < 80; i++)
        close_to(out[i], 1);
    const struct nau_ramp k[] = {{10, 0, 0.5f, NAU_RAMP_LINEAR, 0}, {20, 10, 1.5f, NAU_RAMP_LINEAR, 0}, {35, 2, 0, NAU_RAMP_LINEAR, 0}};
    assert(nau_fader_set_ramps(fader, k, 3) == NAU_OK);
    /* Snapshotted by the next commit */
    assert(nau_engine_render(e, 0, 40, out) == NAU_OK);
    close_to(out[30], 1);
    assert(nau_engine_commit(e, r, 2, 1) == NAU_OK);
    assert(nau_engine_render(e, 0, 40, out) == NAU_OK);
    for (int i = 0; i < 40; i++) {
        float v = i < 10 ? 1 : i < 20 ? 0.5f : i < 30 ? 0.5f + (float)(i - 20) / 10 : i < 35 ? 1.5f : i < 37 ? 1.5f * (1 - (float)(i - 35) / 2) : 0;
        close_to(out[2 * i], v);
        close_to(out[2 * i + 1], v);
    }
    /* From any frame, as after a seek */
    assert(nau_engine_render(e, 25, 1, out) == NAU_OK);
    close_to(out[0], 1);
    /* Shaped and bent, as nau_ramp_volume() tells */
    const struct nau_ramp shaped[] = {{0, 0, 0, NAU_RAMP_LINEAR, 0}, {10, 20, 1, NAU_RAMP_S_CURVE, 0.5f}};
    assert(nau_fader_set_ramps(fader, shaped, 2) == NAU_OK);
    assert(nau_engine_commit(e, r, 2, 1) == NAU_OK);
    assert(nau_engine_render(e, 0, 40, out) == NAU_OK);
    for (int i = 0; i < 40; i++) {
        const float v = i < 10 ? 0 : i < 30 ? nau_ramp_volume(NAU_RAMP_S_CURVE, 0.5f, 0, 1, (float)(i - 10) / 20) : 1;
        close_to(out[2 * i], v);
        close_to(out[2 * i + 1], v);
    }
    /* dB fades preserve silence and quiet plateaus, including their boundaries. */
    const struct nau_ramp db[] = {
        {0, 0, 0, NAU_RAMP_LINEAR, 0},
        {0, 4, 0, NAU_RAMP_DB, 0},
        {4, 8, 0.0001f, NAU_RAMP_DB, 0},
        {12, 8, 0.0001f, NAU_RAMP_DB, 0},
        {20, 8, 0, NAU_RAMP_DB, 0},
        {28, 8, 0, NAU_RAMP_DB, 0},
    };
    assert(nau_fader_set_ramps(fader, db, sizeof(db) / sizeof(*db)) == NAU_OK);
    assert(nau_engine_commit(e, r, 2, 1) == NAU_OK);
    assert(nau_engine_render(e, 0, 40, out) == NAU_OK);
    for (int i = 0; i < 40; i++) {
        float v = 0;
        if (i >= 4 && i <= 12)
            v = nau_ramp_volume(NAU_RAMP_DB, 0, 0, 0.0001f, (float)(i - 4) / 8);
        else if (i > 12 && i < 20)
            v = 0.0001f;
        else if (i >= 20 && i <= 28)
            v = nau_ramp_volume(NAU_RAMP_DB, 0, 0.0001f, 0, (float)(i - 20) / 8);
        assert(out[2 * i] == v && out[2 * i + 1] == v);
    }
    /* Seeking gives the same values as rendering across processing blocks. */
    for (int i = 0; i < 40; i += 4) {
        float sample[2];
        assert(nau_engine_render(e, i, 1, sample) == NAU_OK);
        assert(!memcmp(sample, out + 2 * i, sizeof(sample)));
    }
    const struct nau_ramp bad_shape[] = {{10, 0, 1, (enum nau_ramp_shape)7, 0}};
    assert(nau_fader_set_ramps(fader, bad_shape, 1) == NAU_ERROR_INVALID);
    const struct nau_ramp bad_curvature[] = {{10, 0, 1, NAU_RAMP_LINEAR, 2}};
    assert(nau_fader_set_ramps(fader, bad_curvature, 1) == NAU_ERROR_INVALID);
    const struct nau_ramp nan_curvature[] = {{10, 0, 1, NAU_RAMP_LINEAR, NAN}};
    assert(nau_fader_set_ramps(fader, nan_curvature, 1) == NAU_ERROR_INVALID);
    const struct nau_ramp unsorted[] = {{10, 0, 1, NAU_RAMP_LINEAR, 0}, {5, 0, 1, NAU_RAMP_LINEAR, 0}};
    assert(nau_fader_set_ramps(fader, unsorted, 2) == NAU_ERROR_INVALID);
    const struct nau_ramp negative[] = {{10, 0, -1, NAU_RAMP_LINEAR, 0}};
    assert(nau_fader_set_ramps(fader, negative, 1) == NAU_ERROR_INVALID);
    assert(nau_fader_set_ramps(one, k, 1) == NAU_ERROR_INVALID);
    assert(nau_fader_set_ramps(fader, NULL, 0) == NAU_OK);
    assert(nau_engine_commit(e, r, 2, 1) == NAU_OK);
    assert(nau_engine_render(e, 30, 1, out) == NAU_OK);
    close_to(out[0], 1);

    /* The ramps count from the origin of the node */
    assert(nau_fader_set_ramps(fader, k, 3) == NAU_OK);
    assert(nau_engine_commit(e, r, 2, 1) == NAU_OK);
    assert(nau_engine_render(e, 0, 40, out) == NAU_OK);
    float moved[2 * 40];
    assert(nau_node_set_origin(fader, 1001) == NAU_OK);
    assert(nau_node_set_origin(fader, -1) == NAU_ERROR_INVALID);
    assert(nau_node_set_origin(NULL, 0) == NAU_ERROR_INVALID);
    assert(nau_engine_commit(e, r, 2, 1) == NAU_OK);
    assert(nau_engine_render(e, 1001, 40, moved) == NAU_OK);
    assert(!memcmp(moved, out, sizeof(out)));
    assert(nau_engine_render(e, 1000, 1, moved) == NAU_OK);
    close_to(moved[0], 1);
    nau_node_unrefp(&one);
    nau_node_unrefp(&fader);
    nau_engine_freep(&e);
}

#define RATE 48000

/* A sine of its frequency over the project frames, its opposite on the right */
struct tone {
    double frequency;
};

static int tone_process(void *user, const struct nau_block *p)
{
    const struct tone *t = user;
    for (uint32_t i = 0; i < p->frames; i++) {
        const float v = (float)sin(2. * NAU_PI_F64 * t->frequency * (double)(p->frame + i) / RATE);
        p->output[2 * i] = v;
        p->output[2 * i + 1] = -v;
    }
    return NAU_OK;
}

/* The amplitude of the left channel at a frequency, over whole periods */
static double amplitude(const float *x, int64_t first, uint32_t frames, double frequency)
{
    double a = 0, b = 0;
    for (uint32_t i = 0; i < frames; i++) {
        const double phase = 2. * NAU_PI_F64 * frequency * (double)(first + i) / RATE;
        a += x[2 * i] * sin(phase);
        b += x[2 * i] * cos(phase);
    }
    return 2. * sqrt(a * a + b * b) / frames;
}

/* The gain of the filter at a frequency: the analog one's, its band warped as by the bilinear transform */
static double response(enum nau_lowpass_slope slope, double cutoff, double resonance, double frequency)
{
    const double w = tan(NAU_PI_F64 * frequency / RATE) / tan(NAU_PI_F64 * cutoff / RATE);
    const double q[2] = {slope == NAU_LOWPASS_12DB ? resonance : 0.54119610, 1.84775907 * resonance};
    double gain = 1;
    for (int i = 0; i < (slope == NAU_LOWPASS_12DB ? 1 : 2); i++)
        gain /= sqrt((1 - w * w) * (1 - w * w) + (w / q[i]) * (w / q[i]));
    return gain;
}

static void near(double a, double b, double tolerance)
{
    if (fabs(a - b) > tolerance) {
        fprintf(stderr, "%g != %g (+/- %g)\n", a, b, tolerance);
        assert(0);
    }
}

struct filtered {
    struct nau_engine *engine;
    struct nau_node *tone, *lowpass;
    struct tone sine;
};

/* A ramp of the cutoff and of the resonance of a low-pass filter */
struct filter_ramp {
    int64_t frame, length;
    float cutoff, resonance;
};

static void filtered_init(struct filtered *f, enum nau_lowpass_slope slope, double frequency)
{
    const struct nau_engine_config c = {RATE, 2, 64};
    f->engine = nau_engine_create();
    assert(f->engine);
    assert(nau_engine_init(f->engine, &c) == NAU_OK);
    f->sine.frequency = frequency;
    const struct nau_processor p = {.user = &f->sine, .process = tone_process};
    f->tone = nau_node_create();
    assert(f->tone);
    assert(nau_node_init(f->tone, f->engine, &p) == NAU_OK);
    f->lowpass = nau_node_create();
    assert(f->lowpass);
    assert(nau_effect_init(f->lowpass, f->engine, NAU_EFFECT_LOWPASS, 0) == NAU_OK);
    assert(nau_effect_set_param(f->lowpass, NAU_LOWPASS_SLOPE, (float)slope) == NAU_OK);
}

/* Automate the cutoff and the resonance with the same ramps, and commit */
static void filtered_commit(struct filtered *f, const struct filter_ramp *ramps, uint32_t count)
{
    struct nau_ramp cutoffs[4], resonances[4];
    assert(count <= 4);
    for (uint32_t i = 0; i < count; i++) {
        cutoffs[i] = (struct nau_ramp){.frame = ramps[i].frame, .length = ramps[i].length, .value = ramps[i].cutoff};
        resonances[i] = (struct nau_ramp){.frame = ramps[i].frame, .length = ramps[i].length, .value = ramps[i].resonance};
    }
    assert(nau_effect_set_automation(f->lowpass, NAU_LOWPASS_CUTOFF, cutoffs, count) == NAU_OK);
    assert(nau_effect_set_automation(f->lowpass, NAU_LOWPASS_RESONANCE, resonances, count) == NAU_OK);
    static const uint32_t in[] = {0};
    const struct nau_route r[] = {{f->tone, NULL}, {f->lowpass, in}};
    assert(nau_engine_commit(f->engine, r, 2, 1) == NAU_OK);
}

static void filtered_uninit(struct filtered *f)
{
    nau_node_unrefp(&f->tone);
    nau_node_unrefp(&f->lowpass);
    nau_engine_freep(&f->engine);
}

/* The amplitude of the output over [from, from + frames), rendered from `start` */
static double filtered_amplitude(struct filtered *f, int64_t start, int64_t from, uint32_t frames)
{
    const uint32_t n = (uint32_t)(from - start) + frames;
    float *out = calloc(2 * (size_t)n, sizeof(*out));
    assert(out);
    assert(nau_engine_render(f->engine, start, n, out) == NAU_OK);
    for (uint32_t i = 0; i < n; i++)
        assert(out[2 * i + 1] == -out[2 * i]);
    const double a = amplitude(out + 2 * (from - start), from, frames, f->sine.frequency);
    free(out);
    return a;
}

/* Settled, the filter responds as the analog filter does */
static void test_lowpass_response(void)
{
    const enum nau_lowpass_slope slopes[] = {NAU_LOWPASS_12DB, NAU_LOWPASS_24DB};
    const float resonances[] = {NAU_LOWPASS_FLAT, 4.f};
    const double frequencies[] = {250, 1000, 4000};
    for (int s = 0; s < 2; s++) {
        for (int r = 0; r < 2; r++) {
            for (int i = 0; i < 3; i++) {
                struct filtered f = {0};
                filtered_init(&f, slopes[s], frequencies[i]);
                const struct filter_ramp k = {.cutoff = 1000, .resonance = resonances[r]};
                filtered_commit(&f, &k, 1);
                const double expected = response(slopes[s], 1000, resonances[r], frequencies[i]);
                near(filtered_amplitude(&f, 0, 9600, 4800), expected, 0.01 * expected + 1e-6);
                filtered_uninit(&f);
            }
        }
        /* At the cutoff, the resonance is the gain */
        near(response(slopes[s], 1000, 4, 1000), 4, 1e-6);
    }

    /* Without automation, open */
    struct filtered f = {0};
    filtered_init(&f, NAU_LOWPASS_24DB, 4000);
    filtered_commit(&f, NULL, 0);
    near(filtered_amplitude(&f, 0, 4800, 4800), 1, 1e-3);
    filtered_uninit(&f);
}

/* The cutoff moves by octaves, the resonance linearly */
static void test_lowpass_ramp(void)
{
    struct filtered f = {0};
    filtered_init(&f, NAU_LOWPASS_12DB, 1000);
    /* 250 Hz to 4 kHz, resonance 2 to 6, over 100 s: at the middle, 1 kHz and 4 */
    const int64_t length = 100 * RATE, middle = RATE + length / 2;
    const struct filter_ramp k[] = {{0, 0, 250, 2}, {RATE, length, 4000, 6}};
    filtered_commit(&f, k, 2);
    near(filtered_amplitude(&f, middle - 4800, middle, 4800), 4, 0.04);
    /* Held after the ramp */
    near(filtered_amplitude(&f, RATE + length, RATE + length + 4800, 4800), response(NAU_LOWPASS_12DB, 4000, 6, 1000), 0.01);
    /* From the first point, its values */
    near(filtered_amplitude(&f, 0, 4800, 4800), response(NAU_LOWPASS_12DB, 250, 2, 1000), 0.001);
    filtered_uninit(&f);
}

/*
 * Under water: a 4 kHz tone, the filter closing to 400 Hz in 50 ms at 0.2 s,
 * opening again in 0.3 s at 0.6 s
 */
static void test_lowpass_underwater(void)
{
    struct filtered f = {0};
    filtered_init(&f, NAU_LOWPASS_24DB, 4000);
    const struct filter_ramp k[] = {
        {RATE / 5, RATE / 20, 400, 1.2f},
        {RATE * 3 / 5, RATE * 3 / 10, NAU_LOWPASS_OPEN, NAU_LOWPASS_FLAT},
    };
    filtered_commit(&f, k, 2);
    const uint32_t n = RATE * 6 / 5;
    float *out = calloc(2 * (size_t)n, sizeof(*out));
    assert(out);
    assert(nau_engine_render(f.engine, 0, n, out) == NAU_OK);
    for (uint32_t i = 0; i < 2 * n; i++)
        assert(fabsf(out[i]) < 1.5f);
    const double open = response(NAU_LOWPASS_24DB, 0.49 * RATE, NAU_LOWPASS_FLAT, 4000);
    near(open, 1, 1e-3);
    near(amplitude(out + 2 * 4800, 4800, 4800, 4000), open, 1e-3);
    const double under = response(NAU_LOWPASS_24DB, 400, 1.2, 4000);
    assert(under < 1e-4);
    near(amplitude(out + 2 * 14400, 14400, 14400, 4000), under, 0.05 * under);
    near(amplitude(out + 2 * 48000, 48000, 4800, 4000), open, 1e-3);
    free(out);
    filtered_uninit(&f);
}

/* Renders cut anywhere, or graphs committed again, sound the same */
static void test_lowpass_continuity(void)
{
    struct filtered f = {0};
    filtered_init(&f, NAU_LOWPASS_24DB, 3000);
    const struct filter_ramp k[] = {{1000, 3000, 300, 8}, {5000, 0, 2000, 0.5f}, {7001, 777, 8000, 3}};
    filtered_commit(&f, k, 3);
    enum { N = 9600 };
    static float whole[2 * N], pieces[2 * N];
    assert(nau_engine_render(f.engine, 0, N, whole) == NAU_OK);
    /* From the start again, a jump: the filter starts from silence */
    const uint32_t sizes[] = {1, 7, 13, 64, 100, 3, 255, 1000};
    for (uint32_t done = 0, i = 0; done < N; i++) {
        uint32_t size = sizes[i % 8];
        if (size > N - done)
            size = N - done;
        assert(nau_engine_render(f.engine, done, size, pieces + 2 * done) == NAU_OK);
        /* The automation committed again midway, the memory staying */
        if (done < 4800 && done + size >= 4800)
            filtered_commit(&f, k, 3);
        done += size;
    }
    assert(!memcmp(whole, pieces, sizeof(whole)));
    /* Snapshotted by the next commit */
    const struct nau_ramp closed = {.value = 10};
    assert(nau_effect_set_automation(f.lowpass, NAU_LOWPASS_CUTOFF, &closed, 1) == NAU_OK);
    assert(nau_engine_render(f.engine, 0, N, pieces) == NAU_OK);
    assert(!memcmp(whole, pieces, sizeof(whole)));
    filtered_uninit(&f);
}

/* Live, the amount scales the depth of the filter, the resonance scale its resonance */
static void test_lowpass_live(void)
{
    struct filtered f = {0};
    filtered_init(&f, NAU_LOWPASS_12DB, 1000);
    const double open = 0.49 * RATE;
    const struct filter_ramp deep = {.cutoff = 250, .resonance = 2};
    filtered_commit(&f, &deep, 1);
    /* Half the octaves below open, none, or twice as many */
    const float amounts[] = {0.5f, 0.f, 2.f};
    for (int i = 0; i < 3; i++) {
        assert(nau_effect_set_param(f.lowpass, NAU_LOWPASS_AMOUNT, amounts[i]) == NAU_OK);
        const double cutoff = fmax(open * pow(250 / open, amounts[i]), 10);
        const double expected = response(NAU_LOWPASS_12DB, cutoff, 2, 1000);
        near(filtered_amplitude(&f, 0, 9600, 4800), expected, 0.01 * expected + 1e-6);
    }
    /* At the cutoff, the resonance as scaled, up to 40 */
    assert(nau_effect_set_param(f.lowpass, NAU_LOWPASS_AMOUNT, 1) == NAU_OK);
    const struct filter_ramp at = {.cutoff = 1000, .resonance = 2};
    filtered_commit(&f, &at, 1);
    const float scales[] = {2.f, 0.5f, 100.f};
    const double gains[] = {4, 1, 40};
    for (int i = 0; i < 3; i++) {
        assert(nau_effect_set_param(f.lowpass, NAU_LOWPASS_RESONANCE_SCALE, scales[i]) == NAU_OK);
        near(filtered_amplitude(&f, 0, 9600, 4800), gains[i], 0.01 * gains[i]);
    }
    /* Kept across commits */
    filtered_commit(&f, &at, 1);
    near(filtered_amplitude(&f, 0, 9600, 4800), 40, 0.4);

    const float invalid[] = {-1.f, NAN, INFINITY};
    for (int i = 0; i < 3; i++) {
        assert(nau_effect_set_param(f.lowpass, NAU_LOWPASS_AMOUNT, invalid[i]) == NAU_ERROR_INVALID);
        assert(nau_effect_set_param(f.lowpass, NAU_LOWPASS_RESONANCE_SCALE, invalid[i]) == NAU_ERROR_INVALID);
    }
    assert(nau_effect_set_param(f.tone, NAU_LOWPASS_AMOUNT, 1) == NAU_ERROR_INVALID);
    assert(nau_effect_set_param(NULL, NAU_LOWPASS_RESONANCE_SCALE, 1) == NAU_ERROR_INVALID);
    near(filtered_amplitude(&f, 0, 9600, 4800), 40, 0.4);
    filtered_uninit(&f);
}

/* The values set ramp from where they are, and are there at once after a jump */
static void test_lowpass_live_ramp(void)
{
    struct filtered f = {0};
    filtered_init(&f, NAU_LOWPASS_24DB, 4000);
    nau_node_unrefp(&f.lowpass);
    f.lowpass = nau_node_create();
    assert(f.lowpass);
    assert(nau_effect_init(f.lowpass, f.engine, NAU_EFFECT_LOWPASS, 4800) == NAU_OK);
    const struct filter_ramp muffled = {.cutoff = 400, .resonance = 1.2f};
    filtered_commit(&f, &muffled, 1);
    const double under = response(NAU_LOWPASS_24DB, 400, 1.2, 4000);
    const double open = response(NAU_LOWPASS_24DB, 0.49 * RATE, 1.2, 4000);
    near(filtered_amplitude(&f, 0, 4800, 4800), under, 0.05 * under);
    /* Opened over 4800 frames, from 9600 */
    assert(nau_effect_set_param(f.lowpass, NAU_LOWPASS_AMOUNT, 0) == NAU_OK);
    enum { N = 9600 };
    static float out[2 * N];
    assert(nau_engine_render(f.engine, 9600, N, out) == NAU_OK);
    assert(amplitude(out, 9600, 480, 4000) < 0.01);
    near(amplitude(out + 2 * 4800, 14400, 4800, 4000), open, 1e-3);
    /* Muffled again, at once after a jump */
    assert(nau_effect_set_param(f.lowpass, NAU_LOWPASS_AMOUNT, 1) == NAU_OK);
    near(filtered_amplitude(&f, 0, 2400, 480), under, 0.05 * under);
    filtered_uninit(&f);
}

static void test_lowpass_validation(void)
{
    struct nau_engine *e = NULL;
    const struct nau_engine_config c = {RATE, 2, 16};
    e = nau_engine_create();
    assert(e);
    assert(nau_engine_init(e, &c) == NAU_OK);
    struct nau_node *lowpass = NULL;
    lowpass = nau_node_create();
    assert(lowpass);
    assert(nau_effect_init(lowpass, e, NAU_EFFECT_LOWPASS, 0) == NAU_OK);
    /* Values out of the range of their parameter, or not a step */
    const struct {
        uint32_t param;
        float value;
    } bad[] = {
        {NAU_LOWPASS_CUTOFF, 0}, {NAU_LOWPASS_CUTOFF, -1}, {NAU_LOWPASS_CUTOFF, 9.9f},
        {NAU_LOWPASS_CUTOFF, 2 * NAU_LOWPASS_OPEN}, {NAU_LOWPASS_CUTOFF, NAN}, {NAU_LOWPASS_CUTOFF, INFINITY},
        {NAU_LOWPASS_RESONANCE, 0.05f}, {NAU_LOWPASS_RESONANCE, 41}, {NAU_LOWPASS_RESONANCE, NAN},
        {NAU_LOWPASS_SLOPE, -1}, {NAU_LOWPASS_SLOPE, 0.5f}, {NAU_LOWPASS_SLOPE, 2},
        {NAU_LOWPASS_AMOUNT, 4.5f}, {NAU_LOWPASS_RESONANCE_SCALE, 101},
        {NAU_LOWPASS_RESONANCE_SCALE + 1, 1},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        const struct nau_ramp ramp = {.value = bad[i].value};
        assert(nau_effect_set_param(lowpass, bad[i].param, bad[i].value) == NAU_ERROR_INVALID);
        assert(nau_effect_set_automation(lowpass, bad[i].param, &ramp, 1) == NAU_ERROR_INVALID);
    }
    /* Ramps out of the time, unsorted, or of an unknown shape */
    const struct nau_ramp good = {.value = 1000};
    const struct nau_ramp bad_ramps[] = {
        {.frame = -1, .value = 1000},
        {.length = -1, .value = 1000},
        {.frame = INT64_MAX, .length = 1, .value = 1000},
        {.value = 1000, .shape = (enum nau_ramp_shape)4},
        {.value = 1000, .curvature = 1.5f},
    };
    for (size_t i = 0; i < sizeof(bad_ramps) / sizeof(*bad_ramps); i++)
        assert(nau_effect_set_automation(lowpass, NAU_LOWPASS_CUTOFF, &bad_ramps[i], 1) == NAU_ERROR_INVALID);
    const struct nau_ramp unsorted[] = {{.frame = 10, .value = 1000}, {.frame = 5, .value = 1000}};
    assert(nau_effect_set_automation(lowpass, NAU_LOWPASS_CUTOFF, unsorted, 2) == NAU_ERROR_INVALID);
    assert(nau_effect_set_automation(lowpass, NAU_LOWPASS_CUTOFF, NULL, 1) == NAU_ERROR_INVALID);
    assert(nau_effect_set_automation(lowpass, NAU_LOWPASS_CUTOFF, &good, 65537) == NAU_ERROR_INVALID);
    assert(nau_effect_set_automation(lowpass, NAU_LOWPASS_CUTOFF, &good, 1) == NAU_OK);
    assert(nau_effect_set_automation(lowpass, NAU_LOWPASS_CUTOFF, NULL, 0) == NAU_OK);
    assert(nau_effect_set_param(lowpass, NAU_LOWPASS_SLOPE, NAU_LOWPASS_12DB) == NAU_OK);
    nau_node_unrefp(&lowpass);
    nau_engine_freep(&e);
}

/* Get the description of an effect type, NULL past the last one */
static const struct nau_effect_info *effect_info(uint32_t type)
{
    return nau_effect_get_info((enum nau_effect_type)type);
}

/* Every effect type, listed from 0 until NULL, is described consistently */
static void test_effect_info(void)
{
    uint32_t count = 0;
    for (; effect_info(count); count++) {
        const struct nau_effect_info *info = effect_info(count);
        assert(info->name && *info->name);
        for (uint32_t j = 0; j < count; j++)
            assert(strcmp(info->name, effect_info(j)->name));
        assert(!info->param_count || info->params);
        for (uint32_t i = 0; i < info->param_count; i++) {
            const struct nau_param_info *param = &info->params[i];
            assert(param->name && *param->name);
            for (uint32_t j = 0; j < i; j++)
                assert(strcmp(param->name, info->params[j].name));
            assert(isfinite(param->min) && isfinite(param->max) && param->min < param->max);
            assert(param->default_value >= param->min && param->default_value <= param->max);
            if (param->scale == NAU_PARAM_LOG)
                assert(param->min > 0);
            if (param->scale == NAU_PARAM_STEPPED)
                assert(param->min == truncf(param->min) && param->max == truncf(param->max) &&
                       param->default_value == truncf(param->default_value));
        }
    }
    assert(count >= 1 && !strcmp(effect_info(NAU_EFFECT_PASSTHROUGH)->name, "passthrough"));
    assert(!effect_info(NAU_EFFECT_PASSTHROUGH)->param_count);
    assert(!effect_info(count) && !effect_info(count + 1) && !effect_info(UINT32_MAX));
}

/* A ramp moves along the scale of its parameter */
static void test_ramp_param(void)
{
    const struct nau_param_info frequency = {"frequency", 10, 20000, 1000, NAU_PARAM_LOG};
    const struct nau_param_info level = {"level", 0, 10, 1, NAU_PARAM_LINEAR};
    const struct nau_param_info choice = {"choice", 0, 3, 0, NAU_PARAM_STEPPED};
    /* A frequency by octaves, whatever the dB shape tells */
    close_to(nau_ramp_param(&frequency, NAU_RAMP_LINEAR, 0, 250, 4000, 0.5f) / 1000, 1);
    close_to(nau_ramp_param(&frequency, NAU_RAMP_LINEAR, 0, 250, 4000, 0.25f) / 500, 1);
    assert(nau_ramp_param(&frequency, NAU_RAMP_DB, 0, 250, 4000, 0.25f) ==
           nau_ramp_param(&frequency, NAU_RAMP_LINEAR, 0, 250, 4000, 0.25f));
    close_to(log2f(nau_ramp_param(&frequency, NAU_RAMP_S_CURVE, 0, 250, 4000, 0.25f) / 250), 4 * 0.15625f);
    /* A level linearly, a choice by steps */
    close_to(nau_ramp_param(&level, NAU_RAMP_LINEAR, 0, 2, 6, 0.5f), 4);
    assert(nau_ramp_param(&choice, NAU_RAMP_LINEAR, 0, 0, 1, 0.4f) == 0);
    assert(nau_ramp_param(&choice, NAU_RAMP_LINEAR, 0, 0, 1, 0.6f) == 1);
    /* The ends exactly */
    const struct nau_param_info *params[] = {&frequency, &level, &choice};
    for (int shape = NAU_RAMP_LINEAR; shape <= NAU_RAMP_S_CURVE; shape++) {
        for (size_t i = 0; i < sizeof(params) / sizeof(*params); i++) {
            const struct nau_param_info *param = params[i];
            assert(nau_ramp_param(param, (enum nau_ramp_shape)shape, 0.5f, param->min, param->max, -1) == param->min);
            assert(nau_ramp_param(param, (enum nau_ramp_shape)shape, 0.5f, param->min, param->max, 1) == param->max);
        }
    }
    assert(nau_ramp_param(NULL, NAU_RAMP_LINEAR, 0, 1, 2, 0.5f) == 1);
}

/* Effects are created by their type, and only take the parameters they have */
static void test_effect_validation(void)
{
    struct nau_engine *e = NULL;
    const struct nau_engine_config c = {RATE, 2, 16};
    e = nau_engine_create();
    assert(e);
    assert(nau_engine_init(e, &c) == NAU_OK);
    uint32_t count = 0;
    while (effect_info(count))
        count++;
    struct nau_node *effect = NULL, *fader = NULL;
    effect = nau_node_create();
    assert(effect);
    assert(nau_effect_init(effect, e, (enum nau_effect_type)count, 0) == NAU_ERROR_INVALID && effect);
    assert(nau_effect_init(effect, e, (enum nau_effect_type)-1, 0) == NAU_ERROR_INVALID && effect);
    assert(nau_effect_init(effect, NULL, NAU_EFFECT_PASSTHROUGH, 0) == NAU_ERROR_INVALID && effect);
    assert(nau_effect_init(NULL, e, NAU_EFFECT_PASSTHROUGH, 0) == NAU_ERROR_INVALID);
    assert(nau_effect_init(effect, e, NAU_EFFECT_PASSTHROUGH, 0) == NAU_OK);
    fader = nau_node_create();
    assert(fader);
    assert(nau_fader_init(fader, e) == NAU_OK);
    /* The passthrough has no parameter */
    const struct nau_ramp ramp = {.value = 1};
    assert(nau_effect_set_param(effect, 0, 1) == NAU_ERROR_INVALID);
    assert(nau_effect_set_automation(effect, 0, &ramp, 1) == NAU_ERROR_INVALID);
    assert(nau_effect_set_automation(effect, 0, NULL, 0) == NAU_ERROR_INVALID);
    /* Nodes of other kinds are not effects, and an effect is not a fader */
    assert(nau_effect_set_param(fader, 0, 1) == NAU_ERROR_INVALID);
    assert(nau_effect_set_automation(fader, 0, &ramp, 1) == NAU_ERROR_INVALID);
    assert(nau_effect_set_automation(NULL, 0, &ramp, 1) == NAU_ERROR_INVALID);
    assert(nau_fader_set_ramps(effect, &ramp, 1) == NAU_ERROR_INVALID);
    nau_node_unrefp(&effect);
    nau_node_unrefp(&fader);
    nau_engine_freep(&e);
}

/* A sine of its frequency, delayed by `delay` frames, its opposite on the right */
struct delayed_tone {
    double frequency;
    int64_t delay;
};

static int delayed_tone_process(void *user, const struct nau_block *p)
{
    const struct delayed_tone *t = user;
    for (uint32_t i = 0; i < p->frames; i++) {
        const double frame = (double)(p->frame + i - t->delay);
        const float v = (float)(0.5 * sin(2. * NAU_PI_F64 * t->frequency * frame / RATE));
        p->output[2 * i] = v;
        p->output[2 * i + 1] = -v;
    }
    return NAU_OK;
}

/*
 * Whatever the effect, its output only depends on its inputs and on its
 * automation from its origin: not on how the renders are split, nor on where
 * its track is. Every parameter is automated, from its default value to the
 * farthest end of its range.
 */
static void test_effects_render(void)
{
    enum { N = 6000 };
    static float whole[2 * N], pieces[2 * N], moved[2 * N];
    for (uint32_t type = 0; effect_info(type); type++) {
        const struct nau_effect_info *info = effect_info(type);
        struct nau_engine *e = NULL;
        const struct nau_engine_config c = {RATE, 2, 64};
        e = nau_engine_create();
        assert(e);
        assert(nau_engine_init(e, &c) == NAU_OK);
        struct delayed_tone tone = {440, 0};
        const struct nau_processor tone_processor = {.user = &tone, .process = delayed_tone_process};
        struct nau_node *input = NULL, *effect = NULL;
        input = nau_node_create();
        assert(input);
        assert(nau_node_init(input, e, &tone_processor) == NAU_OK);
        effect = nau_node_create();
        assert(effect);
        assert(nau_effect_init(effect, e, (enum nau_effect_type)type, 0) == NAU_OK);
        for (uint32_t i = 0; i < info->param_count; i++) {
            const struct nau_param_info *param = &info->params[i];
            const int up = param->max - param->default_value > param->default_value - param->min;
            const struct nau_ramp ramp = {.frame = 1000 + 37 * i, .length = 2000, .value = up ? param->max : param->min};
            assert(nau_effect_set_automation(effect, i, &ramp, 1) == NAU_OK);
        }
        assert(nau_node_set_origin(effect, 500) == NAU_OK);
        static const uint32_t in[] = {0};
        const struct nau_route r[] = {{input, NULL}, {effect, in}};
        assert(nau_engine_commit(e, r, 2, 1) == NAU_OK);
        assert(nau_engine_render(e, 0, N, whole) == NAU_OK);
        for (uint32_t i = 0; i < 2 * N; i++)
            assert(isfinite(whole[i]));

        /* Cut anywhere, from the start again: the same */
        const uint32_t sizes[] = {1, 7, 13, 64, 100, 3, 255, 1000};
        for (uint32_t done = 0, i = 0; done < N; i++) {
            const uint32_t size = sizes[i % 8] < N - done ? sizes[i % 8] : N - done;
            assert(nau_engine_render(e, done, size, pieces + 2 * done) == NAU_OK);
            done += size;
        }
        assert(!memcmp(whole, pieces, sizeof(whole)));

        /* Moved with its inputs by any number of frames: moved as is */
        tone.delay = 7;
        assert(nau_node_set_origin(effect, 507) == NAU_OK);
        assert(nau_engine_commit(e, r, 2, 1) == NAU_OK);
        assert(nau_engine_render(e, 7, N, moved) == NAU_OK);
        assert(!memcmp(whole, moved, sizeof(whole)));

        nau_node_unrefp(&input);
        nau_node_unrefp(&effect);
        nau_engine_freep(&e);
    }
}

/* Every effect renders the last frames of the timeline, which the engine
 * accepts: first + frames can reach INT64_MAX */
static void test_effects_timeline_end(void)
{
    for (uint32_t type = 0; effect_info(type); type++) {
        struct nau_engine *e = nau_engine_create();
        const struct nau_engine_config c = {RATE, 2, 64};
        assert(e);
        assert(nau_engine_init(e, &c) == NAU_OK);
        struct nau_node *effect = nau_node_create();
        assert(effect);
        assert(nau_effect_init(effect, e, (enum nau_effect_type)type, 0) == NAU_OK);
        static const uint32_t in[] = {NAU_INPUT_NONE, NAU_INPUT_NONE};
        const struct nau_route r = {effect, in};
        assert(nau_engine_commit(e, &r, 1, 0) == NAU_OK);
        float out[2 * 32];
        assert(nau_engine_render(e, INT64_MAX - 5, 1, out) == NAU_OK);
        assert(nau_engine_render(e, INT64_MAX - 32, 32, out) == NAU_OK);
        for (uint32_t i = 0; i < 2 * 32; i++)
            assert(out[i] == 0.f);
        nau_node_unrefp(&effect);
        nau_engine_freep(&e);
    }
}

/* Without automation, and before its first ramp, a parameter has the value set */
static void test_lowpass_values(void)
{
    struct filtered f = {0};
    filtered_init(&f, NAU_LOWPASS_12DB, 2000);
    assert(nau_effect_set_param(f.lowpass, NAU_LOWPASS_CUTOFF, 1000) == NAU_OK);
    assert(nau_effect_set_param(f.lowpass, NAU_LOWPASS_RESONANCE, 4) == NAU_OK);
    filtered_commit(&f, NULL, 0);
    double expected = response(NAU_LOWPASS_12DB, 1000, 4, 2000);
    near(filtered_amplitude(&f, 0, 9600, 4800), expected, 0.01 * expected);
    /* The slope changed while playing */
    assert(nau_effect_set_param(f.lowpass, NAU_LOWPASS_SLOPE, NAU_LOWPASS_24DB) == NAU_OK);
    expected = response(NAU_LOWPASS_24DB, 1000, 4, 2000);
    near(filtered_amplitude(&f, 0, 9600, 4800), expected, 0.01 * expected);
    /* An automation of the cutoff starting at 1 s: up to 4 kHz over 100 s,
     * from the value set; the resonance as set */
    const int64_t length = 100 * RATE, middle = RATE + length / 2;
    const struct nau_ramp up = {.frame = RATE, .length = length, .value = 4000};
    assert(nau_effect_set_automation(f.lowpass, NAU_LOWPASS_CUTOFF, &up, 1) == NAU_OK);
    static const uint32_t in[] = {0};
    const struct nau_route r[] = {{f.tone, NULL}, {f.lowpass, in}};
    assert(nau_engine_commit(f.engine, r, 2, 1) == NAU_OK);
    near(filtered_amplitude(&f, 0, 9600, 4800), expected, 0.01 * expected);
    near(filtered_amplitude(&f, middle - 4800, middle, 4800), 4, 0.04);
    filtered_uninit(&f);
}

/* The low-pass filter describes its parameters: its cutoff open by default and
 * moving by octaves, its slope by steps */
static void test_lowpass_info(void)
{
    const struct nau_effect_info *lowpass = effect_info(NAU_EFFECT_LOWPASS);
    assert(!strcmp(lowpass->name, "lowpass") && lowpass->param_count == NAU_LOWPASS_RESONANCE_SCALE + 1);
    const struct nau_param_info *params = lowpass->params;
    assert(params[NAU_LOWPASS_CUTOFF].scale == NAU_PARAM_LOG);
    assert(params[NAU_LOWPASS_CUTOFF].default_value == NAU_LOWPASS_OPEN);
    assert(params[NAU_LOWPASS_RESONANCE].default_value == NAU_LOWPASS_FLAT);
    assert(params[NAU_LOWPASS_SLOPE].scale == NAU_PARAM_STEPPED);
    assert(params[NAU_LOWPASS_SLOPE].default_value == (float)NAU_LOWPASS_24DB);
    /* Ramps reach the ends of the ranges exactly */
    for (int shape = NAU_RAMP_LINEAR; shape <= NAU_RAMP_S_CURVE; shape++) {
        for (uint32_t i = 0; i < lowpass->param_count; i++) {
            const struct nau_param_info *param = &params[i];
            assert(nau_ramp_param(param, (enum nau_ramp_shape)shape, 0.5f, param->min, param->max, -1) == param->min);
            assert(nau_ramp_param(param, (enum nau_ramp_shape)shape, 0.5f, param->min, param->max, 1) == param->max);
        }
    }
}

int main(void)
{
    test_mix_edit();
    test_schedule();
    test_validation();
    test_ramp_shapes();
    test_db_ramps();
    test_fader();
    test_lowpass_response();
    test_lowpass_ramp();
    test_lowpass_underwater();
    test_lowpass_continuity();
    test_lowpass_live();
    test_lowpass_live_ramp();
    test_lowpass_validation();
    test_effect_info();
    test_ramp_param();
    test_effect_validation();
    test_effects_render();
    test_lowpass_values();
    test_lowpass_info();
    test_effects_timeline_end();
    return 0;
}
