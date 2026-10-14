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

#ifndef TIMERANGE_H
#define TIMERANGE_H

#include <stdbool.h>

/* Time range [start, end), unbounded if end is INFINITY */
struct ngli_timerange {
    double start;
    double end;
};

struct ngli_timerange ngli_timerange_sanitize(double start, double end);
bool ngli_timerange_contains(const struct ngli_timerange *range, double t);

struct ngli_prefetch_range {
    struct ngli_timerange update;
    double render_time;
};

void ngli_timerange_filter_prefetch(struct ngli_prefetch_range *range, const struct ngli_timerange *filter_range,
                                    double render_time);
bool ngli_timerange_get_prefetch_time(const struct ngli_prefetch_range *range,
                                     double t, const struct ngli_timerange *loop,
                                     double *time, double *delay);
bool ngli_timerange_is_active(const struct ngli_timerange *range, double t, double prefetch_time,
                              const struct ngli_timerange *loop);

struct ngli_timerange_filter {
    struct ngli_timerange range;
    double render_time;
    double prefetch_time;
    int updated;
};

void ngli_timerange_filter_init(struct ngli_timerange_filter *s, double start, double end,
                                double render_time, double prefetch_time);
bool ngli_timerange_filter_visit(struct ngli_timerange_filter *s, double t, bool is_active,
                                 bool child_is_active, const struct ngli_timerange *loop,
                                 struct ngli_prefetch_range *child_range);
bool ngli_timerange_filter_update(struct ngli_timerange_filter *s, double *t);

#endif
