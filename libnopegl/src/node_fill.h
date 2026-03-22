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

#ifndef NODE_FILL_H
#define NODE_FILL_H

#include <stddef.h>
#include <ngpu/ngpu.h>

#define FILL_MAX_UNIFORMS 16
#define FILL_MAX_TEXTURES 8

#define FILL_HELPER_SRGB        (1u << 0)
#define FILL_HELPER_MISC_UTILS  (1u << 1)
#define FILL_HELPER_NOISE       (1u << 2)

#define FILL_WRAP_DEFAULT 0
#define FILL_WRAP_DISCARD 1

#define FILL_SCALING_NONE 0
#define FILL_SCALING_FIT  1
#define FILL_SCALING_FILL 2

struct fill_uniform_def {
    char name[64];
    enum ngpu_type type;
    size_t opts_offset;   /* byte offset into fill node opts struct */
};

struct fill_custom_uniform_def {
    char name[64];
    enum ngpu_type type;
    const struct ngl_node *node;
};

struct fill_custom_texture_def {
    char name[64];
    struct ngl_node *texture_node;
};

struct fill_custom_block_def {
    char name[64];
    struct ngl_node *node;
};

struct fill_info {
    uint32_t helper_flags;                                   /* FILL_HELPER_* bitmask */
    const char *glsl;                                        /* ngli_color() function body */
    struct fill_uniform_def uniforms[FILL_MAX_UNIFORMS];
    size_t nb_uniforms;
    struct fill_custom_uniform_def custom_uniforms[FILL_MAX_UNIFORMS];
    size_t nb_custom_uniforms;
    struct fill_custom_texture_def custom_textures[FILL_MAX_TEXTURES];
    size_t nb_custom_textures;
    struct fill_custom_block_def custom_blocks[FILL_MAX_TEXTURES];
    size_t nb_custom_blocks;
    struct ngl_node *texture_node;                            /* texture node (TextureFill only) */
    int scaling;                                             /* FILL_SCALING_* (TextureFill only) */
    int wrap;                                                /* FILL_WRAP_* (TextureFill only) */
    size_t nb_frag_output;                                   /* MRT output count (0 = single output) */
    const void *opts;                                        /* pointer to fill node opts struct */
};

#endif
