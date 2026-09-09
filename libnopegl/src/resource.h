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

#ifndef RESOURCE_H
#define RESOURCE_H

#include <ngpu/ngpu.h>

/* Stable owner; publish only initialized allocations. Consumers borrow this
 * object and retain the GPU allocation they resolve from it. */
struct buffer_resource {
    struct ngpu_buffer *buffer;
};

static inline void ngli_buffer_resource_reset(struct buffer_resource *s)
{
    struct ngpu_buffer *buffer = s->buffer;
    s->buffer = NULL;
    ngpu_buffer_freep(&buffer);
}

#endif
