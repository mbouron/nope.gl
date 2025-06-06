/*
 * Copyright 2021-2022 GoPro Inc.
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

#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <stdint.h>

#include "buffer_layout.h"
#include "ngpu/pipeline.h"

struct ngpu_ctx;

struct geometry {
    struct ngpu_ctx *gpu_ctx;

    struct ngpu_buffer *vertices_buffer;
    struct ngpu_buffer *uvcoords_buffer;
    struct ngpu_buffer *normals_buffer;
    struct ngpu_buffer *indices_buffer;

    uint32_t buffer_ownership;

    struct buffer_layout vertices_layout;
    struct buffer_layout uvcoords_layout;
    struct buffer_layout normals_layout;
    struct buffer_layout indices_layout;

    enum ngpu_primitive_topology topology;

    int64_t max_indices;
};

struct geometry *ngli_geometry_create(struct ngpu_ctx *gpu_ctx);

/* Set vertices/uvs/normals/indices from CPU buffers */
int ngli_geometry_set_vertices(struct geometry *s, size_t n, const float *vertices);
int ngli_geometry_set_uvcoords(struct geometry *s, size_t n, const float *uvcoords);
int ngli_geometry_set_normals(struct geometry *s, size_t n, const float *indices);
int ngli_geometry_set_indices(struct geometry *s, size_t n, const uint16_t *indices);

/* With the following functions, the user own the buffers already */
void ngli_geometry_set_vertices_buffer(struct geometry *s, struct ngpu_buffer *buffer, struct buffer_layout layout);
void ngli_geometry_set_uvcoords_buffer(struct geometry *s, struct ngpu_buffer *buffer, struct buffer_layout layout);
void ngli_geometry_set_normals_buffer(struct geometry *s, struct ngpu_buffer *buffer, struct buffer_layout layout);
void ngli_geometry_set_indices_buffer(struct geometry *s, struct ngpu_buffer *buffer, struct buffer_layout layout, int64_t max_indices);

/* Must be called when vertices/uvs/normals/indices are set */
int ngli_geometry_init(struct geometry *s, enum ngpu_primitive_topology topology);

void ngli_geometry_freep(struct geometry **sp);

#endif
