/*
 * Copyright 2023-2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <string.h>

#include "ctx.h"
#include "fence.h"
#include "rendertarget.h"
#include "utils/memory.h"
#include "utils/log.h"
#include "utils/refcount.h"

#if defined(BACKEND_GL) || defined(BACKEND_GLES)
#include "opengl/ctx_gl.h"
#endif

const char *ngpu_backend_get_string_id(enum ngpu_backend_type backend)
{
    switch (backend) {
    case NGPU_BACKEND_OPENGL:   return "opengl";
    case NGPU_BACKEND_OPENGLES: return "opengles";
    case NGPU_BACKEND_VULKAN:   return "vulkan";
    default:                    return "unknown";
    }
}

const char *ngpu_backend_get_full_name(enum ngpu_backend_type backend)
{
    switch (backend) {
    case NGPU_BACKEND_OPENGL:   return "OpenGL";
    case NGPU_BACKEND_OPENGLES: return "OpenGL ES";
    case NGPU_BACKEND_VULKAN:   return "Vulkan";
    default:                    return "Unknown";
    }
}

int ngpu_viewport_is_valid(const struct ngpu_viewport *viewport)
{
    return viewport->width > 0 && viewport->height > 0;
}

extern const struct ngpu_ctx_class ngpu_ctx_gl;
extern const struct ngpu_ctx_class ngpu_ctx_gles;
extern const struct ngpu_ctx_class ngpu_ctx_vk;

static const struct ngpu_ctx_class *ctx_classes[] = {
#ifdef BACKEND_GL
    &ngpu_ctx_gl,
#endif
#ifdef BACKEND_GLES
    &ngpu_ctx_gles,
#endif
#ifdef BACKEND_VK
    &ngpu_ctx_vk,
#endif
    NULL,
};

int ngpu_get_available_backends(size_t *backend_count, enum ngpu_backend_type *backends)
{
    if (!backend_count)
        return NGPU_ERROR_INVALID_ARG;

    size_t total_count = 0;
    for (size_t i = 0; ctx_classes[i]; i++) {
        total_count++;
    }

    if (!backends) {
        *backend_count = total_count;
        return 0;
    }

    size_t wanted_count = NGPU_MIN(*backend_count, total_count);
    for (size_t i = 0; i < wanted_count; i++) {
        backends[i] = ctx_classes[i]->id;
    }

    return 0;
}

static const struct ngpu_ctx_class *get_ctx_class(enum ngpu_backend_type backend)
{
    for (size_t i = 0; ctx_classes[i]; i++) {
        if (ctx_classes[i]->id == backend)
            return ctx_classes[i];
    }
    return NULL;
}

void ngpu_ctx_params_init_from_shared_ctx(struct ngpu_ctx_params *params, struct ngpu_ctx *shared_ctx)
{
    memset(params, 0, sizeof(*params));
    if (shared_ctx) {
        params->shared_ctx = shared_ctx;
        params->backend    = shared_ctx->params.backend;
        params->platform   = shared_ctx->params.platform;
        params->display    = shared_ctx->params.display;
    }
}

int ngpu_ctx_params_copy(struct ngpu_ctx_params *dst, const struct ngpu_ctx_params *src)
{
    struct ngpu_ctx_params tmp = *src;

    if (src->backend_params) {
#if defined(BACKEND_GL) || defined(BACKEND_GLES)
        if (src->backend == NGPU_BACKEND_OPENGL ||
            src->backend == NGPU_BACKEND_OPENGLES) {
            const size_t size = sizeof(struct ngpu_ctx_params_gl);
            tmp.backend_params = ngpu_memdup(src->backend_params, size);
            if (!tmp.backend_params) {
                return NGPU_ERROR_MEMORY;
            }
            goto done;
            }
#endif

        LOG(ERROR, "backend_params %p is not supported by backend %u",
            src->backend_params, src->backend);
        return NGPU_ERROR_UNSUPPORTED;
    }

    done:
        *dst = tmp;

    return 0;
}

void ngpu_ctx_params_reset(struct ngpu_ctx_params *params)
{
    ngpu_freep(&params->backend_params);
    memset(params, 0, sizeof(*params));
}

static void ctx_freep(void **sp)
{
    struct ngpu_ctx **ctxp = (struct ngpu_ctx **)sp;
    struct ngpu_ctx *s = *ctxp;
    if (!s)
        return;

    ngpu_pgcache_freep(&s->program_cache);
    s->cls->destroy(s);

    /*
     * Drop our ref on shared_ctx (if any) after cls->destroy so the backend
     * can safely access shared resources during teardown.
     */
    struct ngpu_ctx *shared_ctx = s->params.shared_ctx;

    ngpu_ctx_params_reset(&s->params);
    ngpu_freep(ctxp);

    if (shared_ctx)
        NGPU_RC_UNREFP(&shared_ctx);
}

struct ngpu_ctx *ngpu_ctx_create(const struct ngpu_ctx_params *params)
{
    if (params->shared_ctx) {
        const struct ngpu_ctx *shared_ctx = params->shared_ctx;
        if (params->backend != shared_ctx->params.backend) {
            LOG(ERROR, "shared_ctx backend mismatch (%s vs %s)",
                ngpu_backend_get_string_id(params->backend),
                ngpu_backend_get_string_id(shared_ctx->params.backend));
            return NULL;
        }
        if (params->platform != shared_ctx->params.platform) {
            LOG(ERROR, "shared_ctx platform mismatch (%u vs %u)",
                params->platform, shared_ctx->params.platform);
            return NULL;
        }
    }

    const struct ngpu_ctx_class *cls = get_ctx_class(params->backend);
    if (!cls) {
        LOG(ERROR, "backend \"%s\" (%x) not available with this build",
            ngpu_backend_get_string_id(params->backend),
            params->backend);
        return NULL;
    }

    struct ngpu_ctx_params ctx_params = {0};
    int ret = ngpu_ctx_params_copy(&ctx_params, params);
    if (ret < 0)
        return NULL;

    struct ngpu_ctx *s = cls->create(params);
    if (!s) {
        ngpu_ctx_params_reset(&ctx_params);
        return NULL;
    }
    s->rc = NGPU_RC_CREATE(ctx_freep);
    s->params = ctx_params;
    s->cls = cls;

    if (s->params.shared_ctx)
        NGPU_RC_REF(s->params.shared_ctx);

    return s;
}

int ngpu_ctx_init(struct ngpu_ctx *s)
{
    int ret = s->cls->init(s);
    if (ret < 0)
        return ret;

    s->program_cache = ngpu_pgcache_create(s);
    if (!s->program_cache)
        return NGPU_ERROR_MEMORY;

    return 0;
}

int ngpu_ctx_resize(struct ngpu_ctx *s, uint32_t width, uint32_t height)
{
    const struct ngpu_ctx_class *cls = s->cls;
    return cls->resize(s, width, height);
}

int ngpu_ctx_set_capture_buffer(struct ngpu_ctx *s, void *capture_buffer)
{
    const struct ngpu_ctx_class *cls = s->cls;
    return cls->set_capture_buffer(s, capture_buffer);
}

uint32_t ngpu_ctx_advance_frame(struct ngpu_ctx *s)
{
    s->current_frame_index = (s->current_frame_index + 1) % s->nb_in_flight_frames;
    return s->current_frame_index;
}

uint32_t ngpu_ctx_get_current_frame_index(struct ngpu_ctx *s)
{
    return s->current_frame_index;
}

uint32_t ngpu_ctx_get_nb_in_flight_frames(struct ngpu_ctx *s)
{
    return s->nb_in_flight_frames;
}

int ngpu_ctx_begin_update(struct ngpu_ctx *s)
{
    return s->cls->begin_update(s);
}

int ngpu_ctx_end_update(struct ngpu_ctx *s, struct ngpu_fence *wait_fence)
{
    return s->cls->end_update(s, wait_fence);
}

int ngpu_ctx_begin_draw(struct ngpu_ctx *s)
{
    return s->cls->begin_draw(s);
}

int ngpu_ctx_end_draw(struct ngpu_ctx *s, double t, struct ngpu_fence *wait_fence, struct ngpu_fence **signal_fencep)
{
    return s->cls->end_draw(s, t, wait_fence, signal_fencep);
}

int ngpu_ctx_query_draw_time(struct ngpu_ctx *s, int64_t *time)
{
    return s->cls->query_draw_time(s, time);
}

void ngpu_ctx_wait_idle(struct ngpu_ctx *s)
{
    s->cls->wait_idle(s);
}

void ngpu_ctx_freep(struct ngpu_ctx **sp)
{
    NGPU_RC_UNREFP(sp);
}

enum ngpu_backend_type ngpu_ctx_get_backend_type(const struct ngpu_ctx *s)
{
    return s->params.backend;
}

enum ngpu_platform_type ngpu_ctx_get_platform_type(const struct ngpu_ctx *s)
{
    return s->params.platform;
}

void *ngpu_ctx_get_display(const struct ngpu_ctx *s)
{
    return (void *)s->params.display;
}

void *ngpu_ctx_get_window(const struct ngpu_ctx *s)
{
    return (void *)s->params.window;
}

int ngpu_ctx_get_version(const struct ngpu_ctx *s)
{
    return s->version;
}

int ngpu_ctx_get_language_version(const struct ngpu_ctx *s)
{
    return s->language_version;
}

uint64_t ngpu_ctx_get_features(const struct ngpu_ctx *s)
{
    return s->features;
}

const struct ngpu_limits *ngpu_ctx_get_limits(const struct ngpu_ctx *s)
{
    return &s->limits;
}

enum ngpu_cull_mode ngpu_ctx_get_cull_mode(struct ngpu_ctx *s, enum ngpu_cull_mode cull_mode)
{
    return s->cls->get_cull_mode(s, cull_mode);
}

void ngpu_ctx_get_projection_matrix(struct ngpu_ctx *s, float *dst)
{
    s->cls->get_projection_matrix(s, dst);
}

void ngpu_ctx_get_projection_matrix_inverse(struct ngpu_ctx *s, float *dst)
{
    s->cls->get_projection_matrix_inverse(s, dst);
}

void ngpu_ctx_begin_render_pass(struct ngpu_ctx *s, struct ngpu_rendertarget *rt)
{
    ngpu_assert(rt);
    ngpu_assert(!s->rendertarget);

    s->rendertarget = rt;
    s->cls->begin_render_pass(s, rt);
}

void ngpu_ctx_end_render_pass(struct ngpu_ctx *s)
{
    s->cls->end_render_pass(s);
    s->rendertarget = NULL;
}

bool ngpu_ctx_is_render_pass_active(const struct ngpu_ctx *s)
{
    return s->rendertarget != NULL;
}

void ngpu_ctx_get_rendertarget_uvcoord_matrix(struct ngpu_ctx *s, float *dst)
{
    s->cls->get_rendertarget_uvcoord_matrix(s, dst);
}

struct ngpu_rendertarget *ngpu_ctx_get_default_rendertarget(struct ngpu_ctx *s)
{
    return s->cls->get_default_rendertarget(s);
}

const struct ngpu_rendertarget_layout *ngpu_ctx_get_default_rendertarget_layout(struct ngpu_ctx *s)
{
    return s->cls->get_default_rendertarget_layout(s);
}

void ngpu_ctx_get_default_rendertarget_size(struct ngpu_ctx *s, uint32_t *width, uint32_t *height)
{
    s->cls->get_default_rendertarget_size(s, width, height);
}

void ngpu_ctx_set_viewport(struct ngpu_ctx *s, const struct ngpu_viewport *viewport)
{
    ngpu_assert(s->rendertarget);
    s->cls->set_viewport(s, viewport);
}

void ngpu_ctx_set_scissor(struct ngpu_ctx *s, const struct ngpu_scissor *scissor)
{
    ngpu_assert(s->rendertarget);
    s->cls->set_scissor(s, scissor);
}

enum ngpu_format ngpu_ctx_get_preferred_depth_format(struct ngpu_ctx *s)
{
    return s->cls->get_preferred_depth_format(s);
}

enum ngpu_format ngpu_ctx_get_preferred_depth_stencil_format(struct ngpu_ctx *s)
{
    return s->cls->get_preferred_depth_stencil_format(s);
}

uint32_t ngpu_ctx_get_format_features(struct ngpu_ctx *s, enum ngpu_format format)
{
    return s->cls->get_format_features(s, format);
}

void ngpu_ctx_generate_texture_mipmap(struct ngpu_ctx *s, struct ngpu_texture *texture)
{
    ngpu_assert(!s->rendertarget);
    s->cls->generate_texture_mipmap(s, texture);
}

void ngpu_ctx_set_pipeline(struct ngpu_ctx *s, struct ngpu_pipeline *pipeline)
{
    s->pipeline = pipeline;
    s->cls->set_pipeline(s, pipeline);
}

void ngpu_ctx_set_bindgroup(struct ngpu_ctx *s, struct ngpu_bindgroup *bindgroup, const uint32_t *offsets, size_t nb_offsets)
{
    s->bindgroup = bindgroup;

    ngpu_assert(bindgroup->layout->nb_dynamic_offsets == nb_offsets);
    memcpy(s->dynamic_offsets, offsets, nb_offsets * sizeof(*s->dynamic_offsets));
    s->nb_dynamic_offsets = nb_offsets;

    s->cls->set_bindgroup(s, bindgroup, offsets, nb_offsets);
}

static void validate_vertex_buffers(struct ngpu_ctx *s)
{
    const struct ngpu_pipeline *pipeline = s->pipeline;
    const struct ngpu_pipeline_graphics *graphics = &pipeline->graphics;
    const struct ngpu_vertex_state *vertex_state = &graphics->vertex_state;
    for (size_t i = 0; i < vertex_state->nb_buffers; i++) {
        ngpu_assert(s->vertex_buffers[i]);
    }
}

void ngpu_ctx_draw(struct ngpu_ctx *s, uint32_t nb_vertices, uint32_t nb_instances, uint32_t first_vertex)
{
    ngpu_assert(s->pipeline);
    validate_vertex_buffers(s);
    ngpu_assert(s->bindgroup);
    const struct ngpu_bindgroup_layout *p_layout = s->pipeline->layout.bindgroup_layout;
    const struct ngpu_bindgroup_layout *b_layout = s->bindgroup->layout;
    ngpu_assert(ngpu_bindgroup_layout_is_compatible(p_layout, b_layout));

    s->cls->draw(s, nb_vertices, nb_instances, first_vertex);
}

void ngpu_ctx_draw_indexed(struct ngpu_ctx *s, uint32_t nb_indices, uint32_t nb_instances, uint32_t first_index)
{
    ngpu_assert(s->pipeline);
    validate_vertex_buffers(s);
    ngpu_assert(s->bindgroup);
    const struct ngpu_bindgroup_layout *p_layout = s->pipeline->layout.bindgroup_layout;
    const struct ngpu_bindgroup_layout *b_layout = s->bindgroup->layout;
    ngpu_assert(ngpu_bindgroup_layout_is_compatible(p_layout, b_layout));

    s->cls->draw_indexed(s, nb_indices, nb_instances, first_index);
}

void ngpu_ctx_dispatch(struct ngpu_ctx *s, uint32_t nb_group_x, uint32_t nb_group_y, uint32_t nb_group_z)
{
    ngpu_assert(s->pipeline);
    ngpu_assert(s->bindgroup);
    const struct ngpu_bindgroup_layout *p_layout = s->pipeline->layout.bindgroup_layout;
    const struct ngpu_bindgroup_layout *b_layout = s->bindgroup->layout;
    ngpu_assert(ngpu_bindgroup_layout_is_compatible(p_layout, b_layout));

    s->cls->dispatch(s, nb_group_x, nb_group_y, nb_group_z);
}

void ngpu_ctx_set_vertex_buffer(struct ngpu_ctx *s, uint32_t index, const struct ngpu_buffer *buffer)
{
    struct ngpu_limits *limits = &s->limits;
    ngpu_assert(index < limits->max_vertex_attributes);
    s->vertex_buffers[index] = buffer;
    s->cls->set_vertex_buffer(s, index, buffer);
}

void ngpu_ctx_set_index_buffer(struct ngpu_ctx *s, const struct ngpu_buffer *buffer, enum ngpu_format format)
{
    s->index_buffer = buffer;
    s->index_format = format;
    s->cls->set_index_buffer(s, buffer, format);
}

struct ngpu_fence *ngpu_fence_create(struct ngpu_ctx *s)
{
    return s->cls->fence_create(s);
}

int ngpu_fence_reset(struct ngpu_fence *fence)
{
    return fence->gpu_ctx->cls->fence_reset(fence);
}

int ngpu_fence_wait(struct ngpu_fence *fence)
{
    return fence->gpu_ctx->cls->fence_wait(fence);
}

int ngpu_fence_is_signaled(struct ngpu_fence *fence)
{
    return fence->gpu_ctx->cls->fence_is_signaled(fence);
}

void ngpu_fence_freep(struct ngpu_fence **sp)
{
    if (!*sp)
        return;
    (*sp)->gpu_ctx->cls->fence_freep(sp);
}
