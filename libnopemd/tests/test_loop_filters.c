/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavutil/time.h>

#include "playback_utils.h"

static int expect_frame(struct nmd_ctx *s, int next, int expected)
{
    struct nmd_frame *frame = NULL;
    const int ret = next ? nmd_get_next_frame(s, &frame) : nmd_get_frame(s, 0.0, &frame);
    const int ok = ret == expected && !!frame == (expected == NMD_RET_NEWFRAME);
    if (!ok)
        fprintf(stderr, "  %s: got %d, expected %d\n", next ? "next frame" : "frame at 0", ret, expected);
    nmd_frame_releasep(&frame);
    return ok ? 0 : -1;
}

static int expect_ts(struct nmd_ctx *s, double t, double ts)
{
    struct nmd_frame *frame = NULL;
    const int ret = nmd_get_frame(s, t, &frame);
    const int ok = ret == NMD_RET_NEWFRAME && fabs(frame->ts - ts) < 0.001;
    if (!ok)
        fprintf(stderr, "  frame at %g: got %d (ts=%g), expected ts=%g\n", t, ret, frame ? frame->ts : -1.0, ts);
    nmd_frame_releasep(&frame);
    return ok ? 0 : -1;
}

/* A full pass with no filtered output ends playback; a new range and seek can restore it. */
static int test_empty(const char *filename, const struct pb_player_opts *opts, const char *filters, double end)
{
    struct nmd_ctx *s = pb_create_player(filename, opts);
    if (!s)
        return -1;
    nmd_set_option(s, "filters", filters);
    nmd_set_loop_range(s, 0.0, end);

    int ret = -1;
    if (expect_frame(s, 0, NMD_ERR_EOF) < 0 ||
        expect_frame(s, 0, NMD_ERR_EOF) < 0 ||
        expect_frame(s, 1, NMD_ERR_EOF) < 0)
        goto end;

    nmd_set_loop_range(s, 0.0, 0.0);
    nmd_seek(s, 0.0);
    if (expect_frame(s, 0, NMD_RET_NEWFRAME) < 0)
        goto end;

    nmd_set_loop_range(s, 0.0, end);
    nmd_seek(s, 0.0);
    if (expect_frame(s, 0, NMD_ERR_EOF) < 0)
        goto end;

    nmd_set_loop_range(s, 0.0, 2.0);
    nmd_seek(s, 0.0);
    for (int i = 0; i < 6; i++)
        if (expect_frame(s, 1, NMD_RET_NEWFRAME) < 0)
            goto end;
    ret = 0;

end:
    printf("%s: %s\n", filters, ret < 0 ? "FAIL" : "OK");
    nmd_freep(&s);
    return ret;
}

/*
 * The filter only keeps frames at or after 1s, so [0s, 0.5s) has no output.
 * Extend the range to 2s or disable looping, then request frames at 1.5s
 * without calling `nmd_seek()`. Playback must recover under the new settings.
 *
 * With `delay < 0`, wait for the empty range to stop playback first. Otherwise,
 * change the range after `delay` microseconds of background decoding. `shown`
 * also checks recovery when a frame was returned before the empty range.
 */
static int change_range(const char *filename, const struct pb_player_opts *opts, int shown, int delay, double new_end)
{
    struct nmd_ctx *s = pb_create_player(filename, opts);
    if (!s)
        return -1;
    nmd_set_option(s, "filters", "select=gte(t\\,1)");

    int ret = -1;
    if (shown) {
        nmd_set_loop_range(s, 0.0, 2.0);
        if (expect_ts(s, 1.0, 1.0) < 0)
            goto end;
    }

    nmd_set_loop_range(s, 0.0, 0.5);
    if (delay < 0) {
        struct nmd_frame *frame = NULL;
        const int got = nmd_get_frame(s, 0.25, &frame);
        nmd_frame_releasep(&frame);
        if (got != (shown ? NMD_RET_UNCHANGED : NMD_ERR_EOF)) {
            fprintf(stderr, "  frame at 0.25: got %d, expected the end of the playback\n", got);
            goto end;
        }
    } else {
        nmd_start(s);
        av_usleep(delay);
    }

    nmd_set_loop_range(s, 0.0, new_end);
    if (expect_ts(s, 1.5, 1.5) < 0 || expect_ts(s, 1.75, 52 / 30.0) < 0)
        goto end;
    ret = 0;

end:
    if (ret < 0)
        fprintf(stderr, "  range change (shown:%d delay:%d end:%g) failed\n", shown, delay, new_end);
    nmd_freep(&s);
    return ret;
}

static int test_change(const char *filename, const struct pb_player_opts *opts)
{
    int ret = 0;
    if (change_range(filename, opts, 0, -1, 2.0) < 0)
        ret = -1;
    if (change_range(filename, opts, 0, -1, 0.0) < 0)
        ret = -1;
    if (change_range(filename, opts, 1, -1, 2.0) < 0)
        ret = -1;
    printf("range change after the end: %s\n", ret < 0 ? "FAIL" : "OK");

    /* Change the range at different points while the old range is being decoded. */
    int nb_errors = 0;
    for (int delay = 0; delay <= 10000; delay += 250)
        if (change_range(filename, opts, 0, delay, 2.0) < 0)
            nb_errors++;
    printf("range change during the empty passes: %s\n", nb_errors ? "FAIL" : "OK");
    return ret < 0 || nb_errors ? -1 : 0;
}

/*
 * Seeking to 1.4s skips all frames accepted by the filter in the initial pass.
 * The next full repetition of [0s, 1.5s) still produces frames, so continue.
 */
static int test_partial(const char *filename, const struct pb_player_opts *opts)
{
    struct nmd_ctx *s = pb_create_player(filename, opts);
    if (!s)
        return -1;
    nmd_set_option(s, "filters", "select=lt(t\\,1)");
    nmd_set_loop_range(s, 0.0, 1.5);
    nmd_seek(s, 1.4);

    int ret = 0;
    for (int i = 0; i < 40; i++) {
        if (expect_frame(s, 1, NMD_RET_NEWFRAME) < 0) {
            ret = -1;
            break;
        }
    }
    printf("empty initial pass: %s\n", ret < 0 ? "FAIL" : "OK");
    nmd_freep(&s);
    return ret;
}

/* The fps filter can hold the only output frame until the graph is drained at the wrap. */
static int test_flush(const char *filename, const struct pb_player_opts *opts)
{
    struct nmd_ctx *s = pb_create_player(filename, opts);
    if (!s)
        return -1;
    nmd_set_option(s, "filters", "fps=1");
    nmd_set_loop_range(s, 0.0, 0.6);

    int ret = 0;
    for (int i = 0; i < 4; i++) {
        if (expect_frame(s, 1, NMD_RET_NEWFRAME) < 0) {
            ret = -1;
            break;
        }
    }
    printf("output on flush: %s\n", ret < 0 ? "FAIL" : "OK");
    nmd_freep(&s);
    return ret;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return -1;
    const struct pb_player_opts opts = {
        .use_pkt_duration = argc > 2 ? atoi(argv[2]) : 0,
        .start_time = -1.0,
        .end_time = -1.0,
    };
    int ret = 0;
    if (test_empty(argv[1], &opts, "fps=1", 0.1) < 0)
        ret = -1;
    if (test_empty(argv[1], &opts, "select=gte(t\\,1)", 0.5) < 0)
        ret = -1;
    if (test_partial(argv[1], &opts) < 0)
        ret = -1;
    if (test_flush(argv[1], &opts) < 0)
        ret = -1;
    if (test_change(argv[1], &opts) < 0)
        ret = -1;
    return ret;
}
