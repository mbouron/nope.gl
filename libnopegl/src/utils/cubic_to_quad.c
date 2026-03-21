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

#include "cubic_to_quad.h"

/*
 * Compute the exact maximum approximation error when representing cubic bezier
 * P0,P1,P2,P3 as a single quadratic with control point:
 *  Q = (3*(P1 + P2) - P0 - P3) / 4
 *
 * The error curve E(t) = Cubic(t) - Quadratic(t) simplifies to:
 *   E(t) = (d / 2) * t * (1 - t) * (1 - 2t)
 * where d = -P0 + 3*P1 - 3*P2 + P3 (per component).
 *
 * The maximum of |t(1-t)(1-2t)| on [0,1] occurs at t = (3 ± sqrt(3)) / 6 and
 * equals sqrt(3) / 18. Thus:
 *   max|E| = |d| * sqrt(3) / 36 ~= 0.048112522432468815
 *
 * Each midpoint subdivision divides d by 8, so the error scales by 1/8 per
 * subdivision level.
 */
static float compute_max_error(const struct point cubic[4])
{
    const float dx = -cubic[0].x + 3.f * cubic[1].x - 3.f * cubic[2].x + cubic[3].x;
    const float dy = -cubic[0].y + 3.f * cubic[1].y - 3.f * cubic[2].y + cubic[3].y;
    const float k = 0.048112522432468815f;
    return sqrtf(dx * dx + dy * dy) * k;
}

int cubic_compute_subdivision_count(const struct point cubic[4], float tolerance)
{
    if (tolerance <= 0.f)
        return 0;

    float error = compute_max_error(cubic);

    int count;
    for (count = 0; count < CUBIC_MAX_SUBDIVISION_COUNT; count++) {
        if (error <= tolerance)
            break;
        error *= 0.125f;
    }

    return count;
}

static struct point *cubic_subdivide(const struct point *cubic, struct point *points, int level)
{
    if (level == 0) {
        *points++ = (struct point){
            (3.f * (cubic[1].x + cubic[2].x) - cubic[0].x - cubic[3].x) / 4.f,
            (3.f * (cubic[1].y + cubic[2].y) - cubic[0].y - cubic[3].y) / 4.f,
        };
        *points++ = cubic[3];
        return points;
    }

    /* Split at t=0.5 using de Casteljau */
    const struct point m01  = {(cubic[0].x + cubic[1].x) * .5f, (cubic[0].y + cubic[1].y) * .5f};
    const struct point m12  = {(cubic[1].x + cubic[2].x) * .5f, (cubic[1].y + cubic[2].y) * .5f};
    const struct point m23  = {(cubic[2].x + cubic[3].x) * .5f, (cubic[2].y + cubic[3].y) * .5f};
    const struct point m012 = {(m01.x + m12.x) * .5f, (m01.y + m12.y) * .5f};
    const struct point m123 = {(m12.x + m23.x) * .5f, (m12.y + m23.y) * .5f};
    const struct point mid  = {(m012.x + m123.x) * .5f, (m012.y + m123.y) * .5f};

    const struct point left[4]  = {cubic[0], m01, m012, mid};
    const struct point right[4] = {mid, m123, m23, cubic[3]};

    level--;
    points = cubic_subdivide(left, points, level);
    return cubic_subdivide(right, points, level);
}

static int cubic_split_into_quadratics(const struct point *cubic, struct point *points, int count)
{
    *points = cubic[0];

    cubic_subdivide(cubic, points + 1, count);

    return 1 << count;
}

void cubic_converter_to_quadratics(struct cubic_converter *converter, const struct point *cubic, float tolerance)
{
    int count = cubic_compute_subdivision_count(cubic, tolerance);
    converter->quadratic_count = cubic_split_into_quadratics(cubic, converter->quadratics, count);
}
