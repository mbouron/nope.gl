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

#define _GNU_SOURCE  /* RUSAGE_THREAD */
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <libavutil/intreadwrite.h>

#include "nopeaudio/nopeaudio.h"
#include "../src/utils/time.h"

#ifdef HAVE_VALGRIND_H
#include <valgrind/valgrind.h>
#else
#define RUNNING_ON_VALGRIND 0
#endif
#if defined(__SANITIZE_THREAD__)
#define THREAD_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define THREAD_SANITIZER 1
#endif
#endif
#ifndef THREAD_SANITIZER
#define THREAD_SANITIZER 0
#endif

#define CHECK_EDIT(call) do {                \
    int edit_ret;                            \
    while ((edit_ret = (call)) == NAU_AGAIN) \
        nau_sleep_ms(1);                     \
    assert(edit_ret == NAU_OK);              \
} while (0)

static const char *fixture, *fixture_index, *output_dir, *fixture_gap;

static void close_to(float a, float b)
{
    if (fabsf(a - b) > 0.0001f) {
        fprintf(stderr, "%f != %f\n", a, b);
        assert(0);
    }
}

static int64_t index_of(const float *frame)
{
    const long left = lroundf(frame[0] * 32768.f), right = lroundf(frame[1] * 32768.f);
    if (!left && !right)
        return -1;
    return (left - 1) | (right - 1) << 14;
}

/* Locate the samples without assuming a fixed header size: FFmpeg may
 * write JUNK, fact and LIST chunks before data. */
static uint32_t wav_data(FILE *f)
{
    uint8_t header[12];
    assert(fread(header, 1, 12, f) == 12);
    assert(!memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVE", 4));
    for (;;) {
        assert(fread(header, 1, 8, f) == 8);
        const uint32_t size = AV_RL32(header + 4);
        if (!memcmp(header, "data", 4))
            return size;
        assert(fseek(f, (long)size + (size & 1), SEEK_CUR) == 0);
    }
}

/* The frames of a stereo float WAV file written by the player */
static float *read_wav(const char *filename, int64_t *frame_count)
{
    FILE *f = fopen(filename, "rb");
    assert(f);
    const uint32_t size = wav_data(f);
    assert(size % 8 == 0);
    float *frames = malloc((size_t)size + 1);
    assert(frames && fread(frames, 1, size, f) == size);
    fclose(f);
    *frame_count = size / 8;
    return frames;
}

static struct nau_player *offline_player(int64_t duration)
{
    struct nau_player *p = NULL;
    const struct nau_player_config config = {.mode = NAU_PLAYER_OFFLINE};
    p = nau_player_create();
    assert(p);
    assert(nau_player_init(p, &config) == NAU_OK);
    assert(nau_player_get_sample_rate(p) == 48000);
    assert(nau_player_set_duration(p, duration) == NAU_OK);
    return p;
}

/* Automate a parameter of an effect */
static int automate(struct nau_player *p, struct nau_track_effect *effect, uint32_t param,
                    const struct nau_ramp *ramps, uint32_t count)
{
    const struct nau_lane lane = {param, ramps, count};
    return nau_player_set_track_effect_automation(p, effect, &lane, 1);
}

static void test_export(void)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/export.wav", output_dir);
    struct nau_player *p = offline_player(15000);
    struct nau_track *t = NULL;
    const struct nau_track_params params = {.filename = fixture_index, .start = 1000, .trim = 2000, .duration = 10000, .volume = 1};
    assert(nau_player_add_track(p, &params, &t) == NAU_OK);
    assert(nau_track_get_length(t) == 48000 && nau_track_get_duration(t) == 10000);
    assert(nau_player_export(p, path, NAU_EXPORT_F32) == NAU_OK);
    int64_t n;
    float *frames = read_wav(path, &n);
    assert(n == 15000);
    for (int64_t i = 0; i < n; i++)
        assert(index_of(frames + 2 * i) == (i >= 1000 && i < 11000 ? 2000 + i - 1000 : -1));
    free(frames);

    /* Moved and cut while laid: the rest of the media from its trim */
    assert(nau_player_set_track_timings(p, t, 500, 40000, -1) == NAU_OK);
    assert(nau_track_get_duration(t) == 8000);
    assert(nau_player_set_loop_range(p, 2000, 4000) == NAU_OK);
    assert(nau_player_set_loop_hold(p, 2400) == NAU_OK);
    assert(nau_player_set_looping(p, 1) == NAU_OK);
    assert(nau_player_export(p, path, NAU_EXPORT_F32) == NAU_OK);
    assert(nau_player_is_looping(p));
    frames = read_wav(path, &n);
    for (int64_t i = 0; i < n; i++)
        assert(index_of(frames + 2 * i) == (i >= 500 && i < 8500 ? 40000 + i - 500 : -1));
    free(frames);
    assert(nau_player_remove_track(p, &t) == NAU_OK && !t);
    nau_player_freep(&p);
}

/* Export a timestamp gap as silence, preserving the audio on both sides. */
static void test_export_gap(void)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/export_gap.wav", output_dir);
    struct nau_player *p = offline_player(24960);
    struct nau_track *t = NULL;
    const struct nau_track_params params = {.filename = fixture_gap, .duration = -1, .volume = 1};
    assert(nau_player_add_track(p, &params, &t) == NAU_OK);
    assert(nau_player_export(p, path, NAU_EXPORT_F32) == NAU_OK);
    int64_t n;
    float *frames = read_wav(path, &n);
    assert(n == 24960);
    for (int64_t i = 0; i < n; i++) {
        const float sample = i < 480 || i >= 24480 ? 0.25f : 0.f;
        close_to(frames[2 * i], sample);
        close_to(frames[2 * i + 1], -sample);
    }
    free(frames);
    nau_player_freep(&p);
}

/* The fixture's sample at a frame of its media */
static float fixture_at(int64_t frame)
{
    return frame < 4096 ? (float)(1000 + frame) / 32768.f : 0.f;
}

static void test_mix(void)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/mix.wav", output_dir);
    struct nau_player *p = offline_player(6000);
    struct nau_track *a = NULL, *b = NULL;
    const struct nau_track_params pa = {.filename = fixture, .start = 0, .duration = -1, .volume = 0.5f};
    const struct nau_track_params pb = {.filename = fixture, .start = 1000, .trim = 100, .duration = 2000, .volume = 1};
    assert(nau_player_add_track(p, &pa, &a) == NAU_OK);
    assert(nau_player_add_track(p, &pb, &b) == NAU_OK);
    /* From the start of the track: 0.25 from its frame 100, then a ramp to 1 over its frames [200, 300) */
    const struct nau_ramp ramps[] = {{100, 0, 0.25f, NAU_RAMP_LINEAR, 0}, {200, 100, 1, NAU_RAMP_LINEAR, 0}};
    assert(nau_player_set_track_fader(p, b, ramps, 2) == NAU_OK);
    assert(nau_player_set_volume(p, 0.8f) == NAU_OK);
    close_to(nau_player_get_volume(p), 0.8f);
    assert(nau_player_export(p, path, NAU_EXPORT_F32) == NAU_OK);
    int64_t n;
    float *frames = read_wav(path, &n);
    assert(n == 6000);
    for (int64_t i = 0; i < n; i++) {
        float expected = 0.5f * fixture_at(i);
        if (i >= 1000 && i < 3000) {
            const int64_t local = i - 1000;
            const float fader = local < 100 ? 1 : local < 200 ? 0.25f : local < 300 ? 0.25f + 0.75f * (float)(local - 200) / 100 : 1;
            expected += fader * fixture_at(100 + local);
        }
        close_to(frames[2 * i], 0.8f * expected);
        close_to(frames[2 * i + 1], -0.8f * expected);
    }
    free(frames);
    /* As the application exports: 16 bits */
    assert(nau_player_export(p, path, NAU_EXPORT_S16) == NAU_OK);
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(wav_data(f) == 6000 * 4);
    fclose(f);
    assert(nau_player_clear_tracks(p) == NAU_OK);
    nau_player_freep(&p);
}

/*
 * The audio thread never waits, whatever the application does meanwhile:
 * a thread renders 10 ms every 10 ms, its voluntary context switches
 * counted in each render, while the control thread seeks, loops, holds,
 * adds and removes tracks, and moves their timings, faders and volumes.
 */
struct renderer {
    struct nau_player *player;
    _Atomic int stop;
    _Atomic int64_t renders;
    _Atomic int64_t waits;       /* voluntary context switches in renders that did not fault */
    _Atomic int64_t max_ns;
    _Atomic int64_t audible;
    _Atomic long peak;           /* the loudest sample, in 1/32768 */
};

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void *run_renderer(void *arg)
{
    struct renderer *r = arg;
    float out[2 * 480];
    int64_t next = now_ns();
    while (!atomic_load(&r->stop)) {
#ifdef RUSAGE_THREAD
        struct rusage before, after;
        getrusage(RUSAGE_THREAD, &before);
#endif
        const int64_t start = now_ns();
        assert(nau_player_render(r->player, start, 480, out) >= 0);
        const int64_t spent = now_ns() - start;
#ifdef RUSAGE_THREAD
        getrusage(RUSAGE_THREAD, &after);
        if (after.ru_minflt == before.ru_minflt && after.ru_majflt == before.ru_majflt)
            atomic_fetch_add(&r->waits, after.ru_nvcsw - before.ru_nvcsw);
#endif
        if (spent > atomic_load(&r->max_ns))
            atomic_store(&r->max_ns, spent);
        for (int i = 0; i < 960; i++)
            if (out[i] != 0.f) {
                atomic_fetch_add(&r->audible, 1);
                break;
            }
        for (int i = 0; i < 960; i++) {
            const long sample = lroundf(fabsf(out[i]) * 32768.f);
            if (sample > atomic_load(&r->peak))
                atomic_store(&r->peak, sample);
        }
        atomic_fetch_add(&r->renders, 1);
        next += 10000000;
        const int64_t left = next - now_ns();
        if (left > 0)
            nau_sleep_ms((uint32_t)(left / 1000000));
    }
    return NULL;
}

struct fast_renderer {
    struct nau_player *player;
    _Atomic int stop;
};

/* Renders of 16 frames in real time, writing the clock 3000 times a second */
static void *run_fast_renderer(void *arg)
{
    struct fast_renderer *r = arg;
    float out[2 * 16];
    int64_t next = now_ns();
    while (!atomic_load(&r->stop)) {
        assert(nau_player_render(r->player, now_ns() + 10000000, 16, out) >= 0);
        next += 16 * 1000000000LL / 48000;
        while (now_ns() < next && !atomic_load(&r->stop))
            ;
    }
    return NULL;
}

/*
 * The position heard is told whenever asked while the renderer writes the
 * clock: a reader meeting a write waits for it to end
 */
static void test_position_while_rendering(void)
{
    struct nau_player *p = NULL;
    const struct nau_player_config config = {.mode = NAU_PLAYER_HOST};
    p = nau_player_create();
    assert(p);
    assert(nau_player_init(p, &config) == NAU_OK);
    assert(nau_player_set_duration(p, 48000 * 60) == NAU_OK);
    struct fast_renderer r = {.player = p};
    pthread_t thread;
    assert(nau_player_start_rendering(p) == NAU_OK);
    assert(nau_player_play(p) == NAU_OK);
    assert(!pthread_create(&thread, NULL, run_fast_renderer, &r));
    nau_sleep_ms(50);
    int64_t unanswered = 0, queries = 0;
    const int64_t end = now_ns() + 1000000000;
    while (now_ns() < end) {
        int64_t heard;
        unanswered += nau_player_get_position_at(p, now_ns(), &heard) == NAU_AGAIN;
        queries++;
    }
    atomic_store(&r.stop, 1);
    assert(!pthread_join(thread, NULL));
    assert(nau_player_stop_rendering(p) == NAU_OK);
    printf("position while rendering: %" PRId64 " of %" PRId64 " queries unanswered\n", unanswered, queries);
    assert(unanswered == 0);
    nau_player_freep(&p);
}

static void test_never_waits(void)
{
    struct nau_player *p = NULL;
    const struct nau_player_config config = {.mode = NAU_PLAYER_HOST};
    p = nau_player_create();
    assert(p);
    assert(nau_player_init(p, &config) == NAU_OK);
    assert(nau_player_set_duration(p, 96000) == NAU_OK);
    struct nau_track *tracks[9] = {0};
    struct nau_track_effect *effects[4] = {0};
    for (int i = 0; i < 4; i++) {
        const struct nau_track_params params = {.filename = fixture_index, .start = 6000 * i, .duration = -1, .volume = 0.25f};
        CHECK_EDIT(nau_player_add_track(p, &params, &tracks[i]));
    }
    struct renderer r = {.player = p};
    pthread_t thread;
    assert(nau_player_start_rendering(p) == NAU_OK);
    assert(!pthread_create(&thread, NULL, run_renderer, &r));
    assert(nau_player_play(p) == NAU_OK);
    uint32_t seed = 7;
    for (int i = 0; i < 120; i++) {
        seed = seed * 1103515245 + 12345;
        const uint32_t what = (seed >> 16) % 8;
        const int64_t frame = (seed >> 4) % 90000;
        if (what == 0) {
            assert(nau_player_seek(p, frame) == NAU_OK);
        } else if (what == 1) {
            assert(nau_player_set_looping(p, 1) == NAU_OK);
            assert(nau_player_set_loop_range(p, frame, frame + 12000) == NAU_OK);
            assert(nau_player_set_loop_hold(p, (seed >> 8) % 2 ? 2400 : 0) == NAU_OK);
        } else if (what == 2) {
            assert(nau_player_set_looping(p, 0) == NAU_OK);
            assert(nau_player_play(p) == NAU_OK);
        } else if (what == 3) {
            const int slot = 4 + (int)((seed >> 8) % 5);
            if (tracks[slot]) {
                CHECK_EDIT(nau_player_remove_track(p, &tracks[slot]));
            } else {
                const struct nau_track_params params = {.filename = fixture_index, .start = frame, .trim = 1000, .duration = 24000, .volume = 0.25f, .normalize = slot % 2};
                CHECK_EDIT(nau_player_add_track(p, &params, &tracks[slot]));
            }
        } else if (what == 4) {
            const struct nau_ramp ramps[] = {{0, 0, 0.5f, NAU_RAMP_LINEAR, 0}, {(int64_t)(seed % 20000), 4800, 0.1f, NAU_RAMP_LINEAR, 0}};
            CHECK_EDIT(nau_player_set_track_fader(p, tracks[seed % 4], ramps, 2));
            /* Effects added, edited in place, moved and removed: a filter on
             * each track */
            const uint32_t slot = seed % 4;
            const enum nau_track_effect_placement placement =
                (seed >> 9) % 2 ? NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER : NAU_TRACK_EFFECT_PLACEMENT_POST_FADER;
            if (!effects[slot]) {
                CHECK_EDIT(nau_player_add_track_effect(p, tracks[slot], NAU_EFFECT_LOWPASS, placement, &effects[slot]));
            } else if ((seed >> 10) % 4 == 0) {
                CHECK_EDIT(nau_player_remove_track_effect(p, &effects[slot]));
            } else {
                const struct nau_ramp automation[] = {
                    {.value = 2000.f},
                    {.frame = seed % 20000, .length = 4800, .value = (float)(300 + seed % 1000)},
                };
                CHECK_EDIT(automate(p, effects[slot], NAU_LOWPASS_CUTOFF, automation, 2));
                CHECK_EDIT(nau_player_move_track_effect(p, effects[slot], placement, 0));
            }
        } else if (what == 5) {
            CHECK_EDIT(nau_player_set_track_timings(p, tracks[seed % 4], frame, (seed >> 3) % 20000, 20000));
        } else if (what == 6) {
            assert(nau_player_set_track_volume(p, tracks[seed % 4], (float)(seed % 100) / 100) == NAU_OK);
            if (effects[0])
                assert(nau_player_set_track_effect_param(p, effects[0], NAU_LOWPASS_AMOUNT, (float)(seed % 150) / 100) == NAU_OK);
            if (effects[2])
                assert(nau_player_set_track_effect_param(p, effects[2], NAU_LOWPASS_RESONANCE_SCALE, (float)(seed % 300) / 100) == NAU_OK);
            assert(nau_player_set_volume(p, 0.5f + (float)(seed % 50) / 100) == NAU_OK);
        } else {
            assert(nau_player_pause(p) == NAU_OK);
            nau_sleep_ms(20);
            assert(nau_player_play(p) == NAU_OK);
        }
        nau_sleep_ms(25);
        int64_t heard;
        const int clock_ret = nau_player_get_position_at(p, now_ns(), &heard);
        assert(clock_ret == NAU_OK || clock_ret == NAU_AGAIN);
        if (clock_ret == NAU_OK)
            assert(heard >= 0 && heard <= 96000);
    }
    atomic_store(&r.stop, 1);
    assert(!pthread_join(thread, NULL));
    assert(nau_player_stop_rendering(p) == NAU_OK);
    struct nau_player_stats stats;
    nau_player_get_stats(p, &stats);
    printf("never waits: %" PRId64 " renders, %" PRId64 " waits, longest %.3f ms, %" PRId64 " audible, %" PRId64 " requests, %" PRId64 " frames silent\n",
           atomic_load(&r.renders), atomic_load(&r.waits), (double)atomic_load(&r.max_ns) / 1e6, atomic_load(&r.audible),
           stats.requests, stats.silent_frames);
    assert(stats.errors == 0 && atomic_load(&r.audible) > 0);
    /* Threads switch out of their own accord under Valgrind and TSan, which serialize them */
#ifdef RUSAGE_THREAD
    if (!RUNNING_ON_VALGRIND && !THREAD_SANITIZER)
        assert(atomic_load(&r.waits) == 0);
#endif
    nau_player_freep(&p);
}

/* Edits while paused reach the graph at once, though nothing is heard: the
 * renders adopt them, the control thread not waiting for the playback */
static void test_edits_while_paused(void)
{
    struct nau_player *p = NULL;
    const struct nau_player_config config = {.mode = NAU_PLAYER_HOST};
    p = nau_player_create();
    assert(p);
    assert(nau_player_init(p, &config) == NAU_OK);
    /* Each track at most 0x4000 / 32768 alone */
    assert(nau_player_set_duration(p, 48000) == NAU_OK);
    const struct nau_track_params params = {.filename = fixture_index, .duration = -1, .volume = 1};
    struct nau_track *tracks[4] = {0};
    CHECK_EDIT(nau_player_add_track(p, &params, &tracks[0]));
    struct renderer r = {.player = p};
    pthread_t thread;
    assert(nau_player_start_rendering(p) == NAU_OK);
    assert(!pthread_create(&thread, NULL, run_renderer, &r));
    assert(nau_player_play(p) == NAU_OK);
    nau_sleep_ms(100);
    assert(nau_player_pause(p) == NAU_OK);
    nau_sleep_ms(100);
    const int64_t start = now_ns();
    for (int i = 1; i < 4; i++)
        CHECK_EDIT(nau_player_add_track(p, &params, &tracks[i]));
    const struct nau_ramp ramp = {0, 0, 1, NAU_RAMP_LINEAR, 0};
    for (int i = 0; i < 4; i++)
        CHECK_EDIT(nau_player_set_track_fader(p, tracks[i], &ramp, 1));
    /* A render or so each, rather than the 2 s a commit gives up after */
    assert(now_ns() - start < 1000000000);
    atomic_store(&r.peak, 0);
    assert(nau_player_play(p) == NAU_OK);
    nau_sleep_ms(200);
    /* More than two tracks heard together */
    assert(atomic_load(&r.peak) > 2 * 0x4000);
    atomic_store(&r.stop, 1);
    assert(!pthread_join(thread, NULL));
    assert(nau_player_stop_rendering(p) == NAU_OK);
    nau_player_freep(&p);
}

static void test_host_stop(void)
{
    struct nau_player *p = NULL;
    const struct nau_player_config config = {.mode = NAU_PLAYER_HOST};
    p = nau_player_create();
    assert(p);
    assert(nau_player_init(p, &config) == NAU_OK);
    assert(nau_player_set_duration(p, 480000) == NAU_OK);
    float output[2];
    assert(nau_player_render(p, 0, 1, output) == NAU_ERROR_INVALID);

    struct renderer r = {.player = p};
    pthread_t thread;
    assert(nau_player_start_rendering(p) == NAU_OK);
    assert(!pthread_create(&thread, NULL, run_renderer, &r));
    while (!atomic_load(&r.renders))
        nau_sleep_ms(1);
    for (uint32_t i = 0; i < 200; i++) {
        assert(nau_player_play(p) == NAU_OK);
        assert(nau_player_stop(p) == NAU_OK);
        nau_sleep_ms(1);
    }
    atomic_store(&r.stop, 1);
    assert(!pthread_join(thread, NULL));
    assert(nau_player_stop_rendering(p) == NAU_OK);
    assert(nau_player_get_position(p) == 0);
    assert(nau_player_render(p, 0, 1, output) == NAU_ERROR_INVALID);
    nau_player_freep(&p);
}

/*
 * A track added while the playback loops past it, as the tracks of a scene
 * laid again while playing, has its audio on time on the next pass: its
 * source is positioned at the end of its range, which its decoder serves
 * from the start of the loop.
 */
static void test_add_past_track(void)
{
    struct nau_player *p = NULL;
    const struct nau_player_config config = {.mode = NAU_PLAYER_HOST};
    p = nau_player_create();
    assert(p);
    assert(nau_player_init(p, &config) == NAU_OK);
    assert(nau_player_set_duration(p, 48000) == NAU_OK);
    assert(nau_player_set_looping(p, 1) == NAU_OK);
    assert(nau_player_seek(p, 36000) == NAU_OK);
    struct renderer r = {.player = p};
    pthread_t thread;
    assert(nau_player_start_rendering(p) == NAU_OK);
    assert(!pthread_create(&thread, NULL, run_renderer, &r));
    assert(nau_player_play(p) == NAU_OK);
    nau_sleep_ms(50);
    /* [0.25 s, 0.5 s) of the project from 0.1 s of the media */
    const struct nau_track_params params = {
        .filename = fixture_index, .start = 12000, .trim = 4800, .duration = 12000, .volume = 1,
    };
    struct nau_track *track = NULL;
    CHECK_EDIT(nau_player_add_track(p, &params, &track));
    /* Round the loop and through the track */
    nau_sleep_ms(800);
    atomic_store(&r.stop, 1);
    assert(!pthread_join(thread, NULL));
    assert(nau_player_stop_rendering(p) == NAU_OK);
    struct nau_player_stats stats;
    nau_player_get_stats(p, &stats);
    printf("add past track: %" PRId64 " frames silent, %" PRId64 " requests\n", stats.silent_frames, stats.requests);
    assert(stats.errors == 0 && atomic_load(&r.audible) > 0);
    assert(stats.silent_frames == 0);
    nau_player_freep(&p);
}

static bool is_silent(const float *frame)
{
    return frame[0] == 0.f && frame[1] == 0.f;
}

/* The first frame heard of `out` from `from`, -1 if none */
static int64_t heard_from(const float *out, int64_t from, int64_t frame_count)
{
    int64_t i = from;
    while (i < frame_count && is_silent(out + 2 * i))
        i++;
    return i < frame_count ? i : -1;
}

/* The first frame after the next silence of `out`, from `from`, -1 if none */
static int64_t after_silence(const float *out, int64_t from, int64_t frame_count)
{
    int64_t i = from;
    while (i < frame_count && !is_silent(out + 2 * i))
        i++;
    while (i < frame_count && is_silent(out + 2 * i))
        i++;
    return i < frame_count ? i : -1;
}

/* The volume of a track set while the player is paused: the playback
 * resumes at this volume, the previous one is not heard on its way out */
static void test_volume_while_paused(void)
{
    struct nau_player *p = NULL;
    struct nau_track *t = NULL;
    const struct nau_player_config config = {.mode = NAU_PLAYER_HOST};
    p = nau_player_create();
    assert(p);
    assert(nau_player_init(p, &config) == NAU_OK);
    assert(nau_player_set_duration(p, 48000) == NAU_OK);
    const struct nau_track_params params = {.filename = fixture_index, .duration = -1, .volume = 1};
    assert(nau_player_add_track(p, &params, &t) == NAU_OK);
    assert(nau_player_start_rendering(p) == NAU_OK);
    assert(nau_player_play(p) == NAU_OK);
    static float out[2 * 4800];
    /* Until the track is heard at full level */
    bool heard = false;
    for (int i = 0; i < 50 && !heard; i++) {
        assert(nau_player_render(p, 0, 480, out) >= 0);
        heard = index_of(&out[2 * 479]) >= 0;
        nau_sleep_ms(1);
    }
    assert(heard);
    assert(nau_player_pause(p) == NAU_OK);
    assert(nau_player_render(p, 0, 480, out) >= 0);
    assert(nau_player_render(p, 0, 480, out) == NAU_OK);
    assert(is_silent(&out[0]) && is_silent(&out[2 * 479]));
    assert(nau_player_set_track_volume(p, t, 0.f) == NAU_OK);
    assert(nau_player_play(p) == NAU_OK);
    assert(nau_player_render(p, 0, 4800, out) >= 0);
    for (int i = 0; i < 4800; i++)
        assert(is_silent(&out[2 * i]));
    assert(nau_player_stop_rendering(p) == NAU_OK);
    nau_player_freep(&p);
}

/*
 * Playback heard from the very frame played from, after play as after a
 * seek: the position holds while the decoder gets there, rather than skip
 * what it decodes meanwhile.
 */
static void test_heard_from_jump(void)
{
    struct nau_player *p = NULL;
    const struct nau_player_config config = {.mode = NAU_PLAYER_HOST};
    p = nau_player_create();
    assert(p);
    assert(nau_player_init(p, &config) == NAU_OK);
    assert(nau_player_set_duration(p, 96000) == NAU_OK);
    const struct nau_track_params params = {.filename = fixture_index, .duration = -1, .volume = 1};
    struct nau_track *track = NULL;
    CHECK_EDIT(nau_player_add_track(p, &params, &track));
    assert(nau_player_start_rendering(p) == NAU_OK);
    assert(nau_player_play(p) == NAU_OK);
    /* Renders of 1 ms, at most every millisecond: the decoder may lag */
    enum { BLOCK = 48, SEEK_AT = 4800, NB_FRAMES = 19200 };
    const int64_t declick = 240, target = 30000;
    float *out = calloc(2 * NB_FRAMES, sizeof(*out));
    assert(out);
    for (int64_t done = 0; done < NB_FRAMES; done += BLOCK) {
        if (done == SEEK_AT)
            assert(nau_player_seek(p, target) == NAU_OK);
        assert(nau_player_render(p, now_ns(), BLOCK, out + 2 * done) >= 0);
        nau_sleep_ms(1);
    }
    assert(nau_player_stop_rendering(p) == NAU_OK);
    /* Past the ramp in, the frames played from on: at the first render if
     * the decoder was there, after holding silent otherwise */
    const int64_t first = heard_from(out, 0, NB_FRAMES);
    assert(first >= 0 && first + declick < SEEK_AT);
    const int64_t heard = index_of(out + 2 * (first + declick));
    const int64_t after_seek = after_silence(out, SEEK_AT, NB_FRAMES);
    assert(after_seek >= 0 && after_seek + declick < NB_FRAMES);
    const int64_t heard_seek = index_of(out + 2 * (after_seek + declick));
    struct nau_player_stats stats;
    nau_player_get_stats(p, &stats);
    printf("heard from jump: %" PRId64 " and %" PRId64 " frames skipped, audible after %" PRId64 " and %" PRId64
           " frames, %" PRId64 " frames silent\n", heard - declick, heard_seek - declick - target, first,
           after_seek - SEEK_AT, stats.silent_frames);
    assert(heard == declick && heard_seek == target + declick);
    assert(stats.errors == 0 && stats.silent_frames == 0);
    free(out);
    nau_player_freep(&p);
}

static void test_device(void)
{
    struct nau_player *p = NULL;
    const struct nau_player_config config = {.mode = NAU_PLAYER_NULL_DEVICE, .period_ms = 10};
    p = nau_player_create();
    assert(p);
    assert(nau_player_init(p, &config) == NAU_OK);
    assert(nau_player_set_duration(p, 48000) == NAU_OK);
    struct nau_track *t = NULL;
    const struct nau_track_params params = {.filename = fixture_index, .duration = -1, .volume = 1};
    assert(nau_player_add_track(p, &params, &t) == NAU_OK);
    /* Paused, before the device started: the seek applied at once */
    assert(nau_player_seek(p, 4800) == NAU_OK);
    assert(nau_player_get_position(p) == 4800);
    assert(nau_player_play(p) == NAU_OK && nau_player_is_playing(p));
    nau_sleep_ms(300);
    const int64_t position = nau_player_get_position(p);
    assert(position > 4800 + 48 * 200 && position < 4800 + 48 * 400);
    int64_t heard = -1;
    for (int i = 0; i < 100; i++) {
        if (nau_player_get_position_at(p, nau_sink_now(), &heard) == NAU_OK)
            break;
        nau_sleep_ms(1);
    }
    const int64_t rendered = nau_player_get_position(p);
    assert(heard >= 0 && heard <= rendered && heard > rendered - 48 * 100);
    /* To the end, stopped at its start */
    nau_sleep_ms(1000);
    assert(!nau_player_is_playing(p) && nau_player_get_position(p) == 0);
    assert(nau_player_play(p) == NAU_OK);
    nau_sleep_ms(100);
    assert(nau_player_stop(p) == NAU_OK);
    assert(!nau_player_is_playing(p) && nau_player_get_position(p) == 0);
    struct nau_player_stats stats;
    nau_player_get_stats(p, &stats);
    assert(stats.errors == 0);
    nau_player_freep(&p);
}

static void test_time_limits(void)
{
    struct nau_player *p = offline_player(48000);
    struct nau_track *t = NULL;
    const struct nau_track_params params = {.filename = fixture_index, .duration = -1, .volume = 1};
    assert(nau_player_add_track(p, &params, &t) == NAU_OK);
    assert(nau_player_set_track_timings(p, t, 0, INT64_MAX, 100) == NAU_OK);
    assert(nau_track_get_duration(t) == 0);
    assert(nau_player_set_track_timings(p, t, INT64_MAX, 0, 100) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_timings(p, t, 100, 0, 100) == NAU_OK);
    /* The automation counts from the start of its track, wherever the track is */
    const struct nau_ramp volume_ramp = {.frame = INT64_MAX, .value = 1};
    assert(nau_player_set_track_fader(p, t, &volume_ramp, 1) == NAU_OK);
    assert(nau_player_set_track_fader(p, t, &volume_ramp, UINT32_MAX) == NAU_ERROR_INVALID);
    struct nau_track_effect *lowpass = NULL;
    assert(nau_player_add_track_effect(p, t, NAU_EFFECT_LOWPASS, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, &lowpass) == NAU_OK);
    const struct nau_ramp cutoff = {.frame = INT64_MAX, .value = 1000};
    assert(automate(p, lowpass, NAU_LOWPASS_CUTOFF, &cutoff, 1) == NAU_OK);
    assert(nau_player_set_track_timings(p, t, INT64_MAX - 100, 100, 100) == NAU_OK);
    assert(nau_player_play(p) == NAU_OK);
    float output[2 * 256];
    assert(nau_player_render(p, 0, 256, output) == NAU_OK);
    assert(nau_player_pause(p) == NAU_OK);
    assert(nau_player_set_track_timings(p, t, 100, 0, 100) == NAU_OK);

    /* RF64 removes the RIFF duration limit. An invalid output path must
     * still fail before rendering, even for a very long project. */
    assert(nau_player_set_duration(p, INT64_MAX) == NAU_OK);
    assert(nau_player_set_track_timings(p, t, INT64_MAX - 100, 100, 100) == NAU_OK);
    assert(nau_player_set_looping(p, 1) == NAU_OK);
    assert(nau_player_export(p, output_dir, NAU_EXPORT_F32) == NAU_ERROR_IO);
    nau_player_freep(&p);
}

static void test_pending_edits(void)
{
    struct nau_player *p = NULL;
    const struct nau_player_config config = {.mode = NAU_PLAYER_HOST};
    p = nau_player_create();
    assert(p);
    assert(nau_player_init(p, &config) == NAU_OK);
    assert(nau_player_set_duration(p, 48000) == NAU_OK);
    struct nau_track *track = NULL;
    const struct nau_track_params params = {.filename = fixture_index, .duration = -1, .volume = 1};
    assert(nau_player_add_track(p, &params, &track) == NAU_OK);
    const struct nau_ramp unity = {.value = 1};
    const struct nau_ramp mute = {.value = 0};

    assert(nau_player_start_rendering(p) == NAU_OK);
    assert(nau_player_set_track_fader(p, track, &unity, 1) == NAU_OK);
    assert(nau_player_set_track_timings(p, track, 1000, 2000, 100) == NAU_AGAIN);
    assert(nau_track_get_duration(track) == 48000);
    assert(nau_player_set_track_fader(p, track, &mute, 1) == NAU_AGAIN);
    struct nau_track *other = NULL;
    assert(nau_player_add_track(p, &params, &other) == NAU_AGAIN);
    assert(!other);
    struct nau_track *saved = track;
    assert(nau_player_remove_track(p, &track) == NAU_AGAIN);
    assert(track == saved);
    assert(nau_player_clear_tracks(p) == NAU_AGAIN);
    assert(nau_player_set_track_volume(p, track, 1) == NAU_OK);

    float output[2 * 256];
    assert(nau_player_render(p, 0, 1, output) == NAU_OK);
    assert(nau_player_stop_rendering(p) == NAU_OK);
    struct nau_track_params silent_params = params;
    silent_params.volume = 0;
    assert(nau_player_add_track(p, &silent_params, &other) == NAU_OK);
    assert(nau_player_start_rendering(p) == NAU_OK);
    assert(nau_player_play(p) == NAU_OK);
    int heard = 0;
    for (int i = 0; i < 64; i++) {
        /* Held while the tracks get their data, playing from there */
        const int64_t from = nau_player_get_position(p);
        const int ret = nau_player_render(p, 0, 256, output);
        assert(ret >= 0);
        if (ret == NAU_OK) {
            assert(index_of(output + 2 * 255) == from + 255);
            heard = 1;
            break;
        }
        nau_sleep_ms(1);
    }
    assert(heard);
    assert(nau_player_stop_rendering(p) == NAU_OK);
    assert(nau_player_remove_track(p, &track) == NAU_OK && !track);
    nau_player_freep(&p);
}

/* The delay of the 24 dB filter at low frequencies, in frames: half the sum
 * of 1 / (Q g) over its stages */
static double lowpass_delay(double cutoff, double resonance)
{
    const double g = tan(3.14159265358979323846 * cutoff / 48000);
    return (1 / 0.54119610 + 1 / (1.84775907 * resonance)) / (2 * g);
}

/* The fixture's left sample, delayed: its slope passing a low-pass filter */
static void lagged(float sample, int64_t frame, double delay)
{
    const double expected = (1000. + (double)frame - delay) / 32768.;
    if (fabs(sample - expected) > 5e-5) {
        fprintf(stderr, "frame %" PRId64 ": %f != %f\n", frame, sample, expected);
        assert(0);
    }
}

/* The frames of the project exported to a file of the output directory */
static float *export_frames(struct nau_player *p, const char *name, int64_t frame_count)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", output_dir, name);
    assert(nau_player_export(p, path, NAU_EXPORT_F32) == NAU_OK);
    int64_t n;
    float *frames = read_wav(path, &n);
    assert(n == frame_count);
    return frames;
}

/* A ramp of the cutoff and of the resonance of a low-pass filter */
struct filter_ramp {
    int64_t frame, length;
    float cutoff, resonance;
};

/* Automate the cutoff and the resonance of a low-pass filter with the same ramps */
static int set_filter(struct nau_player *p, struct nau_track_effect *lowpass, const struct filter_ramp *ramps,
                      uint32_t count)
{
    struct nau_ramp cutoffs[4], resonances[4];
    assert(count <= 4);
    for (uint32_t i = 0; i < count; i++) {
        cutoffs[i] = (struct nau_ramp){.frame = ramps[i].frame, .length = ramps[i].length, .value = ramps[i].cutoff};
        resonances[i] = (struct nau_ramp){.frame = ramps[i].frame, .length = ramps[i].length, .value = ramps[i].resonance};
    }
    const struct nau_lane lanes[] = {
        {NAU_LOWPASS_CUTOFF, cutoffs, count},
        {NAU_LOWPASS_RESONANCE, resonances, count},
    };
    return nau_player_set_track_effect_automation(p, lowpass, lanes, 2);
}

/*
 * Under water from frame 1000 of the track to frame 2000: the filter closes
 * to 400 Hz over 200 frames, and opens again over 1000 frames. The fixture,
 * a slow ramp, comes out of it delayed.
 */
static void test_lowpass(void)
{
    struct nau_player *p = offline_player(6000);
    struct nau_track *t = NULL;
    const struct nau_track_params params = {.filename = fixture, .duration = -1, .volume = 1};
    assert(nau_player_add_track(p, &params, &t) == NAU_OK);
    float *dry = export_frames(p, "lowpass.wav", 6000);

    struct nau_track_effect *lowpass = NULL;
    assert(nau_player_add_track_effect(p, t, NAU_EFFECT_LOWPASS, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, &lowpass) == NAU_OK);
    const struct filter_ramp underwater[] = {
        {1000, 200, 400, 1.2f},
        {2000, 1000, NAU_LOWPASS_OPEN, NAU_LOWPASS_FLAT},
    };
    assert(set_filter(p, lowpass, underwater, 2) == NAU_OK);
    /* Failed edits leave the filter as it was */
    const struct nau_ramp invalid = {.value = NAN}, cutoff = {.value = 1000};
    assert(automate(p, lowpass, NAU_LOWPASS_CUTOFF, &invalid, 1) == NAU_ERROR_INVALID);
    assert(automate(p, lowpass, NAU_LOWPASS_CUTOFF, NULL, 2) == NAU_ERROR_INVALID);
    assert(automate(p, lowpass, NAU_LOWPASS_CUTOFF, &cutoff, 65537) == NAU_ERROR_INVALID);
    assert(automate(p, lowpass, 99, &cutoff, 1) == NAU_ERROR_INVALID);
    assert(automate(NULL, lowpass, NAU_LOWPASS_CUTOFF, &cutoff, 1) == NAU_ERROR_INVALID);
    assert(automate(p, NULL, NAU_LOWPASS_CUTOFF, &cutoff, 1) == NAU_ERROR_INVALID);
    /* Lanes are set together or not at all, each one once */
    const struct nau_lane valid_first[] = {{NAU_LOWPASS_CUTOFF, &cutoff, 1}, {NAU_LOWPASS_RESONANCE, &invalid, 1}};
    assert(nau_player_set_track_effect_automation(p, lowpass, valid_first, 2) == NAU_ERROR_INVALID);
    const struct nau_lane twice[] = {{NAU_LOWPASS_CUTOFF, &cutoff, 1}, {NAU_LOWPASS_CUTOFF, &cutoff, 1}};
    assert(nau_player_set_track_effect_automation(p, lowpass, twice, 2) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_effect_automation(p, lowpass, twice, 0) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_effect_automation(p, lowpass, NULL, 1) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_SLOPE, 2) == NAU_ERROR_INVALID);
    float *wet = export_frames(p, "lowpass.wav", 6000);
    const double delay = lowpass_delay(400, 1.2);
    for (int64_t i = 0; i < 6000; i++) {
        close_to(wet[2 * i + 1], -wet[2 * i]);
        assert(fabsf(wet[2 * i]) < 0.2f);
        /* Open, once its start settled, and once opened again */
        if ((i >= 500 && i < 1000) || (i >= 3500 && i < 4000))
            close_to(wet[2 * i], dry[2 * i]);
        /* Under water, once the dive settled */
        if (i >= 1700 && i < 2000)
            lagged(wet[2 * i], i, delay);
    }

    /* Moved with its track, by any number of frames: the same */
    assert(nau_player_set_track_timings(p, t, 100, 0, -1) == NAU_OK);
    float *moved = export_frames(p, "lowpass.wav", 6000);
    for (int64_t i = 0; i < 100; i++)
        assert(moved[2 * i] == 0.f);
    for (int64_t i = 0; i + 100 < 6000; i++)
        assert(moved[2 * (i + 100)] == wet[2 * i] && moved[2 * (i + 100) + 1] == wet[2 * i + 1]);

    /* Removed */
    assert(nau_player_set_track_timings(p, t, 0, 0, -1) == NAU_OK);
    struct nau_track_effect *removed = lowpass;
    assert(nau_player_remove_track_effect(p, &lowpass) == NAU_OK && !lowpass);
    assert(nau_player_remove_track_effect(p, &removed) == NAU_ERROR_INVALID);
    assert(automate(p, removed, NAU_LOWPASS_CUTOFF, &cutoff, 1) == NAU_ERROR_INVALID);
    float *frames = export_frames(p, "lowpass.wav", 6000);
    assert(!memcmp(frames, dry, 6000 * 2 * sizeof(*dry)));
    free(dry);
    free(wet);
    free(moved);
    free(frames);
    nau_player_freep(&p);
}

/* The values set of a filter drive it on top of its automation */
static void test_lowpass_live(void)
{
    struct nau_player *p = offline_player(6000);
    struct nau_track *t = NULL;
    const struct nau_track_params params = {.filename = fixture, .duration = -1, .volume = 1};
    assert(nau_player_add_track(p, &params, &t) == NAU_OK);
    float *dry = export_frames(p, "lowpass_live.wav", 6000);

    /* Set before the automation, for it */
    struct nau_track_effect *lowpass = NULL;
    assert(nau_player_add_track_effect(p, t, NAU_EFFECT_LOWPASS, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, &lowpass) == NAU_OK);
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_AMOUNT, 0) == NAU_OK);
    const struct filter_ramp muffled = {.cutoff = 400, .resonance = 1};
    assert(set_filter(p, lowpass, &muffled, 1) == NAU_OK);
    float *frames = export_frames(p, "lowpass_live.wav", 6000);
    for (int64_t i = 500; i < 4000; i++)
        close_to(frames[2 * i], dry[2 * i]);
    free(frames);

    /* Half the octaves below open */
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_AMOUNT, 0.5f) == NAU_OK);
    float *half = export_frames(p, "lowpass_live.wav", 6000);
    const double delay = lowpass_delay(sqrt(0.49 * 48000 * 400), 1);
    for (int64_t i = 1000; i < 4000; i++)
        lagged(half[2 * i], i, delay);
    /* Kept through a change of slope and edits of the automation */
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_SLOPE, NAU_LOWPASS_12DB) == NAU_OK);
    assert(set_filter(p, lowpass, NULL, 0) == NAU_OK);
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_SLOPE, NAU_LOWPASS_24DB) == NAU_OK);
    assert(set_filter(p, lowpass, &muffled, 1) == NAU_OK);
    frames = export_frames(p, "lowpass_live.wav", 6000);
    assert(!memcmp(frames, half, 6000 * 2 * sizeof(*frames)));
    free(frames);

    /* The resonance at its highest, ringing when the media ends */
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_RESONANCE_SCALE, 100) == NAU_OK);
    frames = export_frames(p, "lowpass_live.wav", 6000);
    float ringing = 0;
    for (int64_t i = 4096; i < 4600; i++)
        ringing = fmaxf(ringing, fabsf(frames[2 * i] - half[2 * i]));
    assert(ringing > 0.05f);
    free(frames);

    /* The automation as it is */
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_AMOUNT, 1) == NAU_OK);
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_RESONANCE_SCALE, 1) == NAU_OK);
    frames = export_frames(p, "lowpass_live.wav", 6000);
    for (int64_t i = 1000; i < 4000; i++)
        lagged(frames[2 * i], i, lowpass_delay(400, 1));
    free(frames);

    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_AMOUNT, -1) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_AMOUNT, NAN) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_RESONANCE_SCALE, INFINITY) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_effect_param(p, lowpass, 99, 1) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_effect_param(NULL, lowpass, NAU_LOWPASS_AMOUNT, 1) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_effect_param(p, NULL, NAU_LOWPASS_AMOUNT, 1) == NAU_ERROR_INVALID);
    /* Gone with its track */
    assert(nau_player_remove_track(p, &t) == NAU_OK);
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_AMOUNT, 1) == NAU_ERROR_INVALID);
    assert(nau_player_remove_track_effect(p, &lowpass) == NAU_ERROR_INVALID);
    free(dry);
    free(half);
    nau_player_freep(&p);
}

/*
 * Edited while playing, the filter keeps its memory: no jump is heard, its
 * delay changing. Edits failing while another is pending leave it as it was.
 */
static void test_lowpass_edits(void)
{
    struct nau_player *p = NULL;
    const struct nau_player_config config = {.mode = NAU_PLAYER_HOST};
    p = nau_player_create();
    assert(p);
    assert(nau_player_init(p, &config) == NAU_OK);
    assert(nau_player_set_duration(p, 9600) == NAU_OK);
    struct nau_track *t = NULL;
    const struct nau_track_params params = {.filename = fixture, .duration = -1, .volume = 1};
    assert(nau_player_add_track(p, &params, &t) == NAU_OK);
    struct nau_track_effect *lowpass = NULL;
    assert(nau_player_add_track_effect(p, t, NAU_EFFECT_LOWPASS, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, &lowpass) == NAU_OK);
    const struct filter_ramp muffled = {.cutoff = 400, .resonance = 1};
    const struct nau_ramp opener = {.value = 500}, open = {.value = NAU_LOWPASS_OPEN};
    assert(set_filter(p, lowpass, &muffled, 1) == NAU_OK);

    assert(nau_player_start_rendering(p) == NAU_OK);
    const struct nau_ramp unity = {.value = 1};
    assert(nau_player_set_track_fader(p, t, &unity, 1) == NAU_OK);
    assert(automate(p, lowpass, NAU_LOWPASS_CUTOFF, &open, 1) == NAU_AGAIN);
    assert(automate(p, lowpass, NAU_LOWPASS_CUTOFF, NULL, 0) == NAU_AGAIN);
    assert(nau_player_move_track_effect(p, lowpass, NAU_TRACK_EFFECT_PLACEMENT_POST_FADER, 0) == NAU_AGAIN);
    struct nau_track_effect *saved = lowpass, *other = NULL;
    assert(nau_player_remove_track_effect(p, &lowpass) == NAU_AGAIN && lowpass == saved);
    assert(nau_player_add_track_effect(p, t, NAU_EFFECT_LOWPASS, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, &other) == NAU_AGAIN && !other);
    assert(nau_player_set_track_timings(p, t, 4800, 0, -1) == NAU_AGAIN);
    /* The values set are never pending */
    assert(nau_player_set_track_effect_param(p, lowpass, NAU_LOWPASS_AMOUNT, 1) == NAU_OK);

    /* About in real time, for the track to have its data once ready */
    assert(nau_player_play(p) == NAU_OK);
    static float output[2 * 9600];
    int64_t played = -1, edited = -1;
    while (nau_player_get_position(p) < 4600) {
        float block[2 * 256];
        const int64_t from = nau_player_get_position(p);
        const int ret = nau_player_render(p, 0, 256, block);
        const int64_t to = nau_player_get_position(p);
        assert(ret == NAU_OK || (ret == NAU_AGAIN && played < 0));
        /* Held while waiting for the track, then playing on */
        if (to > from) {
            memcpy(output + 2 * from, block + 2 * (256 - (to - from)), (size_t)(to - from) * 2 * sizeof(*block));
            if (played < 0)
                played = from;
        }
        if (to >= 2048 && edited < 0) {
            assert(automate(p, lowpass, NAU_LOWPASS_CUTOFF, &opener, 1) == NAU_OK);
            edited = to;
        }
        assert(played >= 0 || from == 0);
        nau_sleep_ms(5);
    }
    assert(played == 0 && edited >= 2048 && edited < 2500);
    for (int64_t i = 1; i < 4090; i++)
        assert(fabsf(output[2 * i] - output[2 * (i - 1)]) < 1e-3f);
    for (int64_t i = 1000; i < 2000; i++)
        lagged(output[2 * i], i, lowpass_delay(400, 1));
    for (int64_t i = edited + 500; i < 4000; i++)
        lagged(output[2 * i], i, lowpass_delay(500, 1));
    /* The media ended at 4096, the filter still sounding */
    assert(output[2 * 4100] > 0.1f);
    assert(nau_player_stop_rendering(p) == NAU_OK);
    nau_player_freep(&p);
}

/*
 * The effects of a track in their order, before its fader or after its
 * volume, added, moved and removed by their handles: passthroughs leave the
 * track as it is.
 */
static void test_effect_chain(void)
{
    struct nau_player *p = offline_player(6000);
    struct nau_track *t = NULL;
    const struct nau_track_params params = {.filename = fixture, .duration = -1, .volume = 1};
    assert(nau_player_add_track(p, &params, &t) == NAU_OK);
    const struct nau_ramp cut = {.frame = 2000, .value = 0};
    assert(nau_player_set_track_fader(p, t, &cut, 1) == NAU_OK);
    float *dry = export_frames(p, "chain.wav", 6000);

    struct nau_track_effect *pre = NULL, *post = NULL;
    assert(nau_player_add_track_effect(p, t, NAU_EFFECT_PASSTHROUGH, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, &pre) == NAU_OK);
    assert(nau_player_add_track_effect(p, t, NAU_EFFECT_PASSTHROUGH, NAU_TRACK_EFFECT_PLACEMENT_POST_FADER, &post) == NAU_OK);
    float *frames = export_frames(p, "chain.wav", 6000);
    assert(!memcmp(frames, dry, 6000 * 2 * sizeof(*frames)));
    free(frames);

    /* Moved within their placement, or to the other one */
    assert(nau_player_move_track_effect(p, pre, NAU_TRACK_EFFECT_PLACEMENT_POST_FADER, 2) == NAU_ERROR_INVALID);
    assert(nau_player_move_track_effect(p, pre, (enum nau_track_effect_placement)2, 0) == NAU_ERROR_INVALID);
    assert(nau_player_move_track_effect(p, NULL, NAU_TRACK_EFFECT_PLACEMENT_POST_FADER, 0) == NAU_ERROR_INVALID);
    assert(nau_player_move_track_effect(p, pre, NAU_TRACK_EFFECT_PLACEMENT_POST_FADER, 1) == NAU_OK);
    assert(nau_player_move_track_effect(p, post, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, 0) == NAU_OK);
    frames = export_frames(p, "chain.wav", 6000);
    assert(!memcmp(frames, dry, 6000 * 2 * sizeof(*frames)));
    free(frames);

    /* Without parameters, nothing to set */
    const struct nau_ramp ramp = {.value = 1};
    const struct nau_lane lane = {0, &ramp, 1};
    assert(nau_player_set_track_effect_param(p, pre, 0, 1) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_effect_automation(p, pre, &lane, 1) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_effect_automation(p, pre, &lane, 0) == NAU_ERROR_INVALID);
    assert(nau_player_set_track_effect_automation(p, pre, NULL, 1) == NAU_ERROR_INVALID);

    /* Removed by its handle, wherever it moved */
    struct nau_track_effect *removed = pre;
    assert(nau_player_remove_track_effect(p, &pre) == NAU_OK && !pre);
    assert(nau_player_remove_track_effect(p, &removed) == NAU_ERROR_INVALID);
    assert(nau_player_move_track_effect(p, removed, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, 0) == NAU_ERROR_INVALID);
    frames = export_frames(p, "chain.wav", 6000);
    assert(!memcmp(frames, dry, 6000 * 2 * sizeof(*frames)));
    free(frames);

    /* Up to 16 effects per track */
    struct nau_track_effect *effects[16] = {post};
    for (int i = 1; i < 16; i++) {
        const enum nau_track_effect_placement placement = i % 2 ? NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER : NAU_TRACK_EFFECT_PLACEMENT_POST_FADER;
        assert(nau_player_add_track_effect(p, t, NAU_EFFECT_PASSTHROUGH, placement, &effects[i]) == NAU_OK);
    }
    struct nau_track_effect *extra = NULL;
    assert(nau_player_add_track_effect(p, t, NAU_EFFECT_PASSTHROUGH, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, &extra) == NAU_ERROR_UNSUPPORTED && !extra);
    assert(nau_player_add_track_effect(p, t, (enum nau_effect_type)-1, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, &extra) == NAU_ERROR_INVALID && !extra);
    assert(nau_player_add_track_effect(p, t, NAU_EFFECT_PASSTHROUGH, (enum nau_track_effect_placement)2, &extra) == NAU_ERROR_INVALID && !extra);
    assert(nau_player_add_track_effect(p, NULL, NAU_EFFECT_PASSTHROUGH, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, &extra) == NAU_ERROR_INVALID && !extra);
    assert(nau_player_add_track_effect(p, t, NAU_EFFECT_PASSTHROUGH, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, NULL) == NAU_ERROR_INVALID);
    frames = export_frames(p, "chain.wav", 6000);
    assert(!memcmp(frames, dry, 6000 * 2 * sizeof(*frames)));
    free(frames);
    /* Gone with the track */
    assert(nau_player_clear_tracks(p) == NAU_OK);
    assert(nau_player_move_track_effect(p, effects[3], NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, 0) == NAU_ERROR_INVALID);
    free(dry);
    nau_player_freep(&p);
}

/*
 * A filter before a fader cutting the track is cut with it; after the volume,
 * it rings on.
 */
static void test_lowpass_placement(void)
{
    struct nau_player *p = offline_player(6000);
    struct nau_track *t = NULL;
    const struct nau_track_params params = {.filename = fixture, .duration = -1, .volume = 1};
    assert(nau_player_add_track(p, &params, &t) == NAU_OK);
    const struct nau_ramp cut = {.frame = 2000, .value = 0};
    assert(nau_player_set_track_fader(p, t, &cut, 1) == NAU_OK);

    struct nau_track_effect *lowpass = NULL;
    assert(nau_player_add_track_effect(p, t, NAU_EFFECT_LOWPASS, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, &lowpass) == NAU_OK);
    const struct nau_ramp muffled = {.value = 400};
    assert(automate(p, lowpass, NAU_LOWPASS_CUTOFF, &muffled, 1) == NAU_OK);
    float *pre = export_frames(p, "placement.wav", 6000);
    for (int64_t i = 1000; i < 2000; i++)
        lagged(pre[2 * i], i, lowpass_delay(400, NAU_LOWPASS_FLAT));
    for (int64_t i = 2000; i < 6000; i++)
        assert(pre[2 * i] == 0.f);

    /* After the volume: the same until the cut, then ringing */
    assert(nau_player_move_track_effect(p, lowpass, NAU_TRACK_EFFECT_PLACEMENT_POST_FADER, 0) == NAU_OK);
    float *post = export_frames(p, "placement.wav", 6000);
    assert(!memcmp(post, pre, 2000 * 2 * sizeof(*post)));
    assert(fabsf(post[2 * 2010]) > 0.01f);
    free(pre);
    free(post);
    nau_player_freep(&p);
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    fixture = argv[1];
    fixture_index = argv[2];
    output_dir = argv[3];
    fixture_gap = argv[4];
    test_time_limits();
    test_pending_edits();
    test_export();
    test_export_gap();
    test_mix();
    test_lowpass();
    test_lowpass_live();
    test_lowpass_edits();
    test_lowpass_placement();
    test_effect_chain();
    test_never_waits();
    test_position_while_rendering();
    test_edits_while_paused();
    test_host_stop();
    test_volume_while_paused();
    test_add_past_track();
    test_heard_from_jump();
    test_device();
    return 0;
}
