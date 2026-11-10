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

#ifndef NAU_TRANSPORT_CLOCK_H
#define NAU_TRANSPORT_CLOCK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "nopeaudio/nopeaudio.h"

#define NAU_CLOCK_HISTORY_SIZE 256

/* A continuous segment of the playback clock, see transport_clock.c. */
struct nau_clock_entry {
    _Atomic int64_t position;  /* of the first frame */
    _Atomic int64_t time;      /* time the first frame is heard, 0 if unknown */
    _Atomic int64_t end_time;  /* time the run ends */
    _Atomic uint32_t frames;   /* number of frames in the run */
    _Atomic int playing;       /* 0 if the position stays during the run */
};

struct nau_transport_clock {
    uint32_t sample_rate;          /* constant once initialized */

    /* Published by the renderer */
    _Atomic uint32_t sequence;     /* odd during a write */
    _Atomic uint32_t count;        /* entries recorded, capped to the size */
    _Atomic uint32_t write;        /* index of the next entry written */
    struct nau_clock_entry entries[NAU_CLOCK_HISTORY_SIZE];

    /* Render thread only */
    bool ready;                    /* the fields below are set */
    double next_time;              /* time expected for the next render */
    double period;                 /* estimated nanoseconds per frame */
};

/*
 * State for one render call. The sole writer keeps local copies of the
 * sequence and ring indices.
 */
struct nau_clock_writer {
    uint32_t sequence;
    uint32_t next;
    uint32_t count;
    bool merge;      /* an entry was written by this call, and can be extended */
    int64_t time;    /* smoothed time of the call, 0 if unknown */
    double period;
};

void nau_transport_clock_init(struct nau_transport_clock *c, uint32_t sample_rate);

/* Render thread */
struct nau_clock_writer nau_transport_clock_begin(struct nau_transport_clock *c, int64_t time, uint32_t frames);
void nau_transport_clock_record(struct nau_transport_clock *c, struct nau_clock_writer *writer,
                                uint32_t offset, uint32_t frames, int64_t position, int playing);
void nau_transport_clock_clear(struct nau_transport_clock *c);

/* Any thread */
int nau_transport_clock_get(const struct nau_transport_clock *c, struct nau_clock *clock);
int nau_transport_clock_get_position_at(const struct nau_transport_clock *c, int64_t time,
                                        int64_t *position);

#endif
