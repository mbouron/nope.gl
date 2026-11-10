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
#include <stdbool.h>

#include "nopeaudio/nopeaudio.h"
#include "ramp.h"
#include "utils/utils.h"

/*
 * Limit dB ramps to a 60 dB curve so fades to or from silence have a finite
 * shape: 10^(60 / 20) = 1000. The endpoints are still reached exactly.
 */
#define DB_RANGE 1000.

float nau_ramp_volume(enum nau_ramp_shape shape, float curvature, float from, float to, float progress)
{
    float p = fminf(fmaxf(progress, 0.f), 1.f);
    if (p == 0.f || from == to)
        return from;
    if (p == 1.f)
        return to;
    /* The curvature raises the progress to a power, from 1/8 to 8 */
    if (curvature != 0.f)
        p = powf(p, exp2f(-3.f * curvature));
    switch (shape) {
    case NAU_RAMP_DB: {
        /*
         * A ramp linear in dB is from * r^p with r = to / from, which is also
         * from + (to - from) * (r^p - 1) / (r - 1).
         *
         * The second form still starts at `from` and ends at `to` when r is
         * not their ratio. This is used to limit r to 60 dB: two gains
         * further apart (silence, or a gain close to it) follow the curve of
         * a 60 dB ramp, and reach their ends without a jump.
         *
         * Computed in double precision: the ratio of two floats may not fit
         * in a float.
         */
        const double r = from > 0.f ? fmin(fmax((double)to / from, 1. / DB_RANGE), DB_RANGE)
                       : DB_RANGE;
        const double l = log(r);
        return (float)(from + ((double)to - from) * (expm1(l * p) / expm1(l)));
    }
    case NAU_RAMP_EQUAL_POWER:
        /* A rising ramp follows a sine, a falling one a cosine */
        p = to >= from ? sinf(p * NAU_HALF_PI_F32) : 1.f - cosf(p * NAU_HALF_PI_F32);
        break;
    case NAU_RAMP_S_CURVE:
        p = p * p * (3.f - 2.f * p);
        break;
    default:
        break;
    }
    return from + (to - from) * p;
}

float nau_ramp_param(const struct nau_param_info *param, enum nau_ramp_shape shape, float curvature,
                     float from, float to, float progress)
{
    if (!param)
        return from;

    if (progress <= 0.f)
        return from;
    if (progress >= 1.f)
        return to;
    switch (param->scale) {
    case NAU_PARAM_LOG:
        /* Along the logarithm of the value, where dB are linear */
        if (shape == NAU_RAMP_DB)
            shape = NAU_RAMP_LINEAR;
        return exp2f(nau_ramp_volume(shape, curvature, log2f(from), log2f(to), progress));
    case NAU_PARAM_STEPPED:
        if (shape == NAU_RAMP_DB)
            shape = NAU_RAMP_LINEAR;
        return roundf(nau_ramp_volume(shape, curvature, from, to, progress));
    default:
        return nau_ramp_volume(shape, curvature, from, to, progress);
    }
}

bool nau_ramps_valid(const struct nau_ramp *ramps, uint32_t count)
{
    if (count && !ramps)
        return false;

    if (count > NAU_MAX_RAMPS)
        return false;

    for (uint32_t i = 0; i < count; i++) {
        const struct nau_ramp *k = &ramps[i];
        if (k->frame < 0 || k->length < 0)
            return false;

        if (k->frame > INT64_MAX - k->length)
            return false;

        if (i && k->frame < ramps[i - 1].frame)
            return false;

        if (!isfinite(k->value))
            return false;

        if ((uint32_t)k->shape > NAU_RAMP_S_CURVE)
            return false;

        if (!(k->curvature >= -1.f && k->curvature <= 1.f))
            return false;
    }
    return true;
}

uint32_t nau_ramps_started(const struct nau_ramp *ramps, uint32_t count, int64_t frame)
{
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (ramps[mid].frame <= frame)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
