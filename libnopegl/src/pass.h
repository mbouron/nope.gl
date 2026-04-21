/*
 * Copyright 2019-2022 GoPro Inc.
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

#ifndef PASS_H
#define PASS_H

#include <stdint.h>

#include "blending.h"
#include <ngpu/ngpu.h>
#include <ngpu/ngpu.h>
#include "utils/darray.h"

struct ngl_ctx;
struct pipeline_compat;
struct resource_map;
struct texture_map;

struct pipeline_desc {
    struct pipeline_compat *pipeline_compat;
    NGLI_DARRAY(struct resource_map) blocks_map;
    NGLI_DARRAY(struct texture_map) textures_map;
};

struct pass_params {
    const char *label;
    const char *program_label;

    /* graphics */
    const char *vert_base;
    const char *frag_base;
    struct hmap *vert_resources;
    struct hmap *frag_resources;
    const struct hmap *properties;
    const struct geometry *geometry;
    uint32_t nb_instances;
    struct hmap *attributes;
    struct hmap *instance_attributes;
    struct ngpu_pgcraft_iovar *vert_out_vars;
    size_t nb_vert_out_vars;
    size_t nb_frag_output;
    enum ngli_blending blending;

    /* compute */
    const char *comp_base;
    struct hmap *compute_resources;
    uint32_t workgroup_count[3];
    uint32_t workgroup_size[3];
};

enum {
    PASS_BUILTIN_VERT_MODELVIEW_MATRIX,
    PASS_BUILTIN_VERT_PROJECTION_MATRIX,
    PASS_BUILTIN_VERT_NORMAL_MATRIX,
    PASS_BUILTIN_VERT_NB_FIELDS,
};

struct user_uniform_entry {
    enum ngpu_program_stage stage;
    const void *data;
    size_t field_index;
};

struct pass {
    struct ngl_ctx *ctx;
    struct pass_params params;

    struct ngpu_buffer *indices;
    const struct buffer_layout *indices_layout;
    uint32_t nb_vertices;
    uint32_t nb_instances;
    enum ngpu_primitive_topology topology;
    enum ngpu_pipeline_type pipeline_type;
    NGLI_DARRAY(struct ngpu_pgcraft_attribute) crafter_attributes;
    NGLI_DARRAY(struct ngpu_pgcraft_texture) crafter_textures;
    NGLI_DARRAY(struct ngpu_pgcraft_block) crafter_blocks;
    struct ngpu_pgcraft *crafter;
    int32_t resolution_field_index;
    struct ngpu_block_desc user_vert_block;
    struct ngpu_block_desc user_frag_block;
    struct ngpu_block_desc user_comp_block;
    int32_t user_vert_block_index;
    int32_t user_frag_block_index;
    int32_t user_comp_block_index;
    NGLI_DARRAY(struct user_uniform_entry) user_data_ptrs;
    struct pipeline_desc pipeline_desc;
};

int ngli_pass_init(struct pass *s, struct ngl_ctx *ctx, const struct pass_params *params);
int ngli_pass_prepare(struct pass *s,
                      const struct ngpu_graphics_state *graphics_state,
                      const struct ngpu_rendertarget_layout *rendertarget_layout);
void ngli_pass_uninit(struct pass *s);
int ngli_pass_exec(struct pass *s);

#endif
