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

#include "geometry.h"
#include "log.h"
#include <ngpu/ngpu.h>
#include "nopegl/nopegl.h"
#include "utils/memory.h"
#include "utils/utils.h"

#define OWN_VERTICES (1 << 0)
#define OWN_UVCOORDS (1 << 1)
#define OWN_NORMALS  (1 << 2)
#define OWN_INDICES  (1 << 3)

struct geometry *ngli_geometry_create(struct ngpu_ctx *gpu_ctx)
{
    struct geometry *s = ngli_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->gpu_ctx = gpu_ctx;
    return s;
}

static int gen_buffer(struct geometry *s,
                      struct resource *resource, const struct buffer_layout *layout,
                      const void *data, uint32_t usage)
{
    struct ngpu_buffer *buffer = ngpu_buffer_create(s->gpu_ctx);
    if (!buffer)
        return NGL_ERROR_MEMORY;

    const size_t size = layout->count * layout->stride;

    int ret = ngpu_buffer_init(buffer, size, NGPU_BUFFER_USAGE_TRANSFER_DST_BIT | usage);
    if (ret < 0) {
        ngpu_buffer_freep(&buffer);
        return ret;
    }

    ret = ngpu_buffer_upload(buffer, data, layout->offset, size);
    if (ret < 0) {
        ngpu_buffer_freep(&buffer);
        return ret;
    }

    ngli_resource_set_buffer(resource, buffer);
    ngpu_buffer_freep(&buffer);
    return 0;
}

static int gen_vec3(struct geometry *s,
                    struct resource *resource, struct buffer_layout *layout,
                    size_t count, const float *data)
{
    const enum ngpu_format format = NGPU_FORMAT_R32G32B32_SFLOAT;
    *layout = (struct buffer_layout){
        .type   = NGPU_TYPE_VEC3,
        .format = format,
        .stride = ngpu_format_get_bytes_per_pixel(format),
        .comp   = ngpu_format_get_nb_comp(format),
        .count  = count,
        .offset = 0,
    };
    return gen_buffer(s, resource, layout, data, NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

int ngli_geometry_set_vertices(struct geometry *s, size_t n, const float *vertices)
{
    ngli_assert(!(s->buffer_ownership & OWN_VERTICES));
    s->buffer_ownership |= OWN_VERTICES;
    ngli_resource_freep(&s->vertices);
    s->vertices = ngli_resource_create(NGLI_RESOURCE_BUFFER);
    if (!s->vertices)
        return NGL_ERROR_MEMORY;
    return gen_vec3(s, s->vertices, &s->vertices_layout, n, vertices);
}

int ngli_geometry_set_normals(struct geometry *s, size_t n, const float *normals)
{
    ngli_assert(!(s->buffer_ownership & OWN_NORMALS));
    s->buffer_ownership |= OWN_NORMALS;
    ngli_resource_freep(&s->normals);
    s->normals = ngli_resource_create(NGLI_RESOURCE_BUFFER);
    if (!s->normals)
        return NGL_ERROR_MEMORY;
    return gen_vec3(s, s->normals, &s->normals_layout, n, normals);
}

static int gen_vec2(struct geometry *s,
                    struct resource *resource, struct buffer_layout *layout,
                    size_t count, const float *data)
{
    const enum ngpu_format format = NGPU_FORMAT_R32G32_SFLOAT;
    *layout = (struct buffer_layout){
        .type   = NGPU_TYPE_VEC2,
        .format = format,
        .stride = ngpu_format_get_bytes_per_pixel(format),
        .comp   = ngpu_format_get_nb_comp(format),
        .count  = count,
        .offset = 0,
    };
    return gen_buffer(s, resource, layout, data, NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

int ngli_geometry_set_uvcoords(struct geometry *s, size_t n, const float *uvcoords)
{
    ngli_assert(!(s->buffer_ownership & OWN_UVCOORDS));
    s->buffer_ownership |= OWN_UVCOORDS;
    ngli_resource_freep(&s->uvcoords);
    s->uvcoords = ngli_resource_create(NGLI_RESOURCE_BUFFER);
    if (!s->uvcoords)
        return NGL_ERROR_MEMORY;
    return gen_vec2(s, s->uvcoords, &s->uvcoords_layout, n, uvcoords);
}

int ngli_geometry_set_indices(struct geometry *s, size_t count, const uint16_t *indices)
{
    ngli_assert(!(s->buffer_ownership & OWN_INDICES));
    s->buffer_ownership |= OWN_INDICES;
    ngli_resource_freep(&s->indices);
    s->indices = ngli_resource_create(NGLI_RESOURCE_BUFFER);
    if (!s->indices)
        return NGL_ERROR_MEMORY;
    const enum ngpu_format format = NGPU_FORMAT_R16_UNORM;
    s->indices_layout = (struct buffer_layout){
        .type   = NGPU_TYPE_NONE,
        .format = format,
        .stride = ngpu_format_get_bytes_per_pixel(format),
        .comp   = ngpu_format_get_nb_comp(format),
        .count  = count,
        .offset = 0,
    };
    for (size_t i = 0; i < count; i++)
        s->max_indices = NGLI_MAX(s->max_indices, indices[i]);
    return gen_buffer(s, s->indices, &s->indices_layout, indices, NGPU_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void ngli_geometry_set_vertices_buffer(struct geometry *s, const struct resource *resource, struct buffer_layout layout)
{
    ngli_assert(!(s->buffer_ownership & OWN_VERTICES));
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&s->vertices);
    s->vertices = ref;
    s->vertices_layout = layout;
}

void ngli_geometry_set_uvcoords_buffer(struct geometry *s, const struct resource *resource, struct buffer_layout layout)
{
    ngli_assert(!(s->buffer_ownership & OWN_UVCOORDS));
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&s->uvcoords);
    s->uvcoords = ref;
    s->uvcoords_layout = layout;
}

void ngli_geometry_set_normals_buffer(struct geometry *s, const struct resource *resource, struct buffer_layout layout)
{
    ngli_assert(!(s->buffer_ownership & OWN_NORMALS));
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&s->normals);
    s->normals = ref;
    s->normals_layout = layout;
}

void ngli_geometry_set_indices_buffer(struct geometry *s, const struct resource *resource,
                                      struct buffer_layout layout, int64_t max_indices)
{
    ngli_assert(!(s->buffer_ownership & OWN_INDICES));
    struct resource *ref = ngli_resource_ref(resource);
    ngli_resource_freep(&s->indices);
    s->indices = ref;
    s->indices_layout = layout;
    s->max_indices = max_indices;
}

int ngli_geometry_init(struct geometry *s, enum ngpu_primitive_topology topology)
{
    s->topology = topology;

    if (s->uvcoords_layout.count && s->uvcoords_layout.count != s->vertices_layout.count) {
        LOG(ERROR, "uvcoords count (%zu) does not match vertices count (%zu)", s->uvcoords_layout.count, s->vertices_layout.count);
        return NGL_ERROR_INVALID_ARG;
    }

    if (s->normals_layout.count && s->normals_layout.count != s->vertices_layout.count) {
        LOG(ERROR, "normals count (%zu) does not match vertices count (%zu)", s->normals_layout.count, s->vertices_layout.count);
        return NGL_ERROR_INVALID_ARG;
    }

    return 0;
}

void ngli_geometry_freep(struct geometry **sp)
{
    struct geometry *s = *sp;
    if (!s)
        return;
    if (s->buffer_ownership & OWN_VERTICES)
        ngli_resource_clear(s->vertices);
    ngli_resource_freep(&s->vertices);
    if (s->buffer_ownership & OWN_UVCOORDS)
        ngli_resource_clear(s->uvcoords);
    ngli_resource_freep(&s->uvcoords);
    if (s->buffer_ownership & OWN_NORMALS)
        ngli_resource_clear(s->normals);
    ngli_resource_freep(&s->normals);
    if (s->buffer_ownership & OWN_INDICES)
        ngli_resource_clear(s->indices);
    ngli_resource_freep(&s->indices);
    ngli_freep(sp);
}
