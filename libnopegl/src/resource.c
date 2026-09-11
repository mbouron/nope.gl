/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include "image.h"
#include "resource.h"
#include "utils/memory.h"
#include "utils/refcount.h"
#include "utils/utils.h"

struct buffer_resource {
    struct ngli_rc rc;
    struct ngpu_buffer *buffer;
};

struct texture_resource {
    struct ngli_rc rc;
    struct ngpu_texture *texture;
};

struct image_resource {
    struct ngli_rc rc;
    struct ngli_image *image;
};

NGLI_RC_CHECK_STRUCT(buffer_resource);
NGLI_RC_CHECK_STRUCT(texture_resource);
NGLI_RC_CHECK_STRUCT(image_resource);

static void *resource_create(size_t size, ngli_freep_func freep)
{
    struct ngli_rc *rc = ngli_try_calloc(1, size);
    if (!rc)
        return NULL;

    *rc = NGLI_RC_CREATE(freep);
    return rc;
}

static void *resource_ref(const void *s)
{
    return s ? NGLI_RC_REF(s) : NULL;
}

static void buffer_resource_freep(void **sp)
{
    struct buffer_resource *s = *sp;
    ngli_buffer_resource_set(s, NULL);
    ngli_freep(sp);
}

struct buffer_resource *ngli_buffer_resource_create(void)
{
    return resource_create(sizeof(struct buffer_resource), buffer_resource_freep);
}

struct buffer_resource *ngli_buffer_resource_ref(const struct buffer_resource *s)
{
    return resource_ref(s);
}

void ngli_buffer_resource_unrefp(struct buffer_resource **sp)
{
    NGLI_RC_UNREFP(sp);
}

void ngli_buffer_resource_releasep(struct buffer_resource **sp)
{
    if (*sp)
        ngli_buffer_resource_set(*sp, NULL);
    ngli_buffer_resource_unrefp(sp);
}

void ngli_buffer_resource_set(struct buffer_resource *s, const struct ngpu_buffer *buffer)
{
    if (!s && !buffer)
        return;
    ngli_assert(s);

    struct ngpu_buffer *ref = ngpu_buffer_ref(buffer);
    ngpu_buffer_freep(&s->buffer);
    s->buffer = ref;
}

struct ngpu_buffer *ngli_buffer_resource_get(const struct buffer_resource *s)
{
    ngli_assert(s);
    return s->buffer;
}

static void texture_resource_freep(void **sp)
{
    struct texture_resource *s = *sp;
    ngli_texture_resource_set(s, NULL);
    ngli_freep(sp);
}

struct texture_resource *ngli_texture_resource_create(void)
{
    return resource_create(sizeof(struct texture_resource), texture_resource_freep);
}

struct texture_resource *ngli_texture_resource_ref(const struct texture_resource *s)
{
    return resource_ref(s);
}

void ngli_texture_resource_unrefp(struct texture_resource **sp)
{
    NGLI_RC_UNREFP(sp);
}

void ngli_texture_resource_releasep(struct texture_resource **sp)
{
    if (*sp)
        ngli_texture_resource_set(*sp, NULL);
    ngli_texture_resource_unrefp(sp);
}

void ngli_texture_resource_set(struct texture_resource *s, const struct ngpu_texture *texture)
{
    if (!s && !texture)
        return;
    ngli_assert(s);

    struct ngpu_texture *ref = ngpu_texture_ref(texture);
    ngpu_texture_freep(&s->texture);
    s->texture = ref;
}

struct ngpu_texture *ngli_texture_resource_get(const struct texture_resource *s)
{
    ngli_assert(s);
    return s->texture;
}

static void image_resource_freep(void **sp)
{
    struct image_resource *s = *sp;
    ngli_image_resource_set(s, NULL);
    ngli_freep(sp);
}

struct image_resource *ngli_image_resource_create(void)
{
    return resource_create(sizeof(struct image_resource), image_resource_freep);
}

struct image_resource *ngli_image_resource_ref(const struct image_resource *s)
{
    return resource_ref(s);
}

void ngli_image_resource_unrefp(struct image_resource **sp)
{
    NGLI_RC_UNREFP(sp);
}

void ngli_image_resource_releasep(struct image_resource **sp)
{
    if (*sp)
        ngli_image_resource_set(*sp, NULL);
    ngli_image_resource_unrefp(sp);
}

void ngli_image_resource_set(struct image_resource *s, const struct ngli_image *image)
{
    if (!s && !image)
        return;
    ngli_assert(s);

    struct ngli_image *ref = ngli_image_ref(image);
    ngli_image_unrefp(&s->image);
    s->image = ref;
}

const struct ngli_image *ngli_image_resource_get(const struct image_resource *s)
{
    ngli_assert(s);
    return s->image;
}
