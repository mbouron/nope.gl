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

struct resource {
    struct ngli_rc rc;
    enum resource_type type;
    union {
        struct ngpu_buffer *buffer;
        struct ngpu_texture *texture;
        struct image image;
    } data;
};

NGLI_RC_CHECK_STRUCT(resource);

void ngli_resource_clear(struct resource *s)
{
    ngli_assert(s);

    switch (s->type) {
    case NGLI_RESOURCE_BUFFER:
        ngpu_buffer_freep(&s->data.buffer);
        break;
    case NGLI_RESOURCE_TEXTURE:
        ngpu_texture_freep(&s->data.texture);
        break;
    case NGLI_RESOURCE_IMAGE:
        for (size_t i = 0; i < NGLI_ARRAY_NB(s->data.image.planes); i++)
            ngpu_texture_freep(&s->data.image.planes[i]);
        ngli_image_reset(&s->data.image);
        break;
    default:
        ngli_assert(0);
    }
}

static void resource_freep(void **sp)
{
    struct resource *s = *sp;
    if (!s)
        return;

    ngli_resource_clear(s);
    ngli_freep(sp);
}

struct resource *ngli_resource_create(enum resource_type type)
{
    ngli_assert(type == NGLI_RESOURCE_BUFFER ||
                type == NGLI_RESOURCE_TEXTURE ||
                type == NGLI_RESOURCE_IMAGE);

    struct resource *s = ngli_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->rc = NGLI_RC_CREATE(resource_freep);
    s->type = type;
    if (type == NGLI_RESOURCE_IMAGE)
        ngli_image_reset(&s->data.image);
    return s;
}

struct resource *ngli_resource_ref(const struct resource *s)
{
    return s ? NGLI_RC_REF((struct resource *)s) : NULL;
}

void ngli_resource_freep(struct resource **sp)
{
    NGLI_RC_UNREFP(sp);
}

enum resource_type ngli_resource_get_type(const struct resource *s)
{
    ngli_assert(s);
    return s->type;
}

void ngli_resource_set_buffer(struct resource *s, const struct ngpu_buffer *buffer)
{
    ngli_assert(s && s->type == NGLI_RESOURCE_BUFFER);

    struct ngpu_buffer *ref = ngpu_buffer_ref(buffer);
    ngpu_buffer_freep(&s->data.buffer);
    s->data.buffer = ref;
}

void ngli_resource_set_texture(struct resource *s, const struct ngpu_texture *texture)
{
    ngli_assert(s && s->type == NGLI_RESOURCE_TEXTURE);

    struct ngpu_texture *ref = ngpu_texture_ref(texture);
    ngpu_texture_freep(&s->data.texture);
    s->data.texture = ref;
}

void ngli_resource_set_image(struct resource *s, const struct image *image)
{
    ngli_assert(s && s->type == NGLI_RESOURCE_IMAGE);

    struct image next;
    ngli_image_reset(&next);
    if (image) {
        ngli_assert(image->nb_planes <= NGLI_ARRAY_NB(image->planes));
        next = *image;
        for (size_t i = 0; i < NGLI_ARRAY_NB(next.planes); i++)
            next.planes[i] = i < image->nb_planes ? ngpu_texture_ref(image->planes[i]) : NULL;
    }

    ngli_resource_clear(s);
    s->data.image = next;
}

struct ngpu_buffer *ngli_resource_get_buffer(const struct resource *s)
{
    ngli_assert(s && s->type == NGLI_RESOURCE_BUFFER);
    return s->data.buffer;
}

struct ngpu_texture *ngli_resource_get_texture(const struct resource *s)
{
    ngli_assert(s && s->type == NGLI_RESOURCE_TEXTURE);
    return s->data.texture;
}

const struct image *ngli_resource_get_image(const struct resource *s)
{
    ngli_assert(s && s->type == NGLI_RESOURCE_IMAGE);
    return &s->data.image;
}
