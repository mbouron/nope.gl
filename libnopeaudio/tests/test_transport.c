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
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nopeaudio/nopeaudio.h"

/* A mono source whose samples are their frame numbers in the media */
struct numbers {
    int seeks, wraps;
    int64_t last_seek;
    int again;      /* reads left answering AGAIN */
    int not_ready;  /* readiness checks left answering not ready */
    float value;    /* nonzero: a constant rather than the frame numbers */
    int period;     /* nonzero: the constant changes sign every half period, in frames */
};

static int numbers_read(void *user, int64_t frame, uint32_t frames, float *out)
{
    struct numbers *n = user;
    for (uint32_t i = 0; i < frames; i++) {
        const bool negative = n->period && (frame + i) % n->period >= n->period / 2;
        out[i] = n->value != 0.f ? (negative ? -n->value : n->value) : (float)(frame + i);
    }
    if (n->again > 0) {
        n->again--;
        return NAU_AGAIN;
    }
    return NAU_OK;
}

static int numbers_ready(void *user, int64_t frame)
{
    struct numbers *n = user;
    if (n->not_ready > 0) {
        n->not_ready--;
        return 0;
    }
    return 1;
}

static void numbers_seek(void *user, int64_t frame, int wrap)
{
    struct numbers *n = user;
    n->seeks++;
    n->wraps += !!wrap;
    n->last_seek = frame;
}

struct setup {
    struct nau_engine *engine;
    struct nau_node *source;
    struct nau_transport *transport;
};

static struct setup setup(struct numbers *n, int64_t source_start, int64_t duration,
                          uint32_t declick, uint32_t ready_timeout)
{
    struct setup s = {0};
    const struct nau_engine_config c = {48000, 1, 16};
    s.engine = nau_engine_create();
    assert(s.engine);
    assert(nau_engine_init(s.engine, &c) == NAU_OK);
    const struct nau_source source = {
        .user = n, .read = numbers_read, .seek = numbers_seek, .ready = numbers_ready,
        .timeline_start = 0, .source_start = source_start, .duration = duration,
    };
    s.source = nau_node_create();
    assert(s.source);
    assert(nau_source_init(s.source, s.engine, &source) == NAU_OK);
    const struct nau_route r = {s.source, NULL};
    assert(nau_engine_commit(s.engine, &r, 1, 0) == NAU_OK);
    const struct nau_transport_config tc = {.declick_frames = declick, .ready_timeout_frames = ready_timeout};
    s.transport = nau_transport_create();
    assert(s.transport);
    assert(nau_transport_init(s.transport, s.engine, &tc) == NAU_OK);
    assert(nau_transport_set_duration(s.transport, duration) == NAU_OK);
    return s;
}

static void teardown(struct setup *s)
{
    nau_transport_freep(&s->transport);
    nau_node_unrefp(&s->source);
    nau_engine_freep(&s->engine);
}

static void close_to(float a, float b)
{
    if (fabsf(a - b) > 0.0001f) {
        fprintf(stderr, "%f != %f\n", a, b);
        assert(0);
    }
}

static void test_play_to_end(void)
{
    struct numbers n = {0};
    struct setup s = setup(&n, 1000, 100, 0, 0);
    float out[80];
    /* Not playing: silence, nothing read, the source positioned for its data to be there on play */
    assert(nau_transport_render(s.transport, 0, 8, out) == NAU_OK);
    for (int i = 0; i < 8; i++)
        close_to(out[i], 0);
    assert(n.seeks == 1 && n.last_seek == 1000 && n.wraps == 0);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 40, out) == NAU_OK);
    for (int i = 0; i < 40; i++)
        close_to(out[i], (float)(1000 + i));
    assert(n.seeks == 1); /* not positioned twice */
    assert(nau_transport_get_position(s.transport) == 40);
    /* The end: back to the start, stopped, the source positioned there */
    assert(nau_transport_render(s.transport, 0, 80, out) == NAU_OK);
    for (int i = 0; i < 60; i++)
        close_to(out[i], (float)(1040 + i));
    for (int i = 60; i < 80; i++)
        close_to(out[i], 0);
    assert(!nau_transport_is_playing(s.transport));
    assert(nau_transport_get_position(s.transport) == 0);
    assert(n.seeks == 2 && n.last_seek == 1000);
    teardown(&s);
}

static void test_seek(void)
{
    struct numbers n = {0};
    struct setup s = setup(&n, 1000, 100, 0, 0);
    float out[16];
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 10, out) == NAU_OK);
    assert(nau_transport_seek(s.transport, 50) == NAU_OK);
    assert(nau_transport_get_position(s.transport) == 50); /* before the renderer applies it */
    assert(nau_transport_render(s.transport, 0, 10, out) == NAU_OK);
    for (int i = 0; i < 10; i++)
        close_to(out[i], (float)(1050 + i));
    assert(n.last_seek == 1050 && n.wraps == 0);
    assert(nau_transport_get_position(s.transport) == 60);
    /* Past the duration: cut to it */
    assert(nau_transport_seek(s.transport, 1000) == NAU_OK);
    assert(nau_transport_get_position(s.transport) == 100);
    /* Paused, a seek positions the source at once, for its data to be there on play */
    nau_transport_pause(s.transport);
    assert(nau_transport_seek(s.transport, 30) == NAU_OK);
    const int seeks = n.seeks;
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    for (int i = 0; i < 16; i++)
        close_to(out[i], 0);
    assert(n.seeks == seeks + 1 && n.last_seek == 1030);
    assert(nau_transport_get_position(s.transport) == 30);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 4, out) == NAU_OK);
    for (int i = 0; i < 4; i++)
        close_to(out[i], (float)(1030 + i));
    assert(n.seeks == seeks + 1); /* no second seek */
    /* Nothing rendering: the control thread applies the seek itself */
    assert(nau_transport_seek(s.transport, 70) == NAU_OK);
    nau_transport_sync(s.transport);
    assert(n.last_seek == 1070 && nau_transport_get_position(s.transport) == 70);
    assert(nau_transport_render(s.transport, 0, 4, out) == NAU_OK);
    for (int i = 0; i < 4; i++)
        close_to(out[i], (float)(1070 + i));
    assert(nau_transport_seek(s.transport, -1) == NAU_ERROR_INVALID);
    assert(nau_transport_seek(s.transport, INT64_C(1) << 48) == NAU_ERROR_INVALID);
    teardown(&s);
}

static void test_loop(void)
{
    struct numbers n = {0};
    struct setup s = setup(&n, 1000, 100, 0, 0);
    float out[80];
    nau_transport_set_looping(s.transport, 1);
    assert(nau_transport_set_loop_range(s.transport, 20, 60) == NAU_OK);
    nau_transport_play(s.transport);
    /* Before the range, playback goes on to its end, then round it */
    assert(nau_transport_render(s.transport, 0, 70, out) == NAU_OK);
    for (int i = 0; i < 60; i++)
        close_to(out[i], (float)(1000 + i));
    for (int i = 60; i < 70; i++)
        close_to(out[i], (float)(1020 + i - 60));
    assert(n.wraps == 1 && n.last_seek == 1020);
    assert(nau_transport_get_position(s.transport) == 30);
    /* Holding still at the end: silence, the position at the end, the source going round meanwhile */
    assert(nau_transport_set_loop_hold(s.transport, 10) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 35, out) == NAU_OK);
    for (int i = 0; i < 30; i++)
        close_to(out[i], (float)(1030 + i));
    for (int i = 30; i < 35; i++)
        close_to(out[i], 0);
    assert(nau_transport_get_position(s.transport) == 60);
    assert(n.wraps == 2 && n.last_seek == 1020);
    assert(nau_transport_render(s.transport, 0, 10, out) == NAU_OK);
    for (int i = 0; i < 5; i++)
        close_to(out[i], 0);
    for (int i = 5; i < 10; i++)
        close_to(out[i], (float)(1020 + i - 5));
    assert(n.wraps == 2); /* went round once */
    /* Not looping: the whole duration, to its end */
    nau_transport_set_looping(s.transport, 0);
    assert(nau_transport_render(s.transport, 0, 80, out) == NAU_OK);
    for (int i = 0; i < 75; i++)
        close_to(out[i], (float)(1025 + i));
    assert(!nau_transport_is_playing(s.transport));
    /* A loop range past the duration loops over the whole duration */
    nau_transport_set_looping(s.transport, 1);
    assert(nau_transport_set_loop_hold(s.transport, 0) == NAU_OK);
    assert(nau_transport_set_loop_range(s.transport, 200, 300) == NAU_OK);
    assert(nau_transport_seek(s.transport, 90) == NAU_OK);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 20, out) == NAU_OK);
    for (int i = 0; i < 10; i++)
        close_to(out[i], (float)(1090 + i));
    for (int i = 10; i < 20; i++)
        close_to(out[i], (float)(1000 + i - 10));
    teardown(&s);
}

/*
 * The end of the range moved before the playhead: the output fades out past
 * it, then holds at the end of the range, not where the fade ended
 */
static void test_hold_after_moved_end(void)
{
    struct numbers n = {.value = 1.f};
    struct setup s = setup(&n, 0, 100, 8, 0);
    float out[80];
    nau_transport_set_looping(s.transport, 1);
    assert(nau_transport_set_loop_hold(s.transport, 20) == NAU_OK);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 70, out) == NAU_OK);
    assert(nau_transport_set_duration(s.transport, 40) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 8, out) == NAU_OK);
    for (int i = 0; i < 8; i++)
        close_to(out[i], (float)(7 - i) / 8);
    const int64_t time = 1000000000;
    assert(nau_transport_render(s.transport, time, 10, out) == NAU_OK);
    for (int i = 0; i < 10; i++)
        close_to(out[i], 0);
    assert(nau_transport_get_position(s.transport) == 40);
    int64_t heard;
    assert(nau_transport_get_position_at(s.transport, time + 5 * 1000000000 / 48000, &heard) == NAU_OK);
    assert(heard == 40);
    /* The rest of the hold, then the start of the range */
    assert(nau_transport_render(s.transport, 0, 18, out) == NAU_OK);
    for (int i = 0; i < 10; i++)
        close_to(out[i], 0);
    for (int i = 10; i < 18; i++)
        close_to(out[i], (float)(i - 10 + 1) / 8);
    assert(nau_transport_get_position(s.transport) == 8);
    teardown(&s);
}

/* Gain k/8 after k frames of an 8-frame ramp from silence */
static void check_ramp_in(const float *out, int count)
{
    for (int i = 0; i < count; i++)
        close_to(out[i], i < 8 ? (float)(i + 1) / 8 : 1);
}

static void check_ramp_out(const float *out)
{
    for (int i = 0; i < 8; i++)
        close_to(out[i], (float)(7 - i) / 8);
}

static void test_declick(void)
{
    struct numbers n = {.value = 1};
    struct setup s = setup(&n, 0, 100, 8, 1000);
    float out[64];
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    check_ramp_in(out, 16);
    /* A seek fades out, jumps, and fades back in */
    assert(nau_transport_seek(s.transport, 50) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 24, out) == NAU_OK);
    check_ramp_out(out);
    check_ramp_in(out + 8, 16);
    assert(n.last_seek == 50);
    assert(nau_transport_get_position(s.transport) == 66); /* 8 frames faded out before the jump */
    /* Pausing fades out, playing fades in */
    nau_transport_pause(s.transport);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    check_ramp_out(out);
    for (int i = 8; i < 16; i++)
        close_to(out[i], 0);
    assert(nau_transport_get_position(s.transport) == 74);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 8, out) == NAU_OK);
    check_ramp_in(out, 8);
    /* A loop fades out into its end, and in from its start */
    nau_transport_set_looping(s.transport, 1);
    assert(nau_transport_set_loop_range(s.transport, 0, 96) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 30, out) == NAU_OK); /* 82..95, then 0..15 */
    for (int i = 0; i < 6; i++)
        close_to(out[i], 1);
    check_ramp_out(out + 6);
    check_ramp_in(out + 14, 16);
    /* Edited as it pauses: the fade out plays the graph it started on, the
     * edit adopted once silent, and playing fades the new graph in */
    struct numbers m = {.value = 2};
    const struct nau_source source = {
        .user = &m, .read = numbers_read, .seek = numbers_seek, .timeline_start = 0, .duration = 100,
    };
    struct nau_node *other = NULL;
    other = nau_node_create();
    assert(other);
    assert(nau_source_init(other, s.engine, &source) == NAU_OK);
    const struct nau_route to_other = {other, NULL};
    nau_transport_pause(s.transport);
    assert(nau_engine_commit(s.engine, &to_other, 1, 0) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    check_ramp_out(out);
    for (int i = 8; i < 16; i++)
        close_to(out[i], 0);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 8, out) == NAU_OK);
    for (int i = 0; i < 8; i++)
        close_to(out[i], 2.f * (float)(i + 1) / 8);
    nau_node_unrefp(&other);
    teardown(&s);
}

static void test_waiting(void)
{
    /* After a jump, silent until the sources have their data */
    struct numbers n = {.value = 1, .again = 3};
    struct setup s = setup(&n, 0, 1000, 4, 1000);
    float out[32];
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 12, out) == NAU_AGAIN);
    for (int i = 0; i < 12; i++)
        close_to(out[i], 0);
    assert(nau_transport_render(s.transport, 0, 8, out) == NAU_OK);
    for (int i = 0; i < 8; i++)
        close_to(out[i], i < 4 ? (float)(i + 1) / 4 : 1);
    assert(nau_transport_get_position(s.transport) == 20); /* the position went on meanwhile */
    /* Playing, a source missing its data does not silence the others */
    n.again = 1;
    assert(nau_transport_render(s.transport, 0, 4, out) == NAU_AGAIN);
    for (int i = 0; i < 4; i++)
        close_to(out[i], 1);
    teardown(&s);
    /* A source never ready: the output comes back after the timeout */
    struct numbers late = {.value = 1, .again = 1000};
    s = setup(&late, 0, 1000, 4, 12);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 20, out) == NAU_AGAIN);
    for (int i = 0; i < 12; i++)
        close_to(out[i], 0);
    for (int i = 12; i < 20; i++)
        close_to(out[i], i < 16 ? (float)(i - 11) / 4 : 1);
    teardown(&s);
}

static void test_hold(void)
{
    /* After a jump, the position holds while a source is not ready, for
     * its audio to be heard from the frame jumped to */
    struct numbers n = {.not_ready = 3};
    struct setup s = setup(&n, 1000, 1000, 4, 1000);
    float out[32];
    nau_transport_play(s.transport);
    const int64_t time = 1000000000;
    assert(nau_transport_render(s.transport, time, 20, out) == NAU_AGAIN);
    for (int i = 0; i < 12; i++)
        close_to(out[i], 0);
    for (int i = 12; i < 20; i++)
        close_to(out[i], (float)(1000 + i - 12) * (i < 16 ? (float)(i - 11) / 4 : 1));
    assert(nau_transport_get_position(s.transport) == 8);
    assert(n.seeks == 1 && n.last_seek == 1000);
    /* The clock stood still meanwhile, then went on from there */
    int64_t position;
    assert(nau_transport_get_position_at(s.transport, time + 100000, &position) == NAU_OK && position == 0);
    assert(nau_transport_get_position_at(s.transport, time + 350000, &position) == NAU_OK && position == 4);
    /* A seek while playing waits anew, from the frame sought */
    n.not_ready = 2;
    assert(nau_transport_seek(s.transport, 500) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 20, out) == NAU_AGAIN);
    for (int i = 0; i < 4; i++)
        close_to(out[i], (float)(1008 + i) * (float)(3 - i) / 4);
    for (int i = 4; i < 12; i++)
        close_to(out[i], 0);
    for (int i = 12; i < 20; i++)
        close_to(out[i], (float)(1500 + i - 12) * (i < 16 ? (float)(i - 11) / 4 : 1));
    assert(nau_transport_get_position(s.transport) == 508);
    assert(n.seeks == 2 && n.last_seek == 1500);
    teardown(&s);
    /* A source never ready: played after the timeout, from the frame jumped to */
    struct numbers late = {.not_ready = 1000};
    s = setup(&late, 1000, 1000, 4, 12);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 20, out) == NAU_AGAIN);
    for (int i = 0; i < 12; i++)
        close_to(out[i], 0);
    for (int i = 12; i < 20; i++)
        close_to(out[i], (float)(1000 + i - 12) * (i < 16 ? (float)(i - 11) / 4 : 1));
    assert(nau_transport_get_position(s.transport) == 8);
    teardown(&s);
}

static void test_clock(void)
{
    struct numbers n = {.value = 1};
    struct setup s = setup(&n, 0, 48000 * 10, 0, 0);
    struct nau_clock clock;
    assert(nau_transport_get_clock(s.transport, &clock) == NAU_AGAIN);
    float out[480];
    nau_transport_play(s.transport);
    /* Renders of 10 ms, called with up to 2 ms of jitter */
    const int64_t start = INT64_C(1000000000);
    int64_t max_error = 0;
    for (int i = 0; i < 200; i++) {
        const int64_t jitter = (i * 7919 % 41 - 20) * 100000;
        const int64_t time = start + i * INT64_C(10000000) + jitter;
        assert(nau_transport_render(s.transport, time, 480, out) == NAU_OK);
        assert(nau_transport_get_clock(s.transport, &clock) == NAU_OK);
        assert(clock.playing && clock.sample_rate == 48000 && clock.position == 480 * i);
        /* Once settled, the clock follows the true times rather than the jittered ones */
        const int64_t error = clock.time - (start + i * INT64_C(10000000));
        if (i >= 100 && llabs(error) > max_error)
            max_error = llabs(error);
    }
    assert(max_error < 1000000);
    assert(nau_clock_position_at(&clock, clock.time + 5000000) == clock.position + 240);
    assert(nau_clock_position_at(&clock, clock.time - 5000000) == clock.position - 240);
    nau_transport_pause(s.transport);
    assert(nau_transport_render(s.transport, start + INT64_C(2000000000), 480, out) == NAU_OK);
    assert(nau_transport_get_clock(s.transport, &clock) == NAU_OK);
    assert(!clock.playing && nau_clock_position_at(&clock, clock.time + 5000000) == clock.position);
    teardown(&s);
}

/* Paused or holding still, the renders adopt the commits all the same: the
 * control thread commits again without waiting for the playback to resume */
static void test_commit_while_silent(void)
{
    struct numbers n = {0}, m = {.value = 1};
    struct setup s = setup(&n, 1000, 100, 0, 0);
    const struct nau_source source = {
        .user = &m, .read = numbers_read, .seek = numbers_seek, .timeline_start = 0, .duration = 100,
    };
    struct nau_node *other = NULL;
    other = nau_node_create();
    assert(other);
    assert(nau_source_init(other, s.engine, &source) == NAU_OK);
    const struct nau_route to_other = {other, NULL}, to_source = {s.source, NULL};
    float out[16];
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 10, out) == NAU_OK);
    nau_transport_pause(s.transport);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    assert(nau_engine_commit(s.engine, &to_other, 1, 0) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    for (int i = 0; i < 16; i++)
        close_to(out[i], 0);
    assert(m.seeks == 1 && m.last_seek == 10); /* the new node where the playback resumes */
    assert(nau_engine_commit(s.engine, &to_source, 1, 0) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 4, out) == NAU_OK);
    for (int i = 0; i < 4; i++)
        close_to(out[i], (float)(1010 + i));
    assert(n.seeks == 1); /* went on from where it was */
    /* Holding still at the end of a loop */
    nau_transport_set_looping(s.transport, 1);
    assert(nau_transport_set_loop_range(s.transport, 0, 20) == NAU_OK);
    assert(nau_transport_set_loop_hold(s.transport, 1000) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    for (int i = 6; i < 16; i++)
        close_to(out[i], 0);
    assert(nau_engine_commit(s.engine, &to_other, 1, 0) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    assert(nau_engine_commit(s.engine, &to_source, 1, 0) == NAU_OK);
    nau_node_unrefp(&other);
    teardown(&s);
}

static void check_position_at(struct nau_transport *transport, int64_t time, int64_t expected)
{
    int64_t position = -1;
    assert(nau_transport_get_position_at(transport, time, &position) == NAU_OK);
    assert(position == expected);
}

static void test_clock_history(void)
{
    struct numbers n = {.value = 1};
    struct setup s = setup(&n, 0, 4800, 0, 0);
    float output[960];
    int64_t position = -1;
    assert(nau_transport_get_position_at(s.transport, 1000000000, &position) == NAU_AGAIN);
    assert(position == -1);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 1000000000, 480, output) == NAU_OK);
    assert(nau_transport_render(s.transport, 1010000000, 480, output) == NAU_OK);
    check_position_at(s.transport, 1005000000, 240);
    assert(nau_transport_seek(s.transport, 2400) == NAU_OK);
    assert(nau_transport_render(s.transport, 1020000000, 480, output) == NAU_OK);
    check_position_at(s.transport, 1015000000, 720);
    check_position_at(s.transport, 1025000000, 2640);
    nau_transport_pause(s.transport);
    assert(nau_transport_render(s.transport, 1030000000, 480, output) == NAU_OK);
    check_position_at(s.transport, 1025000000, 2640);
    check_position_at(s.transport, 1035000000, 2880);
    assert(nau_transport_render(s.transport, 1040000000, 0, NULL) == NAU_OK);
    check_position_at(s.transport, 1035000000, 2880);
    teardown(&s);

    s = setup(&n, 0, 960, 0, 0);
    nau_transport_set_looping(s.transport, 1);
    assert(nau_transport_set_loop_range(s.transport, 240, 720) == NAU_OK);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 1000000000, 480, output) == NAU_OK);
    assert(nau_transport_render(s.transport, 1010000000, 480, output) == NAU_OK);
    check_position_at(s.transport, 1012500000, 600);
    check_position_at(s.transport, 1017500000, 360);
    assert(nau_transport_set_loop_hold(s.transport, 240) == NAU_OK);
    assert(nau_transport_render(s.transport, 1020000000, 960, output) == NAU_OK);
    check_position_at(s.transport, 1022500000, 600);
    check_position_at(s.transport, 1027500000, 720);
    check_position_at(s.transport, 1032500000, 360);
    for (int i = 0; i < 300; i++)
        assert(nau_transport_render(s.transport, 1040000000 + (int64_t)i * 10000000, 480, output) == NAU_OK);
    assert(nau_transport_get_position_at(s.transport, 1000000000, &position) == NAU_AGAIN);
    assert(position == -1);
    teardown(&s);

    s = setup(&n, 0, 720, 0, 0);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 1000000000, 960, output) == NAU_OK);
    check_position_at(s.transport, 1010000000, 480);
    check_position_at(s.transport, 1017500000, 0);
    teardown(&s);

    struct nau_clock clock = {.position = INT64_MAX - 1, .time = INT64_MIN, .sample_rate = 768000, .playing = 1};
    assert(nau_clock_position_at(&clock, INT64_MAX) == INT64_MAX);
    clock.position = 0;
    clock.time = INT64_MAX;
    assert(nau_clock_position_at(&clock, INT64_MIN) == 0);
}

static void test_short_declick(void)
{
    const int64_t lengths[] = {1, 2, 120, 240};
    for (uint32_t i = 0; i < sizeof(lengths) / sizeof(*lengths); i++) {
        struct numbers n = {.value = 1};
        struct setup s = setup(&n, 0, lengths[i], 240, 0);
        nau_transport_set_looping(s.transport, 1);
        nau_transport_play(s.transport);
        float output[480];
        assert(nau_transport_render(s.transport, 0, 480, output) == NAU_OK);
        uint32_t audible = 0;
        for (uint32_t j = 0; j < 480; j++) {
            assert(output[j] >= 0.f && output[j] <= 1.f);
            audible += output[j] > 0.f;
        }
        assert(audible > 0);
        teardown(&s);
    }
}

/* The longest declick still ramps, over as many frames as it tells */
static void test_long_declick(void)
{
    struct numbers n = {.value = 1};
    struct setup s = setup(&n, 0, INT64_C(1) << 40, UINT32_MAX, 0);
    nau_transport_play(s.transport);
    float output[16];
    assert(nau_transport_render(s.transport, 0, 16, output) == NAU_OK);
    for (uint32_t i = 0; i < 16; i++)
        assert(output[i] >= 0.f && output[i] < 1e-6f);
    teardown(&s);
}

/* Put a node after the source of a setup, which did not render yet */
static void append(struct setup *s, struct nau_node *node)
{
    nau_transport_sync(s->transport);
    const uint32_t input = 0;
    const struct nau_route routes[] = {{s->source, NULL}, {node, &input}};
    assert(nau_engine_commit(s->engine, routes, 2, 1) == NAU_OK);
}

/* The fade in of the transport, over 4 frames, of a constant */
static void fades_in(const float *out, int frames, float value)
{
    for (int i = 0; i < frames; i++)
        close_to(out[i], value * (i < 4 ? (float)(i + 1) / 4 : 1));
}

static void silent_for(const float *out, int frames)
{
    for (int i = 0; i < frames; i++)
        assert(out[i] == 0.f);
}

/*
 * A gain is ramped to while it is heard. Set while the transport does not
 * render the graph, it applies at once when it does again: the output starts
 * from silence, and the previous gain must not be heard on its way out.
 */
static void test_gain_while_silent(void)
{
    struct numbers n = {.value = 1};
    struct setup s = setup(&n, 0, 1000, 4, 100);
    struct nau_node *gain = NULL;
    gain = nau_node_create();
    assert(gain);
    assert(nau_gain_init(gain, s.engine, 1.f, 8) == NAU_OK);
    append(&s, gain);
    float out[64];
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    fades_in(out, 16, 1.f);
    /* Playing: ramped over 8 frames */
    assert(nau_gain_set(gain, 0.5f) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    for (int i = 0; i < 16; i++)
        close_to(out[i], i < 8 ? 1.f - (float)(i + 1) / 16 : 0.5f);

    /* Paused: at once, to silence, then back */
    nau_transport_pause(s.transport);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    silent_for(out + 4, 12);
    assert(nau_gain_set(gain, 0.f) == NAU_OK);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 32, out) == NAU_OK);
    silent_for(out, 32);
    nau_transport_pause(s.transport);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    assert(nau_gain_set(gain, 1.f) == NAU_OK);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    fades_in(out, 16, 1.f);

    /* Waiting for the source after a seek */
    n.not_ready = 1000;
    assert(nau_transport_seek(s.transport, 100) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_AGAIN);
    silent_for(out + 4, 12);
    assert(nau_gain_set(gain, 0.f) == NAU_OK);
    n.not_ready = 0;
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    silent_for(out, 16);
    assert(nau_transport_get_position(s.transport) == 116);

    /* Holding at the end of a loop pass */
    assert(nau_transport_set_loop_range(s.transport, 100, 132) == NAU_OK);
    assert(nau_transport_set_loop_hold(s.transport, 16) == NAU_OK);
    nau_transport_set_looping(s.transport, 1);
    assert(nau_transport_render(s.transport, 0, 24, out) == NAU_OK);
    assert(nau_transport_get_position(s.transport) == 132);
    assert(nau_gain_set(gain, 1.f) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 24, out) == NAU_OK);
    silent_for(out, 8);
    fades_in(out + 8, 16, 1.f);

    /* Stopped at the end of the project */
    nau_transport_set_looping(s.transport, 0);
    assert(nau_transport_seek(s.transport, 990) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 64, out) == NAU_OK);
    assert(!nau_transport_is_playing(s.transport) && nau_transport_get_position(s.transport) == 0);
    assert(nau_gain_set(gain, 0.25f) == NAU_OK);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 16, out) == NAU_OK);
    fades_in(out, 16, 0.25f);
    nau_node_unrefp(&gain);
    teardown(&s);
}

static float peak_of(const float *out, int frames)
{
    float peak = 0.f;
    for (int i = 0; i < frames; i++)
        peak = fmaxf(peak, fabsf(out[i]));
    return peak;
}

/* The values set of an effect, ramped over a long time while it is heard,
 * apply at once as well */
static void test_lowpass_while_silent(void)
{
    /* A 6 kHz square under a filter closed at 100 Hz */
    struct numbers n = {.value = 1, .period = 8};
    struct setup s = setup(&n, 0, 100000, 4, 100);
    struct nau_node *lowpass = NULL;
    lowpass = nau_node_create();
    assert(lowpass);
    assert(nau_effect_init(lowpass, s.engine, NAU_EFFECT_LOWPASS, 4800) == NAU_OK);
    const struct nau_ramp closed = {.value = 100.f};
    assert(nau_effect_set_automation(lowpass, NAU_LOWPASS_CUTOFF, &closed, 1) == NAU_OK);
    append(&s, lowpass);
    static float out[480];
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 480, out) == NAU_OK);
    assert(peak_of(out + 240, 240) < 0.05f);
    /* Opened while playing: still closing in a tenth of its ramp later */
    assert(nau_effect_set_param(lowpass, NAU_LOWPASS_AMOUNT, 0.f) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 480, out) == NAU_OK);
    assert(peak_of(out, 480) < 0.5f);
    assert(nau_effect_set_param(lowpass, NAU_LOWPASS_AMOUNT, 1.f) == NAU_OK);
    assert(nau_transport_render(s.transport, 0, 480, out) == NAU_OK);
    /* Opened while paused: open from the first frames */
    nau_transport_pause(s.transport);
    assert(nau_transport_render(s.transport, 0, 480, out) == NAU_OK);
    assert(nau_effect_set_param(lowpass, NAU_LOWPASS_AMOUNT, 0.f) == NAU_OK);
    nau_transport_play(s.transport);
    assert(nau_transport_render(s.transport, 0, 48, out) == NAU_OK);
    assert(peak_of(out + 8, 40) > 0.8f);
    nau_node_unrefp(&lowpass);
    teardown(&s);
}

int main(void)
{
    test_play_to_end();
    test_seek();
    test_loop();
    test_hold_after_moved_end();
    test_declick();
    test_short_declick();
    test_long_declick();
    test_waiting();
    test_hold();
    test_clock();
    test_clock_history();
    test_commit_while_silent();
    test_gain_while_silent();
    test_lowpass_while_silent();
    return 0;
}
