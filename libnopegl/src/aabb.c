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

struct point {
    float x, y;
};

static float vec2_dot(struct point a, struct point b)
{
    return a.x * b.x + a.y * b.y;
}

static struct point vec2_sub(struct point a, struct point b)
{
    return (struct point){a.x - b.x, a.y - b.y};
}

static float vec2_cross(struct point a, struct point b)
{
    return a.x * b.y - a.y * b.x;
}

static float vec2_length_squared(struct point v)
{
    return v.x * v.x + v.y * v.y;
}

static struct point vec2_norm(struct point v)
{
    float len = sqrtf(vec2_length_squared(v));
    return (struct point){v.x / len, v.y / len};
}

static int compare_points(const void* a, const void* b)
{
    struct point* p1 = (struct point*)a;
    struct point* p2 = (struct point*)b;
    if (p1->x != p2->x)
        return (p1->x < p2->x) ? -1 : 1;
    return (p1->y < p2->y) ? -1 : 1;
}

/*
 * Compute convex hull using Graham scan algorithm
 *
 * Adapted from https://en.wikipedia.org/wiki/Graham_scan
 */
static float ccw(struct point a, struct point b, struct point c)
{
    return vec2_cross(vec2_sub(a, b), vec2_sub(c, b));
}

static void compute_convex_hull(struct point* points, int nb_points, struct point* hull_points, int *nb_hull_points)
{
    qsort(points, nb_points, sizeof(struct point), compare_points);

    int k = 0;
    for (int i = 0; i < nb_points; i++) {
        while (k >= 2 && ccw(hull_points[k - 1], hull_points[k - 2], points[i]) <= 0) {
            k--;
        }
        hull_points[k++] = points[i];
    }

    for (int i = nb_points - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && ccw(hull_points[k - 1], hull_points[k - 2], points[i]) <= 0) {
            k--;
        }
        hull_points[k++] = points[i];
    }

    *nb_hull_points = k - 1;
}

/*
 * Compute minimal oriented bounding box from a convex hull using the rotating
 * caliper algorithm
 *
 * Adapted from https://en.wikipedia.org/wiki/Rotating_calipers
 */
static struct obb2d compute_obb_from_points(struct point* points, int nb_points)
{
    struct obb2d obb = {0};
    float min_area = FLT_MAX;

    for (int i = 0; i < nb_points; i++) {
        struct point edge = vec2_sub(points[(i + 1) % nb_points], points[i]);
        struct point edge_normalized = vec2_norm(edge);
        struct point r[] = {
            {edge_normalized.x, -edge_normalized.y},
            {edge_normalized.y,  edge_normalized.x},
        };

        struct point min = {FLT_MAX, FLT_MAX};
        struct point max = {-FLT_MAX, -FLT_MAX};

        for (int j = 0; j < nb_points; j++) {
            struct point rt[] = {
                {r[0].x, r[1].x},
                {r[0].y, r[1].y},
            };

            float rp[] = {
                vec2_dot(points[j], rt[0]),
                vec2_dot(points[j], rt[1]),
            };

            min.x = fminf(min.x, rp[0]);
            min.y = fminf(min.y, rp[1]);
            max.x = fmaxf(max.x, rp[0]);
            max.y = fmaxf(max.y, rp[1]);
        }

        const float width = max.y - min.y;
        const float height = max.x - min.x;
        const float area = width * height;

        if (area < min_area) {
            min_area = area;

            const struct point box[] = {
                {min.x, min.y},
                {min.x, max.y},
                {max.x, max.y},
                {max.x, min.y},
            };

            const struct point ps[] = {
                {vec2_dot(box[0], r[0]), vec2_dot(box[0], r[1])},
                {vec2_dot(box[1], r[0]), vec2_dot(box[1], r[1])},
                {vec2_dot(box[2], r[0]), vec2_dot(box[2], r[1])},
                {vec2_dot(box[3], r[0]), vec2_dot(box[3], r[1])},
            };

            obb = (struct obb2d) {
                .aabb = {
                    .center = {
                        ps[0].x * 0.5f + ps[2].x * 0.5f,
                        ps[0].y * 0.5f + ps[2].y * 0.5f,
                    },
                    .extent = {
                        width * 0.5f,
                        height * 0.5f
                    },
                },
                .rotation = NGLI_RAD2DEG(atan2f(edge.x, edge.y)),
            };
        }
    }

    return obb;
}

struct obb2d ngli_aabb_to_obb2d(const struct aabb *aabb, const float *m)
{
    const float *c = aabb->center;
    const float *e = aabb->extent;
    NGLI_ATTR_ALIGNED float corners[8 * 4] = {
        c[0] - e[0], c[1] - e[1], c[2] - e[2], 1.0f,
        c[0] + e[0], c[1] - e[1], c[2] - e[2], 1.0f,
        c[0] - e[0], c[1] + e[1], c[2] - e[2], 1.0f,
        c[0] + e[0], c[1] + e[1], c[2] - e[2], 1.0f,
        c[0] - e[0], c[1] - e[1], c[2] + e[2], 1.0f,
        c[0] + e[0], c[1] - e[1], c[2] + e[2], 1.0f,
        c[0] - e[0], c[1] + e[1], c[2] + e[2], 1.0f,
        c[0] + e[0], c[1] + e[1], c[2] + e[2], 1.0f,
    };

    float *corner = corners;
    for (int i = 0; i < 8; i++) {
        ngli_mat4_mul_vec4(corner, m, corner);
        corner += 4;
    }

    struct point points[8] = {0};
    corner = corners;
    for (int i = 0; i < 8; i++) {
        points[i].x = corner[0];
        points[i].y = corner[1];
        corner += 4;
    }

    struct point hull_points[8] = {0};
    int nb_hull_points = 0;
    compute_convex_hull(points, NGLI_ARRAY_NB(points), hull_points, &nb_hull_points);
    return compute_obb_from_points(hull_points, nb_hull_points);
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
