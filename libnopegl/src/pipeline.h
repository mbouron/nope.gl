/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2022 GoPro Inc.
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

#ifndef PIPELINE_H
#define PIPELINE_H

#include <ngpu/ngpu.h>
#include <ngpu/ngpu.h>

struct image;
struct ngpu_staging_buffer;

struct pipeline_params {
    enum ngpu_pipeline_type type;
    struct ngpu_pipeline_graphics graphics;
    const struct ngpu_program *program;
    struct ngpu_bindgroup_layout_desc layout_desc;
    struct ngpu_pgcraft_texture_infos texture_infos;
};

struct pipeline;

struct pipeline *ngli_pipeline_create(struct ngpu_ctx *gpu_ctx);
/* Resources must be bound with the update functions before drawing or dispatching. */
int ngli_pipeline_init(struct pipeline *s, const struct pipeline_params *params);
int ngli_pipeline_update_vertex_buffer(struct pipeline *s, int32_t index, const struct ngpu_buffer *buffer);
int ngli_pipeline_update_texture(struct pipeline *s, int32_t index, const struct ngpu_texture *texture);
void ngli_pipeline_apply_reframing_matrix(struct pipeline *s, int32_t index, const struct image *image, const float *reframing, struct ngpu_staging_buffer *staging);
void ngli_pipeline_update_image(struct pipeline *s, int32_t index, const struct image *image, struct ngpu_staging_buffer *staging);
int ngli_pipeline_update_buffer(struct pipeline *s, int32_t index, const struct ngpu_buffer *buffer, size_t offset, size_t size);
int ngli_pipeline_update_dynamic_offsets(struct pipeline *s, const uint32_t *offsets, size_t nb_offsets);
void ngli_pipeline_draw(struct pipeline *s, uint32_t nb_vertices, uint32_t nb_instances, uint32_t first_vertex);
void ngli_pipeline_draw_indexed(struct pipeline *s, const struct ngpu_buffer *indices, enum ngpu_format indices_format, uint32_t nb_indices, uint32_t nb_instances);
void ngli_pipeline_dispatch(struct pipeline *s, uint32_t nb_group_x, uint32_t nb_group_y, uint32_t nb_group_z);
void ngli_pipeline_freep(struct pipeline **sp);

#endif
