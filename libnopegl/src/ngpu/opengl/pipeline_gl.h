/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef NGPU_PIPELINE_GL_H
#define NGPU_PIPELINE_GL_H

#include "glincludes.h"
#include "ngpu/pipeline.h"

struct ngpu_ctx;
struct glcontext;

struct ngpu_pipeline_gl {
    struct ngpu_pipeline parent;

    struct darray attribute_bindings; // attribute_binding_gl

    GLuint vao_id;
};

struct ngpu_pipeline *ngpu_pipeline_gl_create(struct ngpu_ctx *gpu_ctx);
int ngpu_pipeline_gl_init(struct ngpu_pipeline *s);
void ngpu_pipeline_gl_draw(struct ngpu_pipeline *s, uint32_t nb_vertices, uint32_t nb_instances, uint32_t first_vertex);
void ngpu_pipeline_gl_draw_indexed(struct ngpu_pipeline *s, uint32_t nb_indices, uint32_t nb_instances);
void ngpu_pipeline_gl_dispatch(struct ngpu_pipeline *s, uint32_t nb_group_x, uint32_t nb_group_y, uint32_t nb_group_z);
void ngpu_pipeline_gl_freep(struct ngpu_pipeline **sp);

#endif
