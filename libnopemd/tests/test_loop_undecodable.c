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

#include "playback_utils.h"

/*
 * The VCL NAL units of the GOP [1s, 1.5s) were changed to the unspecified
 * type 48, which the software decoder skips without an error. A loop pass
 * within this GOP produces no frame and must end playback, as when filters
 * remove every frame of a pass.
 */

static int expect_frame(struct nmd_ctx *s, double t, int expected)
{
    struct nmd_frame *frame = NULL;
    const int ret = nmd_get_frame(s, t, &frame);
    const int ok = ret == expected && !!frame == (expected == NMD_RET_NEWFRAME);
    if (!ok)
        fprintf(stderr, "  frame at %g: got %d, expected %d\n", t, ret, expected);
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

/* A loop range without any decodable frame ends playback; a new range and seek restore it */
static int test_range(const char *filename, const struct pb_player_opts *opts, double start, double end)
{
    struct nmd_ctx *s = pb_create_player(filename, opts);
    if (!s)
        return -1;
    nmd_set_loop_range(s, start, end);

    int ret = -1;
    if (expect_frame(s, start, NMD_ERR_EOF) < 0 ||
        expect_frame(s, start, NMD_ERR_EOF) < 0 ||
        expect_frame(s, end - 0.1, NMD_ERR_EOF) < 0)
        goto end;

    nmd_set_loop_range(s, 0.5, 1.0);
    nmd_seek(s, 0.5);
    if (expect_ts(s, 0.5, 0.5) < 0 ||
        expect_ts(s, 0.9, 0.9) < 0 ||
        expect_ts(s, 0.5, 0.5) < 0)
        goto end;
    ret = 0;

end:
    printf("loop range [%g, %g): %s\n", start, end, ret < 0 ? "FAIL" : "OK");
    nmd_freep(&s);
    return ret;
}

/* Sequential decoding returns the frames before the loop range, then stops */
static int test_next_frame(const char *filename, const struct pb_player_opts *opts)
{
    int ret = pb_load_refs(filename, 0);
    if (ret < 0)
        return ret;
    int nb_frames = 0;
    while (nb_frames < nb_refs && pb_get_ref_time(nb_frames) < 1.0)
        nb_frames++;

    struct nmd_ctx *s = pb_create_player(filename, opts);
    if (!s)
        return -1;
    nmd_set_loop_range(s, 1.0, 1.5);

    int count = 0;
    for (;;) {
        struct nmd_frame *frame = NULL;
        ret = nmd_get_next_frame(s, &frame);
        nmd_frame_releasep(&frame);
        if (ret != NMD_RET_NEWFRAME)
            break;
        count++;
    }
    const int ok = ret == NMD_ERR_EOF && count == nb_frames;
    if (!ok)
        fprintf(stderr, "  got %d frames ending with %d, expected %d frames\n", count, ret, nb_frames);
    printf("next frames: %s\n", ok ? "OK" : "FAIL");
    nmd_freep(&s);
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <media> [<use_pkt_duration>]\n", argv[0]);
        return -1;
    }

    const struct pb_player_opts opts = {
        .use_pkt_duration = argc > 2 ? atoi(argv[2]) : 0,
        .start_time = -1.0,
        .end_time = -1.0,
    };
    int ret = 0;
    if (test_range(argv[1], &opts, 1.0, 1.5) < 0)
        ret = -1;
    if (test_range(argv[1], &opts, 1.1, 1.4) < 0)
        ret = -1;
    if (test_next_frame(argv[1], &opts) < 0)
        ret = -1;
    return ret;
}
