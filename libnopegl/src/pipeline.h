/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

struct ngpu_staging_buffer;

struct ngli_pipeline_params {
    enum ngpu_pipeline_type type;
    struct ngpu_pipeline_graphics graphics;
    const struct ngpu_program *program;
    struct ngpu_bindgroup_layout_desc layout_desc;
    struct ngpu_bindgroup_resources resources;
    struct ngpu_vertex_resources vertex_resources;
    struct ngpu_pgcraft_texture_infos texture_infos;
};

struct ngli_pipeline;

struct ngli_pipeline *ngli_pipeline_create(struct ngpu_ctx *gpu_ctx);
int ngli_pipeline_init(struct ngli_pipeline *s, const struct ngli_pipeline_params *params);
int ngli_pipeline_update_vertex_buffer(struct ngli_pipeline *s, int32_t index, const struct ngpu_buffer *buffer);
void ngli_pipeline_update_vertex_resources(struct ngli_pipeline *s, struct ngpu_vertex_resources resources);
int ngli_pipeline_update_texture(struct ngli_pipeline *s, int32_t index, const struct ngpu_texture *texture);
void ngli_pipeline_update_image(struct ngli_pipeline *s, int32_t index, const struct ngli_image *image);
/* For a dynamic binding, offset is the execution offset and size is the block range. */
int ngli_pipeline_update_buffer(struct ngli_pipeline *s, int32_t index, const struct ngpu_buffer *buffer, size_t offset, size_t size);
void ngli_pipeline_draw(struct ngli_pipeline *s, struct ngpu_staging_buffer *staging_buffer, uint32_t nb_vertices, uint32_t nb_instances, uint32_t first_vertex);
void ngli_pipeline_draw_indexed(struct ngli_pipeline *s, struct ngpu_staging_buffer *staging_buffer, const struct ngpu_buffer *indices, enum ngpu_format indices_format, uint32_t nb_indices, uint32_t nb_instances);
void ngli_pipeline_dispatch(struct ngli_pipeline *s, struct ngpu_staging_buffer *staging_buffer, uint32_t nb_group_x, uint32_t nb_group_y, uint32_t nb_group_z);
/* Forget resolved bindings while preserving the GPU pipeline and layout. */
void ngli_pipeline_discard_resources(struct ngli_pipeline *s);
void ngli_pipeline_freep(struct ngli_pipeline **sp);

#endif
