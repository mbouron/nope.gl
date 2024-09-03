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

#include <float.h>
#include <math.h>
#include <string.h>

#include "aabb.h"
#include "math_utils.h"
#include "utils.h"

struct aabb ngli_aabb_from_vertices(const float *vertices, size_t nb_vertices)
{
    NGLI_ALIGNED_VEC(min) = { FLT_MAX,  FLT_MAX,  FLT_MAX, 1.f};
    NGLI_ALIGNED_VEC(max) = {-FLT_MAX, -FLT_MAX, -FLT_MAX, 1.f};

    const float *vertex = vertices;
    for (size_t i = 0; i < nb_vertices; i++) {
        min[0] = NGLI_MIN(min[0], vertex[0]);
        min[1] = NGLI_MIN(min[1], vertex[1]);
        min[2] = NGLI_MIN(min[2], vertex[2]);
        max[0] = NGLI_MAX(max[0], vertex[0]);
        max[1] = NGLI_MAX(max[1], vertex[1]);
        max[2] = NGLI_MAX(max[2], vertex[2]);
        vertex += 3;
    }

    struct aabb aabb = {
        .center[0] = (min[0] + max[0]) / 2.f,
        .center[1] = (min[1] + max[1]) / 2.f,
        .center[2] = (min[2] + max[2]) / 2.f,
        .center[3] = 1.f,
        .extent[0] = (max[0] - min[0]) / 2.f,
        .extent[1] = (max[1] - min[1]) / 2.f,
        .extent[2] = (max[2] - min[2]) / 2.f,
        .extent[3] = 1.f,
    };

    return aabb;
}

void ngli_aabb_get_min_max(const struct aabb *aabb, float *min, float *max)
{
    ngli_vec4_sub(min, aabb->center, aabb->extent);
    ngli_vec4_add(max, aabb->center, aabb->extent);
}

struct aabb ngli_aabb_apply_transform(const struct aabb *aabb, const float *m)
{
    struct aabb trf_aabb = {0};
    ngli_mat4_mul_vec4(trf_aabb.center, m, aabb->center);

    NGLI_ALIGNED_MAT(abs_m);
    ngli_mat4_abs(abs_m, m);
    abs_m[12] = 0.f;
    abs_m[13] = 0.f;
    abs_m[14] = 0.f;
    ngli_mat4_mul_vec4(trf_aabb.extent, abs_m, aabb->extent);

    return trf_aabb;
}

struct aabb ngli_aabb_apply_projection(const struct aabb *aabb, const float *m)
{
    NGLI_ALIGNED_VEC(min);
    NGLI_ALIGNED_VEC(max);
    ngli_aabb_get_min_max(aabb, min, max);

    NGLI_ALIGNED_VEC(dis);
    ngli_vec4_sub(dis, max, min);

    NGLI_ALIGNED_VEC(sx) = {dis[0], 0.f,    0.f,    0.f};
    NGLI_ALIGNED_VEC(sy) = {0.f,    dis[1], 0.f,    0.f};
    NGLI_ALIGNED_VEC(sz) = {0.f,    0.f,    dis[2], 0.f};
    NGLI_ALIGNED_VEC(p0) = {min[0], min[1], min[2], 1.f};

    ngli_mat4_mul_vec4(sx, m, sx);
    ngli_mat4_mul_vec4(sy, m, sy);
    ngli_mat4_mul_vec4(sz, m, sz);
    ngli_mat4_mul_vec4(p0, m, p0);

    #define p(x) (&points[(x) * 4])

    NGLI_ATTR_ALIGNED float points[8*4] = {0};
    memcpy(p(0), p0, sizeof(p0));
    ngli_vec4_add(p(1), p(0), sz);
    ngli_vec4_add(p(2), p(0), sy);
    ngli_vec4_add(p(3), p(2), sz);
    ngli_vec4_add(p(4), p(0), sx);
    ngli_vec4_add(p(5), p(4), sz);
    ngli_vec4_add(p(6), p(4), sy);
    ngli_vec4_add(p(7), p(6), sz);

    #undef p

    min[0] = FLT_MAX;
    min[1] = FLT_MAX;
    min[2] = FLT_MAX;
    min[3] = 1.f;

    max[0] = -FLT_MAX;
    max[1] = -FLT_MAX;
    max[2] = -FLT_MAX;
    max[3] = 1.f;

    float *p = points;
    for (size_t i = 0; i < 8; i++) {
        ngli_vec4_perspective_div(p, p);
        min[0] = NGLI_MIN(min[0], p[0]);
        min[1] = NGLI_MIN(min[1], p[1]);
        min[2] = NGLI_MIN(min[2], p[2]);
        max[0] = NGLI_MAX(max[0], p[0]);
        max[1] = NGLI_MAX(max[1], p[1]);
        max[2] = NGLI_MAX(max[2], p[2]);
        p += 4;
    }

    struct aabb trf_aabb = {
        .center[0] = (min[0] + max[0]) / 2.f,
        .center[1] = (min[1] + max[1]) / 2.f,
        .center[2] = (min[2] + max[2]) / 2.f,
        .center[3] = 1.f,
        .extent[0] = (max[0] - min[0]) / 2.f,
        .extent[1] = (max[1] - min[1]) / 2.f,
        .extent[2] = (max[2] - min[2]) / 2.f,
        .extent[3] = 1.f,
    };

    return trf_aabb;
}

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
