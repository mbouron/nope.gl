/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2018-2022 GoPro Inc.
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

#ifndef BUFFER_GL_H
#define BUFFER_GL_H

#include "buffer.h"
#include "glincludes.h"

struct buffer_gl {
    struct buffer parent;
    GLuint id;
    GLbitfield map_flags;
    GLbitfield barriers;
};

struct gpu_ctx;

struct buffer *ngli_buffer_gl_create(struct gpu_ctx *gpu_ctx);
int ngli_buffer_gl_init(struct buffer *s);
int ngli_buffer_gl_upload(struct buffer *s, const void *data, size_t offset, size_t size);
int ngli_buffer_gl_map(struct buffer *s, size_t offset, size_t size, void **datap);
void ngli_buffer_gl_unmap(struct buffer *s);
void ngli_buffer_gl_freep(struct buffer **sp);

#endif
