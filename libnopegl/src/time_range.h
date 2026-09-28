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

#ifndef TIME_RANGE_H
#define TIME_RANGE_H

#include "log.h"
#include "utils/utils.h"

/*
 * The range [start, end) a node is active over, from the bounds a client set,
 * which are kept as they are: the start is not negative and the end is not
 * before it (the range is then empty); a negative end means forever. The same
 * applies at init and on a live change, so a client can move one bound past
 * the other on its way to a new range.
 */
static inline void ngli_time_range_get_effective(double start, double end, double *dst_start, double *dst_end)
{
    if (start < 0.0) {
        LOG(DEBUG, "start time %g is negative, evaluated as 0", start);
        start = 0.0;
    }
    if (end >= 0.0 && end < start) {
        LOG(DEBUG, "end time %g is before start time %g, the range is empty", end, start);
        end = start;
    }
    *dst_start = start;
    *dst_end = end;
}

#endif
