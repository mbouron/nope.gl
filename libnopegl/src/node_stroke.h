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

/* Mode values (must match ngli_stroke_mode_choices in node_stroke.c) */
#define STROKE_INSIDE  0
#define STROKE_CENTER  1
#define STROKE_OUTSIDE 2

/* Dash cap styles (must match ngli_stroke_dash_cap_choices in node_stroke.c) */
#define STROKE_DASH_CAP_BUTT   0
#define STROKE_DASH_CAP_ROUND  1
#define STROKE_DASH_CAP_SQUARE 2

/* Maximum number of prebuilt uniforms per stroke source */
#define STROKE_MAX_UNIFORMS 16

/*
 * Helper flags for stroke nodes: share values with FILL_HELPER_* so that
 * DrawRect can OR fill and stroke flags together to decide which helpers to
 * prepend.  Values must stay in sync with node_fill.h.
 */
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

/*
 * stroke_info is stored as the FIRST member of every stroke node's priv_data.
 * DrawRect casts stroke_node->priv_data to (const struct stroke_info *) at init.
 */
struct stroke_info {
    uint32_t helper_flags;                              /* STROKE_HELPER_* bitmask */
    const char *glsl;                                   /* ngl_stroke_color() function body */
    struct stroke_uniform_def uniforms[STROKE_MAX_UNIFORMS];
    size_t nb_uniforms;
    const void *opts;                                   /* pointer to stroke node opts struct */
};

/*
 * Common prefix shared by ALL stroke node opts structs.
 * DrawRect casts any stroke_node->opts to (const struct stroke_base_opts *)
 * to read width, mode and dash parameters.
 */
struct stroke_base_opts {
    float width;
    int mode;
    float dash_length;
    float dash_ratio;
    float dash_offset;
    int dash_cap;
};

struct stroke_opts {
    /* stroke_base_opts fields must come first */
    float width;
    int mode;
    float dash_length;
    float dash_ratio;
    float dash_offset;
    int dash_cap;
    /* stroke-specific fields */
    float color[4];
};

#endif
