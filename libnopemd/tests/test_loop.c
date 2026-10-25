/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

/*
 * Request looping playback at 60 Hz and compare each displayed frame with
 * a reference decoded in order. Count seeks reported by the callback: normal
 * loop repetitions should not add any, while startup and range changes may.
 * Internal seeks used to prepare the next repetition are not reported.
 * Test times are relative to the first media frame, which may start after 0.
 */

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/time.h>
#include <nopemd.h>

#include "playback_utils.h"

#define STEP (1.0 / 60.0)

static double media_start;

struct test {
    const char *name;
    double start_time;          // media options, -1 if unset
    double end_time;
    double loop_start;
    double loop_end;
    double t0;                  // first time requested
    double duration;            // time played
    int nb_seeks;               // seeks expected
    /* Optional change of the loop range during the playback */
    double change_time;
    double new_loop_start;
    double new_loop_end;
    int max_seeks;              // seeks allowed with the change
    /* Optional jump, followed by forward playback without wrapping */
    double seek_time;
    double seek_target;         // time to jump to, `seek_time` if unset
    /* Optional loop range the player is started with before the playback */
    double prefetch_loop_start;
    double prefetch_loop_end;
    int queue_size;             // size of the player queues, the default if 0
};

/* Frame displayed at the media time `t`, rounded to the stream time base */
static int get_ref(double t)
{
    const int64_t pts = llround(t * timebase[1] / timebase[0]);
    int idx = 0;
    for (int i = 0; i < nb_refs; i++)
        if (refs[i].pts <= pts)
            idx = i;
    return idx;
}

static double get_media_time(const struct test *test, double t)
{
    const double mt = (test->start_time > 0.0 ? test->start_time : 0.0) + t;
    return test->end_time >= 0.0 && mt > test->end_time ? test->end_time : mt;
}

/*
 * Find the last frame before the exclusive loop end `le`. Requests just below
 * `le` must still show this frame even if rounding to microseconds reaches `le`.
 * A loop extending beyond the `end_time` option includes the frame held there.
 */
static int get_loop_end_ref(const struct test *test, double offset, double le)
{
    const int64_t start = test->start_time >= 0.0 ? llrint((test->start_time + media_start) * 1e6) : 0;
    int64_t end = start + llrint((le + offset) * 1e6);
    const int64_t den = (int64_t)timebase[0] * 1000000;
    int64_t end_pts = (end * timebase[1] + den - 1) / den;
    if (test->end_time >= 0.0) {
        /* Include the frame at `end_time` after rounding that time to stream ticks. */
        const int64_t end_time = llrint((test->end_time + media_start) * 1e6);
        if (end > end_time)
            end_pts = llrint(end_time * (double)timebase[1] / den) + 1;
    }
    int idx = 0;
    for (int i = 0; i < nb_refs; i++)
        if (refs[i].pts < end_pts)
            idx = i;
    return idx;
}

/* Time requested at playback time `T`, looping over `[ls, le)` once `T` reaches `le` */
static double get_loop_time(double T, double ls, double le)
{
    if (le <= ls || T < le)
        return T;
    const double t = ls + fmod(T - ls, le - ls);
    return t < le ? t : ls;
}

static int run_test(const char *filename, int audio, int use_pkt_duration, const struct test *test)
{
    /* The requested times are offset by the start of the media, unless the `start_time` option is set */
    const int skip = test->start_time >= 0.0;
    const double offset = skip ? 0.0 : media_start;
    const struct pb_player_opts opts = {
        .audio            = audio,
        .use_pkt_duration = use_pkt_duration,
        .start_time       = skip ? test->start_time + media_start : -1.0,
        .end_time         = test->end_time >= 0.0 ? test->end_time + media_start : -1.0,
        .queue_size       = test->queue_size,
    };
    struct nmd_ctx *s = pb_create_player(filename, &opts);
    if (!s)
        return -1;

    /* With no `start_time` option, the first request at a nonzero media start needs a seek. */
    const int nb_start_seeks = !skip && test->t0 == 0.0 && offset > 0.0;

    nb_seeks = 0;
    /* Queue frames under the old range before changing it and requesting playback. */
    if (test->prefetch_loop_end > 0.0) {
        nmd_set_loop_range(s, test->prefetch_loop_start + offset, test->prefetch_loop_end + offset);
        nmd_start(s);
        av_usleep(200000);
    }

    nmd_set_loop_range(s, test->loop_start + offset, test->loop_end + offset);

    double ls = test->loop_start, le = test->loop_end;
    double origin = 0.0;    // playback time of the last loop range change
    int changed = 0, seeked = 0;
    int displayed = -1, nb_frames = 0, nb_errors = 0;
    const int nb_steps = (int)lrint(test->duration / STEP);

    for (int i = 0; i <= nb_steps; i++) {
        const double T = test->t0 + i * STEP;

        if (test->change_time > 0.0 && !changed && T >= test->change_time) {
            /* Preserve the current requested time, then wrap it using the new range. */
            const double t = get_loop_time(T - origin, ls, le);
            nmd_set_loop_range(s, test->new_loop_start + offset, test->new_loop_end + offset);
            ls = test->new_loop_start;
            le = test->new_loop_end;
            origin = T - t;
            changed = 1;
        }

        double req = get_loop_time(T - origin + offset, ls + offset, le + offset);
        if (test->seek_time > 0.0 && T >= test->seek_time) {
            if (!seeked) {
                origin = T - (test->seek_target > 0.0 ? test->seek_target : test->seek_time);
                seeked = 1;
            }
            req = T - origin + offset;
        }
        const double t = req - offset;

        struct nmd_frame *frame = NULL;
        int ret = nmd_get_frame(s, req, &frame);
        int expected = get_ref(media_start + get_media_time(test, t));
        const int looping = llrint((le + offset) * 1e6) > llrint((ls + offset) * 1e6);
        if (looping && req >= ls + offset && req < le + offset && expected > get_loop_end_ref(test, offset, le))
            expected = get_loop_end_ref(test, offset, le);
        if (ret == NMD_RET_NEWFRAME) {
            const uint64_t hash = pb_hash_frame(frame, audio);
            nb_frames++;
            displayed = pb_find_ref(hash);
            if (hash != refs[expected].hash && nb_errors++ < 10)
                fprintf(stderr, "  t=%.4f: got frame %d (ts=%.4f) instead of %d (ts=%.4f)\n",
                        t, displayed, frame->ts, expected, pb_get_ref_time(expected));
            nmd_frame_releasep(&frame);
        } else if (ret == NMD_RET_UNCHANGED) {
            if ((displayed < 0 || refs[displayed].hash != refs[expected].hash) && nb_errors++ < 10)
                fprintf(stderr, "  t=%.4f: frame %d (ts=%.4f) kept instead of %d (ts=%.4f)\n",
                        t, displayed, pb_get_ref_time(displayed), expected, pb_get_ref_time(expected));
        } else {
            if (nb_errors++ < 10)
                fprintf(stderr, "  t=%.4f: unexpected return %d\n", t, ret);
        }
    }
    nmd_freep(&s);

    const int min_seeks = test->nb_seeks + nb_start_seeks;
    const int range_changed = test->change_time > 0.0 || test->prefetch_loop_end > 0.0;
    const int max_seeks = (range_changed ? test->max_seeks : test->nb_seeks) + nb_start_seeks;
    const int seeks_ok = range_changed ? nb_seeks >= min_seeks && nb_seeks <= max_seeks
                                                 : nb_seeks == min_seeks;
    printf("%-24s %s: %d frames, %d errors, %d seeks (expected %d)\n",
           test->name, nb_errors || !seeks_ok ? "FAIL" : "OK", nb_frames, nb_errors, nb_seeks, max_seeks);
    return nb_errors || !seeks_ok ? -1 : 0;
}

static const struct test video_tests[] = {
    {"whole media",       -1, -1, 0.0,  2.0,  0.0, 7.0,  0},
    {"key frame start",   -1, -1, 0.5,  1.5,  0.0, 4.0,  0},
    {"mid gop",           -1, -1, 0.61, 1.37, 0.0, 4.0,  0},
    {"short",             -1, -1, 0.27, 0.41, 0.0, 2.0,  0},
    /* Starts with packets without decoding time in Matroska */
    {"short at start",    -1, -1, 0.038, 0.125, 0.0, 1.0, 0},
    /* Starts within the frames displayed before an open GOP key frame (0.5) */
    {"leading frames",    -1, -1, 0.45, 0.9,  0.0, 2.0,  0},
    {"seek leading frames",-1, -1, 0.0, 0.0,  0.45, 0.1, 1},
    {"shorter than frame",-1, -1, 0.71, 0.72, 0.0, 1.5,  0},
    /* An empty range after rounding to microseconds must still return the first frame. */
    {"submicrosecond",    -1, -1, 0.0,  0.0000004, 0.0, 0.05, 0},
    {"submicrosecond trim",0.3, 1.8, 0.0, 0.0000004, 0.0, 0.05, 1},
    {"past the end",      -1, -1, 1.2,  5.0,  1.0, 4.0,  1},
    {"start/end time",    0.3, 1.8, 0.2, 1.1, 0.0, 4.0,  1},
    {"past end time",     0.3, 1.0, 0.4, 1.2, 0.0, 3.0,  1},
    /* End time between two frames, rounded to the second one in the Matroska time base (ms) */
    {"past end time between", 0.3, 1.4669, 0.4, 1.6, 0.0, 3.0, 1},
    {"start at loop",     -1, -1, 0.61, 1.37, 0.61, 4.0, 1},
    {"change",            -1, -1, 0.5,  1.5,  0.0, 5.0,  0, 1.8, 0.2, 0.9, 1},
    {"extend",            -1, -1, 0.5,  1.0,  0.0, 5.0,  0, 1.2, 0.5, 1.6, 1},
    {"disable",           -1, -1, 0.5,  1.5,  0.0, 3.0,  0, 1.7, 0.0, 0.0, 1},
    {"seek out",          -1, -1, 0.5,  1.0,  0.0, 3.0,  1, .seek_time = 2.0},
    /* Playing forward from past the end of the loop range, without wrapping */
    {"start past loop",   -1, -1, 0.1,  0.6,  0.9, 0.7,  1, .seek_time = 0.9},
    /* Keep playing forward after enabling a loop behind the current time */
    {"enable behind",     -1, -1, 0.0,  0.0,  0.0, 1.6,  0, 0.9, 0.1, 0.6, 1, .seek_time = 0.9},
    /* Jump past EOF with frames of the previous loop range still queued */
    {"disable past eof",  -1, -1, 0.693, 1.375, 0.968, 0.1, 1, 1.0, 0.0, 0.0, 2, .seek_time = 1.0, .seek_target = 2.282},
    {"extend past eof",   -1, -1, 0.693, 1.375, 0.968, 0.1, 1, 1.0, 0.693, 3.0, 2, .seek_time = 1.0, .seek_target = 2.282},
    {"disable after eof", -1, -1, 0.693, 3.0, 1.0, 1.7, 1, 2.6, 0.0, 0.0, 2, .seek_time = 2.6, .seek_target = 3.2},
    /* Change the range while holding the last media frame, with the next pass already queued. */
    {"change after eof",  -1, -1, 0.693, 3.0, 1.0, 1.4, 1, 2.2, 0.693, 2.9, 1},
    /* Changes while several passes of a short loop range are decoded ahead */
    {"extend short",      -1, -1, 0.5,  0.6,  0.4, 3.0,  1, 1.0, 0.5, 1.2, 2},
    {"prefetch extend",   -1, -1, 0.5,  1.0,  0.4, 3.0,  0, .max_seeks = 1, .prefetch_loop_start = 0.5, .prefetch_loop_end = 0.6},
    {"prefetch shorten",  -1, -1, 0.6,  0.8,  0.4, 3.0,  0, .max_seeks = 1, .prefetch_loop_start = 0.2, .prefetch_loop_end = 1.5},
    {"prefetch disable",  -1, -1, 0.0,  0.0,  0.4, 1.5,  0, .max_seeks = 1, .prefetch_loop_start = 0.5, .prefetch_loop_end = 0.6},
    /* Extended with many passes of a loop range of two frames queued ahead */
    {"deep queues",       -1, -1, 0.5,  0.55, 0.4, 3.0,  1, 1.0, 0.5, 0.7, 2, .queue_size = 100},
};

static const struct test audio_tests[] = {
    {"audio",             -1, -1, 10.0, 12.5, 9.0, 8.0,  1},
};

static const struct test media_tests[] = {
    {"long gop",          -1, -1, 3.3,  5.1,  3.0, 6.0,  1},
};

/*
 * A range entirely before the first media frame cannot produce a loop pass.
 * Requests there should still return the first frame, as normal playback does.
 */
static int run_test_before_start(const char *filename)
{
    struct nmd_ctx *s = pb_create_player(filename, &(const struct pb_player_opts){.start_time = -1.0, .end_time = -1.0});
    if (!s)
        return -1;

    const double end = media_start / 2.0;
    nmd_set_loop_range(s, 0.0, end);

    int nb_errors = 0, nb_frames = 0;
    for (int i = 0; i < 60; i++) {
        struct nmd_frame *frame = NULL;
        const double t = get_loop_time(i * STEP, 0.0, end);
        int ret = nmd_get_frame(s, t, &frame);
        if (ret == NMD_RET_NEWFRAME) {
            nb_frames++;
            if (pb_hash_frame(frame, 0) != refs[0].hash && nb_errors++ < 10)
                fprintf(stderr, "  t=%.4f: got frame ts=%.4f instead of the first one\n", t, frame->ts);
            nmd_frame_releasep(&frame);
        } else if (ret != NMD_RET_UNCHANGED || !nb_frames) {
            if (nb_errors++ < 10)
                fprintf(stderr, "  t=%.4f: unexpected return %d\n", t, ret);
        }
    }
    nmd_freep(&s);

    printf("%-24s %s: %d frames, %d errors\n", "before start", nb_errors ? "FAIL" : "OK", nb_frames, nb_errors);
    return nb_errors ? -1 : 0;
}

/*
 * Start the loop before the first media frame, then shorten it during a later
 * repetition. Requests before the first frame must keep showing that frame;
 * all other requests must still match the reference at the requested time.
 */
static int run_test_shrink_from_start(const char *filename)
{
    struct nmd_ctx *s = pb_create_player(filename, &(const struct pb_player_opts){.start_time = -1.0, .end_time = -1.0});
    if (!s)
        return -1;

    double le = pb_get_ref_time(nb_refs - 1);
    const double change_time = 2.0 * le - 0.3;
    nmd_set_loop_range(s, 0.0, le);

    int changed = 0, displayed = -1, nb_frames = 0, nb_errors = 0;
    double origin = 0.0;
    const int nb_steps = (int)lrint(4.0 * le / STEP);
    for (int i = 0; i <= nb_steps; i++) {
        const double T = i * STEP;
        if (!changed && T >= change_time) {
            const double t = get_loop_time(T, 0.0, le);
            le -= 0.2;
            nmd_set_loop_range(s, 0.0, le);
            origin = T - t;
            changed = 1;
        }
        const double t = get_loop_time(T - origin, 0.0, le);
        const struct test raw = {.start_time = -1.0, .end_time = -1.0};
        const int end_ref = get_loop_end_ref(&raw, 0.0, le);
        int expected = get_ref(t);
        if (expected > end_ref)
            expected = end_ref;

        struct nmd_frame *frame = NULL;
        int ret = nmd_get_frame(s, t, &frame);
        if (ret == NMD_RET_NEWFRAME) {
            const uint64_t hash = pb_hash_frame(frame, 0);
            nb_frames++;
            displayed = pb_find_ref(hash);
            if (hash != refs[expected].hash && nb_errors++ < 10)
                fprintf(stderr, "  t=%.4f: got frame %d (ts=%.4f) instead of %d (ts=%.4f)\n",
                        t, displayed, frame->ts, expected, pb_get_ref_time(expected));
            nmd_frame_releasep(&frame);
        } else if (ret != NMD_RET_UNCHANGED || displayed < 0 || refs[displayed].hash != refs[expected].hash) {
            if (nb_errors++ < 10)
                fprintf(stderr, "  t=%.4f: frame %d kept (return %d) instead of %d (ts=%.4f)\n",
                        t, displayed, ret, expected, pb_get_ref_time(expected));
        }
    }
    nmd_freep(&s);

    printf("%-24s %s: %d frames, %d errors\n", "shrink from start", nb_errors ? "FAIL" : "OK", nb_frames, nb_errors);
    return nb_errors ? -1 : 0;
}

int main(int ac, char **av)
{
    if (ac < 3) {
        fprintf(stderr, "Usage: %s <media> <video|audio|media> [<use_pkt_duration>]\n", av[0]);
        return -1;
    }

    const char *filename = av[1];
    const int audio = !strcmp(av[2], "audio");
    const int use_pkt_duration = ac > 3 ? atoi(av[3]) : 0;
    const struct test *tests = video_tests;
    size_t nb_tests = sizeof(video_tests) / sizeof(*video_tests);
    if (audio) {
        tests = audio_tests;
        nb_tests = sizeof(audio_tests) / sizeof(*audio_tests);
    } else if (!strcmp(av[2], "media")) {
        tests = media_tests;
        nb_tests = sizeof(media_tests) / sizeof(*media_tests);
    }

    if (pb_load_refs(filename, audio) < 0)
        return -1;
    media_start = pb_get_ref_time(0);

    int ret = 0;
    const char *only = getenv("TEST_LOOP_ONLY");
    if (tests == video_tests && media_start > 0.0 && (!only || !strcmp(only, "before start")) &&
        run_test_before_start(filename) < 0)
        ret = -1;
    if (tests == video_tests && media_start > 0.0 && (!only || !strcmp(only, "shrink from start")) &&
        run_test_shrink_from_start(filename) < 0)
        ret = -1;
    for (size_t i = 0; i < nb_tests; i++)
        if ((!only || !strcmp(only, tests[i].name)) && run_test(filename, audio, use_pkt_duration, &tests[i]) < 0)
            ret = -1;
    return ret;
}
