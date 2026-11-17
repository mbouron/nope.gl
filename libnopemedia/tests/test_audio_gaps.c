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

/*
 * Audio keeps its timestamps around seeks and loop passes. The media has
 * three 10 ms frames, at 300, 310 and 800 ms.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <nopemedia/nopemedia.h>

static struct nmd_ctx *open_media(const char *filename, int use_pkt_duration, double start)
{
    struct nmd_ctx *s = nmd_create(filename);
    if (!s)
        return NULL;
    nmd_set_option(s, "auto_hwaccel", 0);
    nmd_set_option(s, "avselect", NMD_SELECT_AUDIO);
    nmd_set_option(s, "audio_texture", 0);
    nmd_set_option(s, "use_pkt_duration", use_pkt_duration);
    /* A filter graph, kept across the loop passes */
    nmd_set_option(s, "filters", "aformat=sample_fmts=flt:channel_layouts=stereo:sample_rates=48000");
    if (nmd_seek(s, start) < 0 || nmd_start(s) < 0)
        nmd_freep(&s);
    return s;
}

static int next_frame(struct nmd_ctx *s, struct nmd_frame **frame)
{
    int ret;
    do {
        ret = nmd_get_next_frame(s, frame);
    } while (ret == NMD_RET_UNCHANGED);
    return ret;
}

/* A seek before the audio starts: its first frame keeps its own time */
static int test_late_start(const char *filename, int use_pkt_duration)
{
    struct nmd_ctx *s = open_media(filename, use_pkt_duration, 0.1);
    if (!s)
        return -1;
    struct nmd_frame *frame = NULL;
    int ret = next_frame(s, &frame);
    if (ret != NMD_RET_NEWFRAME || fabs(frame->ts - 0.3) > 1e-6) {
        fprintf(stderr, "late start: got %d at %f instead of a frame at 0.3\n", ret, frame ? frame->ts : -1.);
        ret = -1;
    } else {
        ret = 0;
    }
    nmd_frame_releasep(&frame);
    nmd_freep(&s);
    return ret;
}

/* A loop starting in the middle of a frame: each pass has that frame, at its own time */
static int test_loop_from_frame(const char *filename, int use_pkt_duration)
{
    struct nmd_ctx *s = open_media(filename, use_pkt_duration, 0.315);
    if (!s)
        return -1;
    int ret = nmd_set_loop_range(s, 0.315, 0.55);
    for (int i = 0; ret >= 0 && i < 3; i++) {
        struct nmd_frame *frame = NULL;
        ret = next_frame(s, &frame);
        if (ret != NMD_RET_NEWFRAME || fabs(frame->ts - 0.31) > 1e-6) {
            fprintf(stderr, "pass %d: got %d at %f instead of the frame at 0.31\n", i, ret, frame ? frame->ts : -1.);
            ret = -1;
        }
        nmd_frame_releasep(&frame);
    }
    nmd_freep(&s);
    return ret < 0 ? ret : 0;
}

/*
 * A loop over a range holding no audio, the frame before it included: its
 * passes are empty and the playback ends
 */
static int test_loop_without_audio(const char *filename, int use_pkt_duration)
{
    struct nmd_ctx *s = open_media(filename, use_pkt_duration, 0.33);
    if (!s)
        return -1;
    int ret = nmd_set_loop_range(s, 0.33, 0.55);
    if (ret >= 0) {
        struct nmd_frame *frame = NULL;
        ret = next_frame(s, &frame);
        if (ret != NMD_ERR_EOF) {
            fprintf(stderr, "loop without audio: got %d at %f instead of the end\n", ret, frame ? frame->ts : -1.);
            ret = -1;
        } else {
            ret = 0;
        }
        nmd_frame_releasep(&frame);
    }
    nmd_freep(&s);
    return ret;
}

int main(int ac, char **av)
{
    if (ac < 2) {
        fprintf(stderr, "Usage: %s <audio_gaps.mka> [<use_pkt_duration>]\n", av[0]);
        return -1;
    }
    const char *filename = av[1];
    const int use_pkt_duration = ac > 2 ? atoi(av[2]) : 0;
    if (test_late_start(filename, use_pkt_duration) < 0 ||
        test_loop_from_frame(filename, use_pkt_duration) < 0 ||
        test_loop_without_audio(filename, use_pkt_duration) < 0)
        return -1;
    return 0;
}
