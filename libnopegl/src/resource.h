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

struct image;
struct resource;

enum resource_type {
    NGLI_RESOURCE_BUFFER,
    NGLI_RESOURCE_TEXTURE,
    NGLI_RESOURCE_IMAGE,
};

/* A resource owns its current GPU references. Consumers retain this holder and
 * resolve its current value before execution. All operations run on the render
 * thread; reference counting does not synchronize publication and resolution. */
struct resource *ngli_resource_create(enum resource_type type);
struct resource *ngli_resource_ref(const struct resource *s);
void ngli_resource_freep(struct resource **sp);
enum resource_type ngli_resource_get_type(const struct resource *s);

/* Setters retain inputs before releasing the previous value. Images are copied
 * with references to their planes. NULL clears a publication, preserving the
 * holder and its type. Clear before producer release if consumers must see an
 * unavailable resource; dropping a reference alone preserves the current value. */
void ngli_resource_set_buffer(struct resource *s, const struct ngpu_buffer *buffer);
void ngli_resource_set_texture(struct resource *s, const struct ngpu_texture *texture);
void ngli_resource_set_image(struct resource *s, const struct image *image);
void ngli_resource_clear(struct resource *s);

/* Values are borrowed until the next publication. An empty image resource
 * returns an image with NGLI_IMAGE_LAYOUT_NONE. */
struct ngpu_buffer *ngli_resource_get_buffer(const struct resource *s);
struct ngpu_texture *ngli_resource_get_texture(const struct resource *s);
const struct image *ngli_resource_get_image(const struct resource *s);

#endif
