/*
 * Copyright 2024 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include "aabb.h"
#include "math_utils.h"
#include "utils/utils.h"

void ngli_aabb_get_min_max(const struct aabb *aabb, float *min, float *max)
{
    ngli_vec4_sub(min, aabb->center, aabb->extent);
    ngli_vec4_add(max, aabb->center, aabb->extent);
}

struct aabb ngli_aabb_union(const struct aabb *a, const struct aabb *b)
{
    NGLI_ALIGNED_VEC(a_min);
    NGLI_ALIGNED_VEC(a_max);
    NGLI_ALIGNED_VEC(b_min);
    NGLI_ALIGNED_VEC(b_max);
    ngli_aabb_get_min_max(a, a_min, a_max);
    ngli_aabb_get_min_max(b, b_min, b_max);

    const NGLI_ALIGNED_VEC(min) = {
        NGLI_MIN(a_min[0], b_min[0]),
        NGLI_MIN(a_min[1], b_min[1]),
        NGLI_MIN(a_min[2], b_min[2]),
        1.f,
    };
    const NGLI_ALIGNED_VEC(max) = {
        NGLI_MAX(a_max[0], b_max[0]),
        NGLI_MAX(a_max[1], b_max[1]),
        NGLI_MAX(a_max[2], b_max[2]),
        1.f,
    };

    return (struct aabb){
        .center[0] = (min[0] + max[0]) / 2.f,
        .center[1] = (min[1] + max[1]) / 2.f,
        .center[2] = (min[2] + max[2]) / 2.f,
        .center[3] = 1.f,
        .extent[0] = (max[0] - min[0]) / 2.f,
        .extent[1] = (max[1] - min[1]) / 2.f,
        .extent[2] = (max[2] - min[2]) / 2.f,
        .extent[3] = 1.f,
    };
}

struct aabb ngli_aabb_apply_transform(const struct aabb *aabb, const float *m)
{
    struct aabb trf_aabb = {0};
    ngli_mat4_mul_vec4(trf_aabb.center, m, aabb->center);

    struct ngli_mat4 abs_m;
    ngli_mat4_abs(abs_m.m, m);
    abs_m.m[12] = 0.f;
    abs_m.m[13] = 0.f;
    abs_m.m[14] = 0.f;
    ngli_mat4_mul_vec4(trf_aabb.extent, abs_m.m, aabb->extent);

    return trf_aabb;
}

struct point {
    float x, y;
};

int ngli_aabb_intersect_point(const struct aabb *aabb, const float *p)
{
    NGLI_ALIGNED_VEC(d);
    ngli_vec4_sub(d, aabb->center, p);
    ngli_vec4_abs(d, d);

    return (d[0] <= aabb->extent[0] &&
            d[1] <= aabb->extent[1] &&
            d[2] <= aabb->extent[2]);
}

int ngli_aabb_intersect_aabb(const struct aabb *aabb1, const struct aabb *aabb2)
{
    NGLI_ALIGNED_VEC(c);
    ngli_vec4_sub(c, aabb1->center, aabb2->center);
    ngli_vec4_abs(c, c);

    NGLI_ALIGNED_VEC(r);
    ngli_vec4_add(r, aabb1->extent, aabb2->extent);

    return (c[0] <= r[0] &&
            c[1] <= r[2] &&
            c[2] <= r[2]);
}

int ngli_aabb_intersect_ray(const struct aabb *aabb, const struct ray *ray)
{
    NGLI_ALIGNED_VEC(min);
    NGLI_ALIGNED_VEC(max);
    ngli_aabb_get_min_max(aabb, min, max);

    float tmin = 0.0f, tmax = INFINITY;
    for (int d = 0; d < 3; ++d) {
        const float t1 = (min[d] - ray->origin[d]) * ray->direction_inv[d];
        const float t2 = (max[d] - ray->origin[d]) * ray->direction_inv[d];

        tmin = NGLI_MIN(NGLI_MAX(t1, tmin), NGLI_MAX(t2, tmin));
        tmax = NGLI_MAX(NGLI_MIN(t1, tmax), NGLI_MIN(t2, tmax));
    }
    return tmin <= tmax;
}
