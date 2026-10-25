/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <nopemd.h>

#include "playback_utils.h"

static int use_pkt_duration;

static struct nmd_ctx *create_player(const char *filename)
{
    const struct pb_player_opts opts = {.use_pkt_duration = use_pkt_duration, .start_time = -1.0, .end_time = -1.0};
    return pb_create_player(filename, &opts);
}

static int run_test(const char *filename, const char *name, int halfway)
{
    struct nmd_ctx *s = create_player(filename);
    if (!s)
        return -1;

    nb_seeks = 0;
    int nb_errors = 0;
    for (int i = nb_refs - 1; i >= 0; i--) {
        const double frame_time = pb_get_ref_time(i);
        const double next_time = i < nb_refs - 1 ? pb_get_ref_time(i + 1) : frame_time;
        const double t = halfway ? (frame_time + next_time) / 2.0 : frame_time;
        struct nmd_frame *frame = NULL;
        int ret = nmd_get_frame(s, t, &frame);
        if (ret != NMD_RET_NEWFRAME) {
            if (nb_errors++ < 10)
                fprintf(stderr, "  t=%.4f: unexpected return %d\n", t, ret);
            continue;
        }
        const uint64_t hash = pb_hash_frame(frame, 0);
        if (hash != refs[i].hash && nb_errors++ < 10) {
            const int got = pb_find_ref(hash);
            fprintf(stderr, "  t=%.4f: got frame %d (ts=%.4f) instead of %d (ts=%.4f)\n",
                    t, got, frame->ts, i, frame_time);
        }
        nmd_frame_releasep(&frame);
    }
    nmd_freep(&s);

    /* Every request but the first seeks back */
    const int seeks_ok = nb_seeks >= nb_refs - 1;
    printf("%-10s %s: %d requests, %d errors, %d seeks\n", name, nb_errors || !seeks_ok ? "FAIL" : "OK",
           nb_refs, nb_errors, nb_seeks);
    return nb_errors || !seeks_ok ? -1 : 0;
}

static int run_test_seek_ahead(const char *filename)
{
    int nb_errors = 0;
    for (int i = 0; i < nb_refs - 1; i += 7) {
        struct nmd_ctx *s = create_player(filename);
        if (!s)
            return -1;
        nmd_seek(s, pb_get_ref_time(nb_refs - 1));
        nmd_start(s);

        const double t = pb_get_ref_time(i);
        struct nmd_frame *frame = NULL;
        int ret = nmd_get_frame(s, t, &frame);
        if (ret != NMD_RET_NEWFRAME) {
            if (nb_errors++ < 10)
                fprintf(stderr, "  t=%.4f: unexpected return %d\n", t, ret);
        } else {
            const uint64_t hash = pb_hash_frame(frame, 0);
            if (hash != refs[i].hash && nb_errors++ < 10)
                fprintf(stderr, "  t=%.4f: got frame %d (ts=%.4f) instead of %d (ts=%.4f)\n",
                        t, pb_find_ref(hash), frame->ts, i, pb_get_ref_time(i));
            nmd_frame_releasep(&frame);
        }
        nmd_freep(&s);
    }
    printf("%-10s %s: %d errors\n", "seek ahead", nb_errors ? "FAIL" : "OK", nb_errors);
    return nb_errors ? -1 : 0;
}

static int run_test_seek_before_start(const char *filename)
{
    struct nmd_ctx *s = create_player(filename);
    if (!s)
        return -1;

    int nb_errors = 0;
    const int mid = nb_refs / 2;
    const double mid_time = pb_get_ref_time(mid);
    const double before_start = pb_get_ref_time(0) / 2.0;
    const double times[] = {mid_time, 0.0, mid_time, before_start};
    const int expected[] = {mid, 0, mid, 0};
    for (size_t i = 0; i < sizeof(times) / sizeof(*times); i++) {
        struct nmd_frame *frame = NULL;
        int ret = nmd_get_frame(s, times[i], &frame);
        if (ret != NMD_RET_NEWFRAME) {
            if (nb_errors++ < 10)
                fprintf(stderr, "  t=%.4f: unexpected return %d\n", times[i], ret);
            continue;
        }
        const uint64_t hash = pb_hash_frame(frame, 0);
        if (hash != refs[expected[i]].hash && nb_errors++ < 10)
            fprintf(stderr, "  t=%.4f: got frame %d (ts=%.4f) instead of %d (ts=%.4f)\n",
                    times[i], pb_find_ref(hash), frame->ts, expected[i], pb_get_ref_time(expected[i]));
        nmd_frame_releasep(&frame);
    }
    nmd_freep(&s);

    printf("%-10s %s: %d errors\n", "before start", nb_errors ? "FAIL" : "OK", nb_errors);
    return nb_errors ? -1 : 0;
}

/*
 * A stop cancels a pending seek: the playback starts again from the start of
 * the media, without seeking
 */
static int run_test_stop_after_seek(const char *filename, int started)
{
    struct nmd_ctx *s = create_player(filename);
    if (!s)
        return -1;

    int nb_errors = 0;
    if (started) {
        struct nmd_frame *frame = NULL;
        nmd_get_frame(s, pb_get_ref_time(0), &frame);
        nmd_frame_releasep(&frame);
    }
    const double seek_time = pb_get_ref_time(nb_refs - 1);
    nmd_seek(s, seek_time);
    nmd_stop(s);

    nb_seeks = 0;
    struct nmd_frame *frame = NULL;
    int ret = nmd_get_frame(s, 0.0, &frame);
    if (ret != NMD_RET_NEWFRAME) {
        nb_errors++;
        fprintf(stderr, "  t=0: unexpected return %d\n", ret);
    } else {
        const uint64_t hash = pb_hash_frame(frame, 0);
        if (hash != refs[0].hash) {
            nb_errors++;
            fprintf(stderr, "  t=0: got frame %d (ts=%.4f) instead of 0 (ts=%.4f)\n",
                    pb_find_ref(hash), frame->ts, pb_get_ref_time(0));
        }
        nmd_frame_releasep(&frame);
    }
    nmd_freep(&s);

    /*
     * The seek of a playing player can be reported after the stop: only count
     * the seeks to an earlier time
     */
    int seeks = 0;
    for (int i = 0; i < nb_seeks && i < MAX_SEEKS; i++)
        if (!started || seek_times[i] < seek_time - 1e-3)
            seeks++;

    printf("%-10s %s: %d errors, %d seeks\n", started ? "stop" : "stop idle",
           nb_errors || seeks ? "FAIL" : "OK", nb_errors, seeks);
    return nb_errors || seeks ? -1 : 0;
}

int main(int ac, char **av)
{
    if (ac < 2) {
        fprintf(stderr, "Usage: %s <media> [<use_pkt_duration>]\n", av[0]);
        return -1;
    }

    const char *filename = av[1];
    use_pkt_duration = ac > 2 ? atoi(av[2]) : 0;
    if (pb_load_refs(filename, 0) < 0 || nb_refs < 2)
        return -1;

    int ret = 0;
    if (run_test(filename, "frames", 0) < 0)
        ret = -1;
    if (run_test(filename, "halfway", 1) < 0)
        ret = -1;
    if (run_test_seek_ahead(filename) < 0)
        ret = -1;
    if (pb_get_ref_time(0) > 0.0 && run_test_seek_before_start(filename) < 0)
        ret = -1;
    if (run_test_stop_after_seek(filename, 1) < 0)
        ret = -1;
    if (run_test_stop_after_seek(filename, 0) < 0)
        ret = -1;
    return ret;
}
