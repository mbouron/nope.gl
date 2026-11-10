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

int main(void)
{
    test_mix_edit();
    test_schedule();
    test_validation();
    test_ramp_shapes();
    test_db_ramps();
    test_fader();
    test_effect_info();
    test_ramp_param();
    test_effect_validation();
    test_effects_render();
    test_effects_timeline_end();
    return 0;
}
