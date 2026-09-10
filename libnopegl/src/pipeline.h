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

#include "resource.h"

struct image;
struct ngl_node;
struct ngpu_staging_buffer;

struct pipeline_params {
    enum ngpu_pipeline_type type;
    struct ngpu_pipeline_graphics graphics;
    const struct ngpu_program *program;
    struct ngpu_bindgroup_layout_desc layout_desc;
    struct ngpu_pgcraft_texture_infos texture_infos;
};

struct pipeline_buffer_source {
    struct resource *resource;
    size_t offset;
    size_t size; /* NGPU_BUFFER_WHOLE_SIZE resolves the remaining allocation */
};

struct pipeline_execution {
    struct ngpu_staging_buffer *staging;
};

struct pipeline;

struct pipeline *ngli_pipeline_create(struct ngpu_ctx *gpu_ctx);
/* Register sources or supply direct bindings before the first execution. */
int ngli_pipeline_init(struct pipeline *s, const struct pipeline_params *params);
/* Registrations retain resource holders and never query GPU allocations.
 * Sources are polled before each execution; no notifications are needed.
 * -1 is optimized out (NOT_FOUND). NULL removes a registration. Direct setters
 * conflict with registered sources, including an image's generated slots. */
int ngli_pipeline_set_buffer_source(struct pipeline *s, int32_t index, const struct pipeline_buffer_source *source);
int ngli_pipeline_set_vertex_source(struct pipeline *s, int32_t index, const struct resource *resource);
int ngli_pipeline_set_index_source(struct pipeline *s, const struct resource *resource, enum ngpu_format format);
int ngli_pipeline_set_image_source(struct pipeline *s, int32_t image_index, const struct resource *resource, struct ngl_node *reframing_node);
int ngli_pipeline_set_texture_source(struct pipeline *s, int32_t index, const struct resource *resource);
int ngli_pipeline_update_index_buffer(struct pipeline *s, const struct ngpu_buffer *buffer, enum ngpu_format format);
/* Discard resolved references and pooled groups while preserving registrations.
 * Direct bindings and dynamic offsets must be supplied again before execution. */
void ngli_pipeline_discard_resources(struct pipeline *s);
int ngli_pipeline_update_vertex_buffer(struct pipeline *s, int32_t index, const struct ngpu_buffer *buffer);
int ngli_pipeline_update_texture(struct pipeline *s, int32_t index, const struct ngpu_texture *texture);
int ngli_pipeline_update_buffer(struct pipeline *s, int32_t index, const struct ngpu_buffer *buffer, size_t offset, size_t size);
int ngli_pipeline_update_dynamic_offsets(struct pipeline *s, const uint32_t *offsets, size_t nb_offsets);
int ngli_pipeline_draw(struct pipeline *s, const struct pipeline_execution *execution, uint32_t nb_vertices, uint32_t nb_instances, uint32_t first_vertex);
int ngli_pipeline_draw_indexed(struct pipeline *s, const struct pipeline_execution *execution, uint32_t nb_indices, uint32_t nb_instances);
int ngli_pipeline_dispatch(struct pipeline *s, const struct pipeline_execution *execution, uint32_t nb_group_x, uint32_t nb_group_y, uint32_t nb_group_z);
void ngli_pipeline_freep(struct pipeline **sp);

#endif
