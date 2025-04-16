/*
 * Copyright 2025 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2022 The Android Open Source Project
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
#include <stdint.h>
#include <string.h>

#include "conic.h"

static inline int32_t f32_to_bits(float x)
{
    const union { int32_t i; float f; } u = {.f = x};
    return u.i;
}

static inline bool f32_is_finite(float v)
{
    const int32_t bits = f32_to_bits(v);
    const int32_t f32_exponent_mask = 0x7F800000;
    return (bits & f32_exponent_mask) != f32_exponent_mask;
}

static bool f32_can_normalize(float dx, float dy)
{
    return (f32_is_finite(dx) && f32_is_finite(dy)) && (dx != 0.0 || dy != 0.0);
}

static bool f32_between(float a, float b, float c)
{
    return (a - b) * (c - b) <= 0.0f;
}

static bool points_are_equals(const struct point *p1, const struct point *p2)
{
    return !f32_can_normalize(p1->x - p2->x, p1->y - p2->y);
}

static bool points_are_finite(const struct point points[], int count)
{
    float prod = 0.0f;
    for (int i = 0; i < count; i++) {
        prod *= points[i].x;
        prod *= points[i].y;
    }
    return prod == 0.0f;
}

static bool point_is_finite(const struct point point)
{
    return points_are_finite(&point, 1);
}

int conic_compute_subdivision_count(const struct conic *conic, float tolerance)
{
    if (tolerance <= 0.0f || !f32_is_finite(tolerance) || !points_are_finite(conic->points, 3))
        return 0;

    const float a = conic->weight - 1.0f;
    const float k = a / (4.0f * (2.0f + a));
    const float x = k * (conic->points[0].x - 2.0f * conic->points[1].x + conic->points[2].x);
    const float y = k * (conic->points[0].y - 2.0f * conic->points[1].y + conic->points[2].y);

    float error = sqrtf(x * x + y * y);
    int count = 0;
    for ( ; count < CONIC_MAX_SUBDIVISION_COUNT; count++) {
        if (error <= tolerance)
            break;
        error *= 0.25f;
    }

    return count;
}

static void conic_split(const struct conic *conic, struct conic * __restrict dst)
{
    const float scale = 1.0f / (1.0f + conic->weight);
    const float new_weight = sqrtf(0.5f + conic->weight * 0.5f);

    const struct point p0 = conic->points[0];
    const struct point p1 = conic->points[1];
    const struct point p2 = conic->points[2];

    const struct point wp1 = {
        conic->weight * p1.x,
        conic->weight * p1.y,
    };
    const struct point m = {
        (p0.x + (wp1.x + wp1.x) + p2.x) * scale * 0.5f,
        (p0.y + (wp1.y + wp1.y) + p2.y) * scale * 0.5f,
    };
    struct point p = m;
    if (!point_is_finite(p)) {
        double w_d = conic->weight;
        double w_2 = w_d * 2.0;
        double scale_half = 1.0 / (1.0 + w_d) * 0.5;
        p.x = (float)(conic->points[0].x + w_2 * conic->points[1].x + conic->points[2].x * scale_half);
        p.y = (float)(conic->points[0].y + w_2 * conic->points[1].y + conic->points[2].y * scale_half);
    }

    dst[0].points[0] = conic->points[0];
    dst[0].points[1] = (struct point){(p0.x + wp1.x) * scale, (p0.y + wp1.y) * scale};
    dst[0].points[2] = p;
    dst[0].weight = new_weight;

    dst[1].points[0] = p;
    dst[1].points[1] = (struct point){(wp1.x + p2.x) * scale, (wp1.y + p2.y) * scale};
    dst[1].points[2] = conic->points[2];
    dst[1].weight = new_weight;
}

static struct point *conic_subdivide(const struct conic *src, struct point *points, int level)
{
    if (level == 0) {
        memcpy(points, &src->points[1], 2 * sizeof(struct point));
        return points + 2;
    }

    struct conic dst[2];
    conic_split(src, &dst[0]);
    const float start_y = src->points[0].y;
    const float end_y = src->points[2].y;
    if (f32_between(start_y, src->points[1].y, end_y)) {
        const float mid_y = dst[0].points[2].y;
        if (!f32_between(start_y, mid_y, end_y)) {
            const float closer_y = fabsf(mid_y - start_y) < fabsf(mid_y - end_y) ? start_y : end_y;
            dst[0].points[2].y = dst[1].points[0].y = closer_y;
        }
        if (!f32_between(start_y, dst[0].points[1].y, dst[0].points[2].y)) {
            dst[0].points[1].y = start_y;
        }
        if (!f32_between(dst[1].points[0].y, dst[1].points[1].y, end_y)) {
            dst[1].points[1].y = end_y;
        }
    }
    level--;
    points = conic_subdivide(&dst[0], points, level);
    return conic_subdivide(&dst[1], points, level);
}

int conic_split_into_quadratics(const struct conic *conic, struct point *points, int count)
{
    *points = conic->points[0];

    if (count >= CONIC_MAX_SUBDIVISION_COUNT) {
        struct conic dst[2];
        conic_split(conic, dst);

        if (points_are_equals(&dst[0].points[1], &dst[0].points[2]) &&
            points_are_equals(&dst[1].points[0], &dst[1].points[1])) {
            points[1] = dst[0].points[1];
            points[2] = dst[0].points[1];
            points[3] = dst[0].points[1];
            points[4] = dst[1].points[2];
            count = 1;
            goto done;
        }
    }

    conic_subdivide(conic, points + 1, count);

done:;
    const int quad_count = 1 << count;
    const int point_count = 2 * quad_count + 1;

    if (!points_are_finite(points, point_count)) {
        for (int i = 1; i < point_count - 1; i++) {
            points[i] = conic->points[1];
        }
    }

    return quad_count;
}

void conic_converter_to_quadratics(struct conic_converter *converter, const struct conic *conic, float tolerance)
{
    int count = conic_compute_subdivision_count(conic, tolerance);
    converter->quadratic_count = conic_split_into_quadratics(conic, converter->quadratics, count);
}
