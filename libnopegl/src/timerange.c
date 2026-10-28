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

#include "log.h"
#include "timerange.h"

struct ngli_timerange ngli_timerange_sanitize(double start, double end)
{
    if (start < 0.0) {
        LOG(DEBUG, "start time %g is negative, evaluated as 0", start);
        start = 0.0;
    }
    if (end < 0.0) {
        end = INFINITY;
    } else if (end < start) {
        LOG(DEBUG, "end time %g is before start time %g, the range is empty", end, start);
        end = start;
    }
    return (struct ngli_timerange){start, end};
}

bool ngli_timerange_contains(const struct ngli_timerange *range, double t)
{
    return t >= range->start && t < range->end;
}
