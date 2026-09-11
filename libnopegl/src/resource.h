/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

struct ngli_image;
struct buffer_resource;
struct texture_resource;
struct image_resource;

struct buffer_resource *ngli_buffer_resource_create(void);
struct buffer_resource *ngli_buffer_resource_ref(const struct buffer_resource *s);
void ngli_buffer_resource_unrefp(struct buffer_resource **sp);
void ngli_buffer_resource_releasep(struct buffer_resource **sp);
void ngli_buffer_resource_set(struct buffer_resource *s, const struct ngpu_buffer *buffer);
struct ngpu_buffer *ngli_buffer_resource_get(const struct buffer_resource *s);

struct texture_resource *ngli_texture_resource_create(void);
struct texture_resource *ngli_texture_resource_ref(const struct texture_resource *s);
void ngli_texture_resource_unrefp(struct texture_resource **sp);
void ngli_texture_resource_releasep(struct texture_resource **sp);
void ngli_texture_resource_set(struct texture_resource *s, const struct ngpu_texture *texture);
struct ngpu_texture *ngli_texture_resource_get(const struct texture_resource *s);

struct image_resource *ngli_image_resource_create(void);
struct image_resource *ngli_image_resource_ref(const struct image_resource *s);
void ngli_image_resource_unrefp(struct image_resource **sp);
void ngli_image_resource_releasep(struct image_resource **sp);
void ngli_image_resource_set(struct image_resource *s, const struct ngli_image *image);
const struct ngli_image *ngli_image_resource_get(const struct image_resource *s);

#endif
