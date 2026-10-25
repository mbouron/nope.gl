/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

#ifndef PLAYBACK_UTILS_H
#define PLAYBACK_UTILS_H

#include <stdatomic.h>
#include <stdint.h>

#include <nopemd.h>

#define MAX_FRAMES 8192
#define MAX_SEEKS 8192

struct pb_player_opts {
    int audio;
    int use_pkt_duration;
    double start_time;          // -1 if unset
    double end_time;            // -1 if unset
    int queue_size;             // size of the queues, the default if 0
};

/* Software player recording its seeks, logging with TEST_LOG set */
struct nmd_ctx *pb_create_player(const char *filename, const struct pb_player_opts *opts);

/* Seeks of the players since nb_seeks was reset, and the time of each */
extern atomic_int nb_seeks;
extern double seek_times[MAX_SEEKS];

uint64_t pb_hash_frame(const struct nmd_frame *frame, int audio);

/* Frames of a sequential decoding of the media */
struct ref_frame {
    int64_t pts;
    uint64_t hash;
};

extern struct ref_frame refs[MAX_FRAMES];
extern int nb_refs;
extern int timebase[2];

int pb_load_refs(const char *filename, int audio);
int pb_find_ref(uint64_t hash);
double pb_get_ref_time(int idx);

#endif
