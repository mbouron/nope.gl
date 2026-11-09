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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <nopemedia/nopemedia.h>

/*
 * A player whose time range changes during playback returns the frames a
 * player opened with the new range returns, at the same requested times.
 */

struct range {
    double start;
    double end;
};

enum preparation {
    BEFORE_START,
    AFTER_PREFETCH,
    DURING_PLAYBACK,
};

static const struct test {
    const char *name;
    struct range initial;
    struct range changed;
    double change_at;  // requested time at which the range changes
    double play_until; // requested time playback goes on to after the change
} tests[] = {
    {"later start and end",      {0.5, 1.5}, {0.7, 1.8},  0.5, 1.2},
    {"earlier start",            {1.0, -1.0}, {0.5, -1.0}, 0.3, 1.0},
    {"range before old start",   {1.0, 1.8}, {0.3, 0.8},  0.2, 0.7},
    {"end grows after its end",  {0.0, 0.8}, {0.0, 1.8},  1.0, 1.6},
    {"end shrinks",              {0.0, 1.8}, {0.0, 0.8},  0.4, 1.4},
    {"no end",                   {0.3, 0.9}, {0.3, -1.0}, 0.8, 1.6},
};

static struct nmd_ctx *create(const char *filename, int use_pkt_duration, const struct range *range)
{
    struct nmd_ctx *s = nmd_create(filename);
    if (!s)
        return NULL;
    if (nmd_set_option(s, "auto_hwaccel", 0) < 0 ||
        nmd_set_option(s, "use_pkt_duration", use_pkt_duration) < 0 ||
        nmd_set_option(s, "start_time", range->start) < 0 ||
        nmd_set_option(s, "end_time", range->end) < 0) {
        nmd_freep(&s);
        return NULL;
    }
    return s;
}

/* The timestamp of the frame shown at t, keeping the previous one if unchanged */
static int get_ts(struct nmd_ctx *s, double t, double *ts)
{
    struct nmd_frame *frame = NULL;
    const int ret = nmd_get_frame(s, t, &frame);
    if (ret < 0 && ret != NMD_ERR_EOF)
        return ret;
    if (frame)
        *ts = frame->ts;
    nmd_frame_releasep(&frame);
    return 0;
}

static int run_test(const char *filename, int use_pkt_duration, const struct test *test, enum preparation preparation)
{
    int ret = -1;
    struct nmd_ctx *live = create(filename, use_pkt_duration, &test->initial);
    struct nmd_ctx *ref = create(filename, use_pkt_duration, &test->changed);
    if (!live || !ref)
        goto end;

    double live_ts = NAN;
    if (preparation == AFTER_PREFETCH) {
        double duration;
        /* Fetching info waits for the start request to finish, without consuming a frame. */
        if (nmd_start(live) < 0 || nmd_get_duration(live, &duration) < 0)
            goto end;
    } else if (preparation == DURING_PLAYBACK) {
        for (double t = 0.0; t < test->change_at; t += 1.0 / 30.0) {
            if (get_ts(live, t, &live_ts) < 0)
                goto end;
        }
    }

    if (nmd_set_time_range(live, test->changed.start, test->changed.end) < 0)
        goto end;

    double ref_ts = NAN;
    const double start = preparation == DURING_PLAYBACK ? test->change_at : 0.0;
    for (double t = start; t < test->play_until; t += 1.0 / 30.0) {
        if (get_ts(live, t, &live_ts) < 0 || get_ts(ref, t, &ref_ts) < 0)
            goto end;
        if (live_ts != ref_ts) {
            fprintf(stderr, "%s: frame at %g s after the change, %g s instead of %g s\n",
                    test->name, t, live_ts, ref_ts);
            goto end;
        }
    }

    double live_duration, ref_duration;
    if (nmd_get_duration(live, &live_duration) < 0 || nmd_get_duration(ref, &ref_duration) < 0)
        goto end;
    if (live_duration != ref_duration) {
        fprintf(stderr, "%s: duration %g s instead of %g s\n", test->name, live_duration, ref_duration);
        goto end;
    }
    ret = 0;

end:
    static const char * const preparations[] = {"before start", "after prefetch", "during playback"};
    printf("%s (%s): %s\n", test->name, preparations[preparation], ret < 0 ? "FAIL" : "OK");
    nmd_freep(&live);
    nmd_freep(&ref);
    return ret;
}

int main(int ac, char **av)
{
    if (ac < 2) {
        fprintf(stderr, "Usage: %s <media> [<use_pkt_duration>]\n", av[0]);
        return -1;
    }

    const int use_pkt_duration = ac > 2 ? atoi(av[2]) : 0;
    int ret = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(*tests); i++) {
        for (enum preparation preparation = BEFORE_START; preparation <= DURING_PLAYBACK; preparation++) {
            if (run_test(av[1], use_pkt_duration, &tests[i], preparation) < 0)
                ret = -1;
        }
    }
    return ret;
}
