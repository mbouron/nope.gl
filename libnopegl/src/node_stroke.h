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

#ifndef NODE_STROKE_H
#define NODE_STROKE_H

#include <stddef.h>
#include <ngpu/ngpu.h>
#include "params.h"
#include "utils/darray.h"

#define STROKE_INSIDE  0
#define STROKE_CENTER  1
#define STROKE_OUTSIDE 2

#define STROKE_DASH_CAP_BUTT   0
#define STROKE_DASH_CAP_ROUND  1
#define STROKE_DASH_CAP_SQUARE 2

#define STROKE_HELPER_SRGB       (1u << 0)
#define STROKE_HELPER_MISC_UTILS (1u << 1)
#define STROKE_HELPER_NOISE      (1u << 2)

extern const struct param_choices ngli_stroke_mode_choices;
extern const struct param_choices ngli_stroke_dash_cap_choices;

/* One prebuilt uniform from stroke node opts */
struct stroke_uniform_def {
    char name[64];
    enum ngpu_type type;
    size_t opts_offset;
};

struct stroke_info {
    uint32_t helper_flags;                              /* STROKE_HELPER_* bitmask */
    const char *glsl;                                   /* ngli_stroke_color() function body */
    NGLI_DARRAY(struct stroke_uniform_def) uniforms;
    const void *opts;                                   /* pointer to stroke node opts struct */
};

void ngli_stroke_info_reset(struct stroke_info *si);

struct stroke_base_opts {
    float width;
    int mode;
    float dash_length;
    float dash_ratio;
    float dash_offset;
    int dash_cap;
    float opacity;
};

#endif
