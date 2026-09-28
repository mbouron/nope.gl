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

#include "log.h"

static inline void ngli_timerange_sanitize(double start, double end, double *dst_start, double *dst_end)
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
