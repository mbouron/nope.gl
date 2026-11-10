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
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nopeaudio/nopeaudio.h"
#include "../src/utils/time.h"

static const char *fixture, *fixture_44100, *fixture_index, *fixture_mono, *fixture_quiet_wav, *fixture_quiet, *fixture_aac;
static const char *fixture_gap;

static void close_to(float a, float b)
{
    if (fabsf(a - b) > 0.00001f) {
        fprintf(stderr, "%f != %f\n", a, b);
        assert(0);
    }
}

/* The index of a frame of the index fixture, -1 for silence */
static int64_t index_of(const float *frame)
{
    const long left = lroundf(frame[0] * 32768.f), right = lroundf(frame[1] * 32768.f);
    if (!left && !right)
        return -1;
    return (left - 1) | (right - 1) << 14;
}

struct media {
    struct nau_engine *engine;
    struct nau_media *media;
    struct nau_node *source;
};

static struct media open_media(const char *filename, int blocking, int normalize,
                               int64_t timeline_start, int64_t source_start, int64_t duration)
{
    struct media m = {0};
    const struct nau_engine_config c = {48000, 2, 64};
    m.engine = nau_engine_create();
    assert(m.engine);
    assert(nau_engine_init(m.engine, &c) == NAU_OK);
    const struct nau_media_params params = {.start = source_start, .blocking = blocking, .normalize = normalize};
    m.media = nau_media_create();
    assert(m.media);
    assert(nau_media_init(m.media, m.engine, filename, &params) == NAU_OK);
    m.source = nau_node_create();
    assert(m.source);
    assert(nau_media_init_source(m.media, m.source, timeline_start, source_start, duration) == NAU_OK);
    const struct nau_route r = {m.source, NULL};
    assert(nau_engine_commit(m.engine, &r, 1, 0) == NAU_OK);
    return m;
}

static struct nau_media_stats stats_of(const struct media *m)
{
    struct nau_media_stats stats;
    nau_media_get_stats(m->media, &stats);
    return stats;
}

static void close_media(struct media *m)
{
    assert(stats_of(m).errors == 0);
    nau_node_unrefp(&m->source);
    nau_engine_freep(&m->engine);
    nau_media_unrefp(&m->media);
}

/* The fixture's frame `frame` of the media */
static void check_fixture(const float *out, int64_t frame, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const float sample = frame + i < 4096 ? (float)(1000 + frame + i) / 32768.f : 0;
        close_to(out[2 * i], sample);
        close_to(out[2 * i + 1], -sample);
    }
}

static void test_read_and_seek(void)
{
    /* Offline: every read waits for its data */
    struct media m = open_media(fixture, 1, 0, 17, 101, 10000);
    assert(nau_media_get_length(m.media) == 4096);
    float out[2 * 64];
    for (int64_t frame = 0; frame < 4224; frame += 64) {
        assert(nau_engine_render(m.engine, frame, 64, out) == NAU_OK);
        for (uint32_t i = 0; i < 64; i++) {
            if (frame + i < 17)
                close_to(out[2 * i], 0);
            else
                check_fixture(out + 2 * i, 101 + frame + i - 17, 1);
        }
    }
    /* Past the end of the media: silence, without asking the decoder again */
    assert(stats_of(&m).requests == 0);
    for (int64_t frame = 4224; frame < 6400; frame += 64)
        assert(nau_engine_render(m.engine, frame, 64, out) == NAU_OK);
    for (uint32_t i = 0; i < 128; i++)
        close_to(out[i], 0);
    assert(stats_of(&m).requests == 0);
    /* A jump back: a seek inside a chunk of the decoder, sample-exact */
    assert(nau_engine_render(m.engine, 17, 17, out) == NAU_OK);
    check_fixture(out, 101, 17);
    assert(stats_of(&m).requests == 1);
    /* To the very start of the media */
    assert(nau_source_set_range(m.source, 0, 0, 10000) == NAU_OK);
    const struct nau_route r = {m.source, NULL};
    assert(nau_engine_commit(m.engine, &r, 1, 0) == NAU_OK);
    for (int64_t frame = 0; frame < 4608; frame += 64) {
        assert(nau_engine_render(m.engine, frame, 64, out) == NAU_OK);
        check_fixture(out, frame, 64);
    }
    close_media(&m);
}

/* A long timestamp gap is silence.
 * Blocking reads must advance through it instead of repeatedly seeking. */
static void test_gap(void)
{
    struct media m = open_media(fixture_gap, 1, 0, 0, 0, 30000);
    assert(nau_media_get_length(m.media) == 24960);
    float out[2 * 64];
    for (int64_t frame = 0; frame < 25600; frame += 64) {
        assert(nau_engine_render(m.engine, frame, 64, out) == NAU_OK);
        for (uint32_t i = 0; i < 64; i++) {
            const int64_t position = frame + i;
            const float sample = position < 480 || (position >= 24480 && position < 24960) ? 0.25f : 0.f;
            close_to(out[2 * i], sample);
            close_to(out[2 * i + 1], -sample);
        }
    }
    const struct nau_media_stats stats = stats_of(&m);
    assert(stats.requests == 0 && stats.silent_frames == 0 && stats.skipped_frames == 0);
    close_media(&m);
}

/*
 * A loop of the project the media goes round ahead, read without a single
 * seek. Set late, once nope.media has read the media to its end, as it reads
 * a short one at once, it goes round it all the same from the start of the
 * loop, the first time too.
 */
static void test_loop(int late)
{
    struct media m = open_media(fixture_index, 1, 0, 0, 0, 48000);
    const struct nau_transport_config tc = {0};
    struct nau_transport *t = nau_transport_create();
    assert(t);
    assert(nau_transport_init(t, m.engine, &tc) == NAU_OK);
    assert(nau_transport_set_duration(t, 48000) == NAU_OK);
    nau_transport_set_looping(t, 1);
    assert(nau_transport_set_loop_range(t, 12000, 24000) == NAU_OK);
    if (late)
        nau_sleep_ms(200);
    assert(nau_media_set_loop(m.media, 12000, 24000) == NAU_OK);
    nau_transport_play(t);
    float out[2 * 256];
    int64_t expected = 0;
    for (int i = 0; i < 300; i++) {
        assert(nau_transport_render(t, 0, 256, out) == NAU_OK);
        for (uint32_t f = 0; f < 256; f++) {
            assert(index_of(out + 2 * f) == expected);
            if (++expected == 24000)
                expected = 12000;
        }
    }
    assert(stats_of(&m).requests == 0 && stats_of(&m).skipped_frames == 0);
    nau_transport_freep(&t);
    close_media(&m);
}

/*
 * A short media looped as a whole: nope.media reads it to its end as soon
 * as it is opened, before it knows of the loop, set right away as a player
 * does, or later on: it goes round it all the same
 */
static void test_short_loop(int set_late)
{
    struct media m = open_media(fixture, 1, 0, 0, 0, 4096);
    if (set_late)
        nau_sleep_ms(200);
    const struct nau_transport_config tc = {0};
    struct nau_transport *t = nau_transport_create();
    assert(t);
    assert(nau_transport_init(t, m.engine, &tc) == NAU_OK);
    assert(nau_transport_set_duration(t, 4096) == NAU_OK);
    nau_transport_set_looping(t, 1);
    assert(nau_media_set_loop(m.media, 0, 4096) == NAU_OK);
    nau_transport_play(t);
    float out[2 * 256];
    for (int i = 0; i < 64; i++) {
        assert(nau_transport_render(t, 0, 256, out) == NAU_OK);
        check_fixture(out, i * 256 % 4096, 256);
    }
    assert(stats_of(&m).requests == 0);
    nau_transport_freep(&t);
    close_media(&m);
}

/* A position asked for as soon as the media is created, before its decoding thread runs: served all the same */
static void test_request_at_once(void)
{
    for (int i = 0; i < 20; i++) {
        struct media m = open_media(fixture_index, 1, 0, 0, 0, 48000);
        float out[2 * 64];
        assert(nau_engine_render(m.engine, 30000, 64, out) == NAU_OK);
        for (uint32_t f = 0; f < 64; f++)
            assert(index_of(out + 2 * f) == 30000 + f);
        assert(stats_of(&m).requests == 1);
        close_media(&m);
    }
}

/*
 * Scrubbing while paused, a seek after the other, nothing read meanwhile:
 * the audio decoded for each position left does not take the room of the
 * next, which is there when the playback goes on
 */
static void test_paused_seeks(void)
{
    struct media m = open_media(fixture_index, 0, 0, 0, 0, 48000);
    const struct nau_transport_config tc = {0};
    struct nau_transport *t = nau_transport_create();
    assert(t);
    assert(nau_transport_init(t, m.engine, &tc) == NAU_OK);
    assert(nau_transport_set_duration(t, 48000) == NAU_OK);
    float out[2 * 480];
    for (int i = 0; i < 30; i++) {
        assert(nau_transport_seek(t, 1000 + 1000 * i) == NAU_OK);
        assert(nau_transport_render(t, 0, 480, out) == NAU_OK);
        nau_sleep_ms(10);
    }
    nau_sleep_ms(100);
    /* In real time, 10 ms every 10 ms */
    nau_transport_play(t);
    for (int i = 0; i < 20; i++) {
        assert(nau_transport_render(t, 0, 480, out) == NAU_OK);
        for (uint32_t f = 0; f < 480; f++)
            assert(index_of(out + 2 * f) == 30000 + 480 * i + (int)f);
        nau_sleep_ms(10);
    }
    assert(stats_of(&m).silent_frames == 0);
    nau_transport_freep(&t);
    close_media(&m);
}

/*
 * A source positioned while paused at the end of its loop, as one past the
 * end of its range is, then going round it: its next pass is there from the
 * start, decoded meanwhile though the reader did not take the audio queued
 * for its previous position
 */
static void test_loop_from_end(void)
{
    struct media m = open_media(fixture_index, 0, 0, 0, 0, 48000);
    const struct nau_transport_config tc = {0};
    struct nau_transport *t = nau_transport_create();
    assert(t);
    assert(nau_transport_init(t, m.engine, &tc) == NAU_OK);
    assert(nau_transport_set_duration(t, 48000) == NAU_OK);
    nau_transport_set_looping(t, 1);
    assert(nau_transport_set_loop_range(t, 12000, 24000) == NAU_OK);
    assert(nau_media_set_loop(m.media, 12000, 24000) == NAU_OK);
    nau_sleep_ms(200);
    assert(nau_transport_seek(t, 24000) == NAU_OK);
    float out[2 * 480];
    assert(nau_transport_render(t, 0, 480, out) == NAU_OK);
    nau_sleep_ms(200);
    /* In real time, 10 ms every 10 ms */
    nau_transport_play(t);
    int64_t expected = 12000;
    for (int i = 0; i < 60; i++) {
        assert(nau_transport_render(t, 0, 480, out) == NAU_OK);
        for (uint32_t f = 0; f < 480; f++) {
            assert(index_of(out + 2 * f) == expected);
            if (++expected == 24000)
                expected = 12000;
        }
        nau_sleep_ms(10);
    }
    assert(stats_of(&m).requests == 1 && stats_of(&m).silent_frames == 0);
    nau_transport_freep(&t);
    close_media(&m);
}

/*
 * Playing in real time, rendering 10 ms every 10 ms: what plays is always
 * at its position, looping or after a seek
 */
static void test_realtime(void)
{
    struct media m = open_media(fixture_index, 0, 0, 0, 0, 48000);
    const struct nau_transport_config tc = {0};
    struct nau_transport *t = nau_transport_create();
    assert(t);
    assert(nau_transport_init(t, m.engine, &tc) == NAU_OK);
    assert(nau_transport_set_duration(t, 48000) == NAU_OK);
    nau_transport_set_looping(t, 1);
    assert(nau_transport_set_loop_range(t, 12000, 24000) == NAU_OK);
    assert(nau_media_set_loop(m.media, 12000, 24000) == NAU_OK);
    nau_transport_play(t);
    float out[2 * 480];
    int64_t position = 0, silent = 0, started = -1;
    for (int i = 0; i < 150; i++) {
        if (i == 100) {
            /* A seek away from the loop, then playing on from there */
            nau_transport_set_looping(t, 0);
            assert(nau_media_set_loop(m.media, 0, 0) == NAU_OK);
            assert(nau_transport_seek(t, 30000) == NAU_OK);
            position = 30000;
        }
        nau_transport_render(t, 0, 480, out);
        for (uint32_t f = 0; f < 480; f++) {
            const int64_t index = index_of(out + 2 * f);
            if (index < 0) {
                silent += started >= 0 && position < 48000; /* not past the end */
            } else {
                assert(index == position);
                if (started < 0)
                    started = i;
            }
            if (++position == 24000 && i < 100)
                position = 12000;
        }
        nau_sleep_ms(10);
    }
    const struct nau_media_stats stats = stats_of(&m);
    printf("realtime: started after %" PRId64 " renders, %" PRId64 " frames silent then (seek included), %" PRId64 " requests\n",
           started, silent, stats.requests);
    assert(started >= 0);
    assert(stats.requests == 2); /* the seek and the end going back to the start: going round the loop asks for nothing */
    nau_transport_freep(&t);
    close_media(&m);
}

/* Source-rate conversion must not introduce holes/duplicates between decoder
 * frames, and must drain the resampler at EOF. */
static void test_resample(void)
{
    struct media m = open_media(fixture_44100, 1, 0, 0, 0, 10000);
    float out[2 * 64];
    for (int64_t frame = 0; frame < 5120; frame += 64) {
        assert(nau_engine_render(m.engine, frame, 64, out) == NAU_OK);
        for (uint32_t i = 0; i < 64; i++) {
            int64_t index = frame + i;
            /* Ignore the resampler's edge transients. A one-sample drift
             * in the body is much larger than this quantization tolerance. */
            if (index < 100 || (index >= 4700 && index < 4800))
                continue;
            float expected = index < 4800 ? (float)(6000. / 32768. * sin(2 * 3.141592653589793 * 1000 * (double)index / 48000)) : 0.f;
            assert(fabsf(out[2 * i] - expected) < 0.0001f);
            assert(fabsf(out[2 * i + 1] + expected) < 0.0001f);
        }
    }
    close_media(&m);
}

static void test_mono(void)
{
    struct media m = open_media(fixture_mono, 1, 0, 0, 0, 4800);
    float out[2 * 64];
    for (int64_t frame = 0; frame < 4800; frame += 64) {
        assert(nau_engine_render(m.engine, frame, 64, out) == NAU_OK);
        for (uint32_t i = 0; i < 64; i++) {
            const float sample = (float)(1000 + frame + i) / 32768.f;
            assert(out[2 * i] == out[2 * i + 1] && out[2 * i] > 0);
            /* Mono to stereo: as is, or each side at -3 dB */
            assert(fabsf(out[2 * i] - sample) < 0.0001f || fabsf(out[2 * i] - sample * 0.70710678f) < 0.0001f);
        }
    }
    close_media(&m);
}

/* Back to the start of an AAC media, its first frame plays as when starting from there */
static void test_aac_start(void)
{
    struct media m = open_media(fixture_aac, 1, 0, 0, 0, 48000);
    float first[2 * 4800], again[2 * 4800];
    for (int64_t frame = 0; frame < 4800; frame += 64) {
        const uint32_t n = 4800 - frame < 64 ? (uint32_t)(4800 - frame) : 64;
        assert(nau_engine_render(m.engine, frame, n, first + 2 * frame) == NAU_OK);
    }
    float peak = 0;
    for (int i = 0; i < 2 * 1024; i++)
        peak = fmaxf(peak, fabsf(first[i]));
    assert(peak > 0.01f); /* not silent from the start */
    for (int64_t frame = 0; frame < 4800; frame += 64) {
        const uint32_t n = 4800 - frame < 64 ? (uint32_t)(4800 - frame) : 64;
        assert(nau_engine_render(m.engine, frame, n, again + 2 * frame) == NAU_OK);
    }
    assert(!memcmp(first, again, sizeof(first)));
    close_media(&m);
}

/*
 * A jump read before the decoder has followed: the read is silent, and the
 * audio of these frames is skipped once decoded, to stay in sync. These are
 * the frames counted as skipped.
 */
static void test_late(void)
{
    struct media m = open_media(fixture_index, 0, 0, 0, 0, 48000);
    nau_sleep_ms(100);
    float out[2 * 64];
    int ret = nau_engine_render(m.engine, 24000, 64, out);
    assert(ret == NAU_OK || ret == NAU_AGAIN);
    const int64_t silent = stats_of(&m).silent_frames;
    assert(silent == (ret == NAU_AGAIN ? 64 : 0));
    nau_sleep_ms(100);
    assert(nau_engine_render(m.engine, 24064, 64, out) == NAU_OK);
    for (uint32_t f = 0; f < 64; f++)
        assert(index_of(out + 2 * f) == 24064 + (int)f);
    const struct nau_media_stats stats = stats_of(&m);
    printf("late: %" PRId64 " frames silent, %" PRId64 " skipped\n", stats.silent_frames, stats.skipped_frames);
    assert(stats.silent_frames == silent && stats.skipped_frames == silent);
    close_media(&m);
}

/*
 * A loop starting in the middle of an AAC packet: each pass is decoded from
 * the start of the packet, and the audio before the start of the loop is
 * dropped. It is not late audio, and is not counted as skipped.
 */
static void test_aac_loop(void)
{
    enum { LOOP_START = 20000, LOOP_END = 44000, PASS_COUNT = 4 };
    struct media m = open_media(fixture_aac, 1, 0, 0, 0, 96000);
    const struct nau_transport_config tc = {0};
    struct nau_transport *t = nau_transport_create();
    assert(t);
    assert(nau_transport_init(t, m.engine, &tc) == NAU_OK);
    assert(nau_transport_set_duration(t, 96000) == NAU_OK);
    nau_transport_set_looping(t, 1);
    assert(nau_transport_set_loop_range(t, LOOP_START, LOOP_END) == NAU_OK);
    assert(nau_media_set_loop(m.media, LOOP_START, LOOP_END) == NAU_OK);
    assert(nau_transport_seek(t, LOOP_START) == NAU_OK);
    nau_transport_play(t);
    static float passes[PASS_COUNT][2 * (LOOP_END - LOOP_START)];
    for (int pass = 0; pass < PASS_COUNT; pass++) {
        for (int64_t frame = 0; frame < LOOP_END - LOOP_START; frame += 250)
            assert(nau_transport_render(t, 0, 250, passes[pass] + 2 * frame) == NAU_OK);
        /* Not silent, and the same audio on each pass decoded from the start of the loop */
        float peak = 0;
        for (int i = 0; i < 2 * (LOOP_END - LOOP_START); i++)
            peak = fmaxf(peak, fabsf(passes[pass][i]));
        assert(peak > 0.01f);
        if (pass > 1)
            assert(!memcmp(passes[pass], passes[1], sizeof(passes[pass])));
    }
    const struct nau_media_stats stats = stats_of(&m);
    printf("AAC loop: %" PRId64 " frames skipped, %" PRId64 " silent, %" PRId64 " requests\n",
           stats.skipped_frames, stats.silent_frames, stats.requests);
    assert(stats.skipped_frames == 0 && stats.silent_frames == 0);
    nau_transport_freep(&t);
    close_media(&m);
}

/* The loudness of `frames` stereo frames, in dB */
static double level_db(const float *frames, int64_t frame_count)
{
    double sum = 0;
    for (int64_t i = 0; i < 2 * frame_count; i++)
        sum += (double)frames[i] * frames[i];
    return 10 * log10(sum / (double)(2 * frame_count) + 1e-12);
}

static double correlation(const float *a, const float *b, int64_t frame_count)
{
    double ab = 0, aa = 0, bb = 0;
    for (int64_t i = 0; i < 2 * frame_count; i++) {
        ab += (double)a[i] * b[i];
        aa += (double)a[i] * a[i];
        bb += (double)b[i] * b[i];
    }
    return ab / sqrt(aa * bb + 1e-24);
}

/*
 * Normalizing lifts a quiet WAV, its channels with no layout: by up to 20 dB
 * in the middle second, the gain ramping up from the start and down to the end
 */
static void test_normalize(void)
{
    enum { LENGTH = 4 * 48000, MIDDLE = 72000, NB = 48000 };
    static float plain[2 * LENGTH], lifted[2 * LENGTH];
    for (int normalize = 0; normalize < 2; normalize++) {
        struct media m = open_media(fixture_quiet_wav, 1, normalize, 0, 0, LENGTH);
        float *out = normalize ? lifted : plain;
        for (int64_t frame = 0; frame < LENGTH; frame += 4800)
            assert(nau_engine_render(m.engine, frame, 4800, out + 2 * frame) == NAU_OK);
        close_media(&m);
    }
    const double gain = level_db(lifted + 2 * MIDDLE, NB) - level_db(plain + 2 * MIDDLE, NB);
    printf("normalize a quiet WAV: %+.1f dB\n", gain);
    assert(gain > 15);
}

/*
 * A normalized media played from a position, after a seek or as a track
 * trimmed, is at the level of the media played on to there, rather than
 * the normalization starting over from unity there: its decoder starts
 * ahead of the position, the audio before it dropped.
 */
static void test_normalize_from(void)
{
    /* Off the 150 ms frames of the normalization, from the start and from the pre-roll */
    enum { AT = 161760, NB = 24000, WINDOW = 2400, LENGTH = 6 * 48000 };
    static float on[2 * NB], sought[2 * NB], started[2 * NB], scratch[2 * 4800];
    struct media m = open_media(fixture_quiet, 1, 1, 0, 0, LENGTH);
    for (int64_t frame = 0; frame < AT; frame += 4800) {
        const int64_t n = AT - frame < 4800 ? AT - frame : 4800;
        assert(nau_engine_render(m.engine, frame, (uint32_t)n, scratch) == NAU_OK);
    }
    assert(nau_engine_render(m.engine, AT, NB, on) == NAU_OK);
    close_media(&m);
    m = open_media(fixture_quiet, 1, 1, 0, 0, LENGTH);
    assert(nau_engine_render(m.engine, AT, NB, sought) == NAU_OK);
    assert(stats_of(&m).requests == 1 && stats_of(&m).skipped_frames == 0);
    close_media(&m);
    m = open_media(fixture_quiet, 1, 1, 0, AT, LENGTH - AT);
    assert(nau_engine_render(m.engine, 0, NB, started) == NAU_OK);
    assert(stats_of(&m).requests == 0 && stats_of(&m).skipped_frames == 0);
    close_media(&m);
    /* At its level, from the very frame */
    double worst = 0;
    for (int64_t i = 0; i < NB; i += WINDOW) {
        const double ref = level_db(on + 2 * i, WINDOW);
        worst = fmax(worst, fmax(fabs(level_db(sought + 2 * i, WINDOW) - ref), fabs(level_db(started + 2 * i, WINDOW) - ref)));
    }
    const double c = fmin(correlation(on, sought, NB), correlation(on, started, NB));
    printf("normalize from a position: %.2f dB off at worst, correlation %.5f\n", worst, c);
    assert(worst < 0.5 && c > 0.999);
}

static void test_open_error(void)
{
    struct nau_engine *engine = NULL;
    const struct nau_engine_config config = {48000, 2, 256};
    engine = nau_engine_create();
    assert(engine);
    assert(nau_engine_init(engine, &config) == NAU_OK);
    char filename[4096];
    snprintf(filename, sizeof(filename), "%s/missing.wav", fixture);
    /* A failed initialization leaves the media to initialize again or release */
    struct nau_media *media = nau_media_create();
    assert(media);
    assert(nau_media_init(media, engine, filename, NULL) == NAU_ERROR_IO);
    assert(nau_media_get_length(media) == 0);
    nau_media_unrefp(&media);
    assert(!media);
    nau_engine_freep(&engine);
}

int main(int argc, char **argv)
{
    assert(argc == 9);
    fixture = argv[1];
    fixture_44100 = argv[2];
    fixture_index = argv[3];
    fixture_mono = argv[4];
    fixture_quiet_wav = argv[5];
    fixture_aac = argv[6];
    fixture_quiet = argv[7];
    fixture_gap = argv[8];
    test_open_error();
    test_read_and_seek();
    test_gap();
    test_loop(0);
    test_loop(1);
    test_short_loop(0);
    test_short_loop(1);
    test_request_at_once();
    test_paused_seeks();
    test_loop_from_end();
    test_realtime();
    test_resample();
    test_mono();
    test_aac_start();
    test_aac_loop();
    test_late();
    test_normalize();
    test_normalize_from();
    return 0;
}
