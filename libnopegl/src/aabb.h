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

#ifndef AABB_H
#define AABB_H

#include <float.h>
#include <stdint.h>

#include "utils/utils.h"

#define NGLI_AABB_EMPTY (struct aabb){ \
    .center = {0.f, 0.f, 0.f, 1.f},  \
    .extent = {-FLT_MAX, -FLT_MAX, 0.f, 1.f}, \
}

struct ray {
    NGLI_ALIGNED_VEC(origin);
    NGLI_ALIGNED_VEC(direction);
    NGLI_ALIGNED_VEC(direction_inv);
};

struct aabb {
    NGLI_ALIGNED_VEC(center);
    NGLI_ALIGNED_VEC(extent);
};

struct obb2d {
    struct aabb aabb;
    float rotation;
};

struct aabb ngli_aabb_union(const struct aabb *a, const struct aabb *b);
void ngli_aabb_get_min_max(const struct aabb *aabb, float *min, float *max);
struct aabb ngli_aabb_apply_transform(const struct aabb *aabb, const float *m);
int ngli_aabb_intersect_point(const struct aabb *aabb, const float *p);
int ngli_aabb_intersect_aabb(const struct aabb *aabb1, const struct aabb *aabb2);
int ngli_aabb_intersect_ray(const struct aabb *aabb, const struct ray *ray);


#endif
