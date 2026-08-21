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

#ifndef NODE_PAINT_H
#define NODE_PAINT_H

#include <stddef.h>
#include <stdint.h>
#include <ngpu/ngpu.h>
#include "utils/darray.h"

#define PAINT_HELPER_SRGB        (1u << 0)
#define PAINT_HELPER_MISC_UTILS  (1u << 1)
#define PAINT_HELPER_NOISE       (1u << 2)

#define PAINT_WRAP_DEFAULT 0
#define PAINT_WRAP_DISCARD 1

#define PAINT_SCALING_NONE 0
#define PAINT_SCALING_FIT  1
#define PAINT_SCALING_FILL 2

/* Maximum length of a symbol a paint node declares in its GLSL, including the nul */
#define PAINT_NAME_LEN 64

struct paint_uniform_def {
    char name[PAINT_NAME_LEN];
    enum ngpu_type type;
    const uint8_t *data;
};

struct paint_custom_uniform_def {
    char name[PAINT_NAME_LEN];
    enum ngpu_type type;
    const struct ngl_node *node;
};

struct paint_custom_texture_def {
    char name[PAINT_NAME_LEN];
    struct ngl_node *texture_node;
};

struct paint_custom_block_def {
    char name[PAINT_NAME_LEN];
    struct ngl_node *node;
};

struct paint_base_opts {
    float opacity;
    int scaling;                                             /* PAINT_SCALING_* */
    int wrap;                                                /* PAINT_WRAP_* */
    int premult;                                             /* premultiply paint color by its alpha */
};

enum paint_shader_role {
    PAINT_SHADER_ROLE_FILL,
    PAINT_SHADER_ROLE_STROKE,
    PAINT_SHADER_ROLE_NB,
};

struct paint_info {
    uint32_t helper_flags;                                   /* PAINT_HELPER_* bitmask */
    const char *glsl_header;                                 /* user declarations, NULL if none */
    const char *glsl[PAINT_SHADER_ROLE_NB];                  /* complete fill and stroke functions */
    NGLI_DARRAY(struct paint_uniform_def) uniforms;
    NGLI_DARRAY(struct paint_custom_uniform_def) custom_uniforms;
    NGLI_DARRAY(struct paint_custom_texture_def) custom_textures;
    NGLI_DARRAY(struct paint_custom_block_def) custom_blocks;
    struct ngl_node *texture;
    size_t color_output_count;
    const void *opts;                                        /* pointer to paint node opts struct */
};

/* Node types implementing the paint interface */
extern const uint32_t ngli_paint_node_types[];

void ngli_paint_info_reset(struct paint_info *info);

#endif
