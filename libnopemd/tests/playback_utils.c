/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "playback_utils.h"

double seek_times[MAX_SEEKS];
atomic_int nb_seeks;

struct ref_frame refs[MAX_FRAMES];
int nb_refs;
int timebase[2];

[[gnu::format(printf, 6, 0)]]
static void log_callback(void *arg, int level, const char *filename, int ln,
                         const char *fn, const char *fmt, va_list vl)
{
    if (!getenv("TEST_LOG"))
        return;
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    fprintf(stderr, "[%s:%d %s] %s\n", filename, ln, fn, buf);
}

static void seek_callback(void *arg, double t)
{
    const int i = atomic_fetch_add(&nb_seeks, 1);
    if (i < MAX_SEEKS)
        seek_times[i] = t;
}

struct nmd_ctx *pb_create_player(const char *filename, const struct pb_player_opts *opts)
{
    struct nmd_ctx *s = nmd_create(filename);
    if (!s)
        return NULL;
    nmd_set_log_callback(s, NULL, log_callback);
    nmd_set_seek_callback(s, NULL, seek_callback);
    nmd_set_option(s, "auto_hwaccel", 0);
    nmd_set_option(s, "avselect", opts->audio ? NMD_SELECT_AUDIO : NMD_SELECT_VIDEO);
    nmd_set_option(s, "audio_texture", 0);
    nmd_set_option(s, "use_pkt_duration", opts->use_pkt_duration);
    if (opts->start_time >= 0.0)
        nmd_set_option(s, "start_time", opts->start_time);
    if (opts->end_time >= 0.0)
        nmd_set_option(s, "end_time", opts->end_time);
    if (opts->queue_size) {
        nmd_set_option(s, "max_nb_packets", opts->queue_size);
        nmd_set_option(s, "max_nb_frames", opts->queue_size);
        nmd_set_option(s, "max_nb_sink", opts->queue_size);
    }
    return s;
}

uint64_t pb_hash_frame(const struct nmd_frame *frame, int audio)
{
    const int nb_lines = audio ? 1 : frame->height;
    const size_t size = audio ? (size_t)frame->nb_samples * 2 * sizeof(float) : (size_t)frame->width * 4;
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    for (int y = 0; y < nb_lines; y++)
        XXH3_64bits_update(&state, frame->datap[0] + (ptrdiff_t)y * frame->linesizep[0], size);
    return XXH3_64bits_digest(&state);
}

int pb_load_refs(const char *filename, int audio)
{
    const struct pb_player_opts opts = {.audio = audio, .use_pkt_duration = 1, .start_time = -1.0, .end_time = -1.0};
    struct nmd_ctx *s = pb_create_player(filename, &opts);
    if (!s)
        return -1;
    for (;;) {
        struct nmd_frame *frame = NULL;
        int ret = nmd_get_next_frame(s, &frame);
        if (ret != NMD_RET_NEWFRAME)
            break;
        if (nb_refs == MAX_FRAMES) {
            nmd_frame_releasep(&frame);
            break;
        }
        refs[nb_refs++] = (struct ref_frame){.pts = frame->pts, .hash = pb_hash_frame(frame, audio)};
        nmd_frame_releasep(&frame);
    }

    struct nmd_info info;
    int ret = nmd_get_info(s, &info);
    nmd_freep(&s);
    if (ret < 0)
        return -1;
    timebase[0] = info.timebase[0];
    timebase[1] = info.timebase[1];
    printf("%d reference frames\n", nb_refs);
    return nb_refs > 0 ? 0 : -1;
}

int pb_find_ref(uint64_t hash)
{
    for (int i = 0; i < nb_refs; i++)
        if (refs[i].hash == hash)
            return i;
    return -1;
}

double pb_get_ref_time(int idx)
{
    return idx < 0 ? -1.0 : refs[idx].pts * timebase[0] / (double)timebase[1];
}
