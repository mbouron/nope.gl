/*
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
 * The key frames are evaluated as sanitized, without altering them: their
 * times as non-decreasing, and with NGLI_ANIMATION_FLAG_TIME_VALUES their
 * scalar values as non-negative and non-decreasing (media time remapping).
 * A client can then update key frames one at a time, in any order, through
 * transiently inconsistent states.
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
    NGLI_DARRAY(double) times;  /* effective key frame times */
    NGLI_DARRAY(double) values; /* effective scalar values, with NGLI_ANIMATION_FLAG_TIME_VALUES */
};

int ngli_animation_init(struct animation *s, void *user_arg,
                        struct ngl_node * const *kfs, size_t nb_kfs,
                        ngli_animation_mix_func_type mix_func,
                        ngli_animation_cpy_func_type cpy_func,
                        uint32_t flags);

void ngli_animation_reset(struct animation *s);

int ngli_animation_evaluate(struct animation *s, void *dst, double t);
int ngli_animation_derivate(struct animation *s, void *dst, double t);

#endif
