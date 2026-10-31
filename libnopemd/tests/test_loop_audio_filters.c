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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nopemd.h>

#include "playback_utils.h"

/*
 * Regroup samples into frames that cross loop boundaries. Each pass must
 * keep the same samples and starting timestamp as the unfiltered output,
 * without padding at the boundary.
 */

#define LOOP_START  0.5
#define LOOP_END    1.2
#define NB_PASSES   5
#define MAX_SAMPLES (1 << 18)

struct pass {
    float *samples; // interleaved stereo
    int nb_samples;
    double first_ts;
};

/*
 * Decoded frames can overlap the loop start and cause small backward
 * timestamp jumps. Only a jump larger than half the loop duration is a wrap.
 */
static int read_passes(const char *filename, const struct pb_player_opts *opts, const char *filters,
                       struct pass *passes)
{
    struct nmd_ctx *s = pb_create_player(filename, opts);
    if (!s)
        return -1;
    if (filters)
        nmd_set_option(s, "filters", filters);
    nmd_set_loop_range(s, LOOP_START, LOOP_END);

    int ret = -1;
    int pass = -1;
    double prev_ts = 0.0;
    for (;;) {
        struct nmd_frame *frame = NULL;
        const int got = nmd_get_next_frame(s, &frame);
        if (got != NMD_RET_NEWFRAME) {
            fprintf(stderr, "  %s: next frame in pass %d: %d\n", filters, pass, got);
            goto end;
        }
        if (pass < 0 || frame->ts < prev_ts - (LOOP_END - LOOP_START) / 2) {
            if (++pass == NB_PASSES) {
                nmd_frame_releasep(&frame);
                break;
            }
            passes[pass].first_ts = frame->ts;
        }
        prev_ts = frame->ts;

        struct pass *p = &passes[pass];
        if (p->nb_samples + frame->nb_samples > MAX_SAMPLES) {
            fprintf(stderr, "  %s: too many samples in pass %d\n", filters, pass);
            nmd_frame_releasep(&frame);
            goto end;
        }
        memcpy(p->samples + 2 * p->nb_samples, frame->datap[0], 2 * frame->nb_samples * sizeof(*p->samples));
        p->nb_samples += frame->nb_samples;
        nmd_frame_releasep(&frame);
    }
    ret = 0;

end:
    nmd_freep(&s);
    return ret;
}

static int test_filters(const char *filename, const struct pb_player_opts *opts, const char *filters)
{
    int ret = -1;
    struct pass ref[NB_PASSES] = {0};
    struct pass out[NB_PASSES] = {0};
    for (int i = 0; i < NB_PASSES; i++) {
        ref[i].samples = calloc(2 * MAX_SAMPLES, sizeof(*ref[i].samples));
        out[i].samples = calloc(2 * MAX_SAMPLES, sizeof(*out[i].samples));
        if (!ref[i].samples || !out[i].samples)
            goto end;
    }

    if (read_passes(filename, opts, NULL, ref) < 0 || read_passes(filename, opts, filters, out) < 0)
        goto end;

    ret = 0;
    for (int i = 0; i < NB_PASSES; i++) {
        if (out[i].nb_samples != ref[i].nb_samples || out[i].first_ts != ref[i].first_ts) {
            fprintf(stderr, "  pass %d: %d samples from %g s instead of %d from %g s\n",
                    i, out[i].nb_samples, out[i].first_ts, ref[i].nb_samples, ref[i].first_ts);
            ret = -1;
        } else if (memcmp(out[i].samples, ref[i].samples, 2 * ref[i].nb_samples * sizeof(*ref[i].samples))) {
            fprintf(stderr, "  pass %d: samples differ\n", i);
            ret = -1;
        }
    }

end:
    printf("%s: %s\n", filters, ret < 0 ? "FAIL" : "OK");
    for (int i = 0; i < NB_PASSES; i++) {
        free(ref[i].samples);
        free(out[i].samples);
    }
    return ret;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <media> [<use_pkt_duration>]\n", argv[0]);
        return -1;
    }
    const struct pb_player_opts opts = {
        .audio = 1,
        .use_pkt_duration = argc > 2 ? atoi(argv[2]) : 0,
        .start_time = -1.0,
        .end_time = -1.0,
    };
    int ret = 0;
    if (test_filters(argv[1], &opts, "asetnsamples=n=1500") < 0)
        ret = -1;
    if (test_filters(argv[1], &opts, "asetnsamples=n=64") < 0)
        ret = -1;
    return ret;
}
