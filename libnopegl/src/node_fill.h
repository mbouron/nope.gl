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

/* Maximum number of prebuilt/custom uniforms per fill source */
#define FILL_MAX_UNIFORMS 16
#define FILL_MAX_TEXTURES 8

/* Bitmask of GLSL helper blocks to prepend to the fragment shader */
#define FILL_HELPER_SRGB        (1u << 0)
#define FILL_HELPER_MISC_UTILS  (1u << 1)
#define FILL_HELPER_NOISE       (1u << 2)

/* Texture wrap values (must match texturefill_wrap_choices in node_fill.c) */
#define FILL_WRAP_DEFAULT 0
#define FILL_WRAP_DISCARD 1

/* Texture scaling values (must match texturefill_scaling_choices in node_fill.c) */
#define FILL_SCALING_NONE 0
#define FILL_SCALING_FIT  1
#define FILL_SCALING_FILL 2

/* One prebuilt uniform from fill node opts */
struct fill_uniform_def {
    char name[64];
    enum ngpu_type type;
    size_t opts_offset;   /* byte offset into fill node opts struct */
};

/* One user-supplied uniform node (CustomFill only) */
struct fill_custom_uniform_def {
    char name[64];
    enum ngpu_type type;
    const struct ngl_node *node;
};

/* One user-supplied texture node (CustomFill only) */
struct fill_custom_texture_def {
    char name[64];
    struct ngl_node *texture_node;   /* root of transform/texture chain */
};

/* One user-supplied block node (CustomFill only) */
struct fill_custom_block_def {
    char name[64];
    struct ngl_node *node;
};

/*
 * fill_info is stored as the FIRST member of every fill node's priv_data.
 * DrawRect casts fill_node->priv_data to (const struct fill_info *) at init.
 */
struct fill_info {
    uint32_t helper_flags;                                   /* FILL_HELPER_* bitmask */
    const char *glsl;                                        /* ngl_color() function body */
    struct fill_uniform_def uniforms[FILL_MAX_UNIFORMS];
    size_t nb_uniforms;
    struct fill_custom_uniform_def custom_uniforms[FILL_MAX_UNIFORMS];
    size_t nb_custom_uniforms;
    struct fill_custom_texture_def custom_textures[FILL_MAX_TEXTURES];
    size_t nb_custom_textures;
    struct fill_custom_block_def custom_blocks[FILL_MAX_TEXTURES];
    size_t nb_custom_blocks;
    struct ngl_node *texture_transform;                      /* root of texture/transform chain (TextureFill only) */
    int scaling;                                             /* FILL_SCALING_* (TextureFill only) */
    int wrap;                                                /* FILL_WRAP_* (TextureFill only) */
    size_t nb_frag_output;                                   /* MRT output count (0 = single output) */
    const void *opts;                                        /* pointer to fill node opts struct */
};

#endif
