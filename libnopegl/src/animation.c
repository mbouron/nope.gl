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

#include <float.h>

#include "animation.h"
#include "internal.h"
#include "log.h"
#include "math_utils.h"
#include "node_animkeyframe.h"
#include "nopegl/nopegl.h"

static size_t get_kf_id(const struct animation *s, size_t start, double t)
{
    size_t ret = SIZE_MAX;

    for (size_t i = start; i < s->nb_kfs; i++) {
        if (s->times.data[i] > t)
            break;
        ret = i;
    }
    return ret;
}

static void mix(const struct animation *s, void *dst, size_t kf0_id, size_t kf1_id, double ratio)
{
    if (s->flags & NGLI_ANIMATION_FLAG_TIME_VALUES) {
        *(double *)dst = NGLI_MIX_F64(s->values.data[kf0_id], s->values.data[kf1_id], ratio);
        return;
    }
    s->mix_func(s->user_arg, dst, s->kfs[kf0_id]->opts, s->kfs[kf1_id]->opts, ratio);
}

static void cpy(const struct animation *s, void *dst, size_t kf_id)
{
    if (s->flags & NGLI_ANIMATION_FLAG_TIME_VALUES) {
        *(double *)dst = s->values.data[kf_id];
        return;
    }
    s->cpy_func(s->user_arg, dst, s->kfs[kf_id]->opts);
}

static int evaluate(struct animation *s, void *dst, double t, int derivative)
{
    struct ngl_node * const *animkf = s->kfs;
    const size_t nb_animkf = s->nb_kfs;
    size_t kf_id = get_kf_id(s, s->current_kf, t);
    if (kf_id == SIZE_MAX)
        kf_id = get_kf_id(s, 0, t);
    if (kf_id != SIZE_MAX && kf_id < nb_animkf - 1) {
        const struct animkeyframe_priv *kf1_priv = animkf[kf_id + 1]->priv_data;
        const struct animkeyframe_opts *kf1 = animkf[kf_id + 1]->opts;
        const double t0 = s->times.data[kf_id];
        const double t1 = s->times.data[kf_id + 1];

        double tnorm = NGLI_LINEAR_NORM(t0, t1, t);
        if (kf1_priv->scale_boundaries)
            tnorm = NGLI_MIX_F64(kf1->offsets[0], kf1->offsets[1], tnorm);
        double ratio;
        if (derivative) {
            ratio = kf1_priv->derivative(tnorm, kf1->args.count, kf1->args.data);
            if (kf1_priv->scale_boundaries)
                ratio *= kf1_priv->derivative_scale;
        } else {
            ratio = kf1_priv->function(tnorm, kf1->args.count, kf1->args.data);
            if (kf1_priv->scale_boundaries)
                ratio = NGLI_LINEAR_NORM(kf1_priv->boundaries[0], kf1_priv->boundaries[1], ratio);
        }

        s->current_kf = kf_id;
        mix(s, dst, kf_id, kf_id + 1, ratio);
    } else {
        cpy(s, dst, t < s->times.data[0] ? 0 : nb_animkf - 1);
    }
    return 0;
}

int ngli_animation_evaluate(struct animation *s, void *dst, double t)
{
    return evaluate(s, dst, t, 0);
}

int ngli_animation_derivate(struct animation *s, void *dst, double t)
{
    return evaluate(s, dst, t, 1);
}

int ngli_animation_init(struct animation *s, void *user_arg,
                        struct ngl_node * const *kfs, size_t nb_kfs,
                        ngli_animation_mix_func_type mix_func,
                        ngli_animation_cpy_func_type cpy_func,
                        uint32_t flags)
{
    if (nb_kfs < 1) {
        LOG(ERROR, "invalid number of animated key frames: %zu", nb_kfs);
        return NGL_ERROR_INVALID_ARG;
    }

    ngli_assert(mix_func && cpy_func);

    ngli_animation_reset(s);
    *s = (struct animation){
        .kfs = kfs,
        .nb_kfs = nb_kfs,
        .user_arg = user_arg,
        .mix_func = mix_func,
        .cpy_func = cpy_func,
        .flags = flags,
    };

    double prev_time = -DBL_MAX;
    double prev_value = 0.0;
    for (size_t i = 0; i < nb_kfs; i++) {
        const struct animkeyframe_opts *kf = kfs[i]->opts;

        if (kf->time < prev_time)
            LOG(DEBUG, "key frame times must be monotonically increasing: %g < %g, evaluated as %g",
                kf->time, prev_time, prev_time);
        prev_time = NGLI_MAX(kf->time, prev_time);
        if (ngli_darray_try_push(&s->times, prev_time) < 0)
            goto fail;

        if (flags & NGLI_ANIMATION_FLAG_TIME_VALUES) {
            if (kf->scalar < prev_value)
                LOG(DEBUG, "times must be positive and monotonically increasing: %g < %g, evaluated as %g",
                    kf->scalar, prev_value, prev_value);
            prev_value = NGLI_MAX(kf->scalar, prev_value);
            if (ngli_darray_try_push(&s->values, prev_value) < 0)
                goto fail;
        }
    }

    return 0;

fail:
    ngli_animation_reset(s);
    return NGL_ERROR_MEMORY;
}

void ngli_animation_reset(struct animation *s)
{
    ngli_darray_reset(&s->times);
    ngli_darray_reset(&s->values);
    *s = (struct animation){0};
}
