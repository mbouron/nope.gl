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

struct ngli_image;
struct buffer_resource;
struct texture_resource;
struct image_resource;

/* A resource owns its current GPU references. Consumers retain this holder and
 * resolve its current value before execution. All operations run on the render
 * thread; reference counting does not synchronize publication and resolution. */
struct buffer_resource *ngli_buffer_resource_create(void);
struct texture_resource *ngli_texture_resource_create(void);
struct image_resource *ngli_image_resource_create(void);

/* All resource types begin with an ngli_rc. ref() retains a holder and returns
 * NULL for a NULL input; freep() drops one reference and clears the caller's
 * pointer, destroying the holder on its last reference. The three holders are
 * structurally identical, so these stay typed: a generic void * helper would
 * let a holder of one kind be stored in a slot of another without a
 * diagnostic. */
struct buffer_resource *ngli_buffer_resource_ref(const struct buffer_resource *s);
struct texture_resource *ngli_texture_resource_ref(const struct texture_resource *s);
struct image_resource *ngli_image_resource_ref(const struct image_resource *s);

void ngli_buffer_resource_freep(struct buffer_resource **sp);
void ngli_texture_resource_freep(struct texture_resource **sp);
void ngli_image_resource_freep(struct image_resource **sp);

/* Clear the publication, then drop the caller's reference, so a consumer still
 * holding the holder observes an unavailable resource rather than the last
 * value. Unlike the setters these accept a pointer to a NULL holder, which a
 * failed initialization leaves behind for the matching uninit to clean up. */
void ngli_buffer_resource_releasep(struct buffer_resource **sp);
void ngli_texture_resource_releasep(struct texture_resource **sp);
void ngli_image_resource_releasep(struct image_resource **sp);

/* Setters retain inputs before releasing the previous value. NULL clears a
 * publication, preserving the holder. Clear before producer release if
 * consumers must see an unavailable resource; dropping a reference alone
 * preserves the current value. Clearing a NULL holder is a no-op, allowing
 * cleanup after partial initialization. Publishing a non-NULL value requires
 * a non-NULL holder. */
void ngli_buffer_resource_set(struct buffer_resource *s, const struct ngpu_buffer *buffer);
void ngli_texture_resource_set(struct texture_resource *s, const struct ngpu_texture *texture);
void ngli_image_resource_set(struct image_resource *s, const struct ngli_image *image);

/* Values are borrowed until the next publication. An empty image resource
 * returns NULL, which ngli_image_get_params() maps to NGLI_IMAGE_LAYOUT_NONE. */
struct ngpu_buffer *ngli_buffer_resource_get(const struct buffer_resource *s);
struct ngpu_texture *ngli_texture_resource_get(const struct texture_resource *s);
const struct ngli_image *ngli_image_resource_get(const struct image_resource *s);

#endif
