/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2017-2022 GoPro Inc.
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

#ifndef ANIMATION_H
#define ANIMATION_H

#include "nopegl/nopegl.h"
#include "utils/darray.h"

struct animkeyframe_opts;

typedef void (*ngli_animation_mix_func_type)(void *user_arg, void *dst,
                                             const struct animkeyframe_opts *kf0,
                                             const struct animkeyframe_opts *kf1,
                                             double ratio);

typedef void (*ngli_animation_cpy_func_type)(void *user_arg, void *dst,
                                             const struct animkeyframe_opts *kf);

/*
 * The animation keyframes are sanitized during evaluation without altering
 * their original values:
 * - their times are evaluated as non-decreasing
 * With NGLI_ANIMATION_FLAG_TIME_VALUES:
 * - their scalar values (media times) are evaluated as non-negative and
 *   non-decreasing
 */
#define NGLI_ANIMATION_FLAG_TIME_VALUES (1U << 0)

struct animation {
    struct ngl_node * const *kfs;
    size_t nb_kfs;
    size_t current_kf;
    void *user_arg;
    ngli_animation_mix_func_type mix_func;
    ngli_animation_cpy_func_type cpy_func;
    uint32_t flags;
    NGLI_DARRAY(double) times;
    NGLI_DARRAY(double) values;
};

int ngli_animation_init(struct animation *s, void *user_arg,
                        struct ngl_node * const *kfs, size_t nb_kfs,
                        ngli_animation_mix_func_type mix_func,
                        ngli_animation_cpy_func_type cpy_func,
                        uint32_t flags);

void ngli_animation_reset(struct animation *s);

void ngli_animation_get_bounds(struct ngl_node * const *kfs, size_t nb_kfs, uint32_t flags,
                               double *times, double *values);

int ngli_animation_evaluate(struct animation *s, void *dst, double t);
int ngli_animation_derivate(struct animation *s, void *dst, double t);

#endif
