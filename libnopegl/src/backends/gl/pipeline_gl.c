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

#include <string.h>

#include "bind_group_gl.h"
#include "buffer_gl.h"
#include "format.h"
#include "gpu_ctx_gl.h"
#include "glcontext.h"
#include "log.h"
#include "memory.h"
#include "internal.h"
#include "pipeline_gl.h"
#include "program_gl.h"
#include "texture_gl.h"
#include "topology.h"
#include "type.h"

struct attribute_binding_gl {
    struct pipeline_attribute_desc desc;
};

static int build_attribute_bindings(struct pipeline *s)
{
    struct pipeline_gl *s_priv = (struct pipeline_gl *)s;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    ngli_glGenVertexArrays(gl, 1, &s_priv->vao_id);
    ngli_glBindVertexArray(gl, s_priv->vao_id);

    const struct pipeline_layout *layout = &s->layout;
    for (size_t i = 0; i < layout->nb_attribute_descs; i++) {
        const struct pipeline_attribute_desc *pipeline_attribute_desc = &layout->attribute_descs[i];

        struct attribute_binding_gl binding = {
            .desc = *pipeline_attribute_desc,
        };
        if (!ngli_darray_push(&s_priv->attribute_bindings, &binding))
            return NGL_ERROR_MEMORY;

        const GLuint location = pipeline_attribute_desc->location;
        const GLuint rate = pipeline_attribute_desc->rate;
        ngli_glEnableVertexAttribArray(gl, location);
        if (rate > 0)
            ngli_glVertexAttribDivisor(gl, location, rate);
    }

    return 0;
}

static const GLenum gl_primitive_topology_map[NGLI_PRIMITIVE_TOPOLOGY_NB] = {
    [NGLI_PRIMITIVE_TOPOLOGY_POINT_LIST]     = GL_POINTS,
    [NGLI_PRIMITIVE_TOPOLOGY_LINE_LIST]      = GL_LINES,
    [NGLI_PRIMITIVE_TOPOLOGY_LINE_STRIP]     = GL_LINE_STRIP,
    [NGLI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST]  = GL_TRIANGLES,
    [NGLI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP] = GL_TRIANGLE_STRIP,
};

static GLenum get_gl_topology(int topology)
{
    return gl_primitive_topology_map[topology];
}

static const GLenum gl_indices_type_map[NGLI_FORMAT_NB] = {
    [NGLI_FORMAT_R16_UNORM] = GL_UNSIGNED_SHORT,
    [NGLI_FORMAT_R32_UINT]  = GL_UNSIGNED_INT,
};

static GLenum get_gl_indices_type(int indices_format)
{
    return gl_indices_type_map[indices_format];
}

static void bind_vertex_attribs(const struct pipeline *s, struct glcontext *gl)
{
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    const struct pipeline_gl *s_priv = (const struct pipeline_gl *)s;

    ngli_glBindVertexArray(gl, s_priv->vao_id);

    const struct buffer **vertex_buffers = gpu_ctx_gl->vertex_buffers;
    const struct attribute_binding_gl *bindings = ngli_darray_data(&s_priv->attribute_bindings);
    for (size_t i = 0; i < ngli_darray_count(&s_priv->attribute_bindings); i++) {
        const struct buffer_gl *buffer_gl = (const struct buffer_gl *)vertex_buffers[i];
        const struct attribute_binding_gl *attribute_binding = &bindings[i];
        const GLuint location = attribute_binding->desc.location;
        const GLuint size = ngli_format_get_nb_comp(attribute_binding->desc.format);
        const GLsizei stride = (GLsizei)attribute_binding->desc.stride;
        ngli_glBindBuffer(gl, GL_ARRAY_BUFFER, buffer_gl->id);
        ngli_glVertexAttribPointer(gl, location, size, GL_FLOAT, GL_FALSE, stride, (void*)(uintptr_t)(attribute_binding->desc.offset));
    }
}

static int pipeline_graphics_init(struct pipeline *s)
{
    int ret = build_attribute_bindings(s);
    if (ret < 0)
        return ret;

    return 0;
}

static int pipeline_compute_init(struct pipeline *s)
{
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    ngli_assert(NGLI_HAS_ALL_FLAGS(gl->features, NGLI_FEATURE_GL_COMPUTE_SHADER_ALL));

    return 0;
}

#if 0
static GLbitfield get_memory_barriers(const struct pipeline *s)
{
    const struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    const struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct pipeline_gl *s_priv = (const struct pipeline_gl *)s;

    GLbitfield barriers = 0;
    const struct buffer_binding_gl *buffer_bindings = ngli_darray_data(&s_priv->buffer_bindings);
    for (size_t i = 0; i < ngli_darray_count(&s_priv->buffer_bindings); i++) {
        const struct buffer_binding_gl *binding = &buffer_bindings[i];
        const struct buffer_gl *buffer_gl = (const struct buffer_gl *)binding->buffer;
        if (!buffer_gl)
            continue;
        const struct pipeline_buffer_desc *desc = &binding->entry;
        if (desc->access & NGLI_ACCESS_WRITE_BIT)
            barriers |= buffer_gl->barriers;
    }

    const struct texture_binding_gl *texture_bindings = ngli_darray_data(&s_priv->texture_bindings);
    for (size_t i = 0; i < ngli_darray_count(&s_priv->texture_bindings); i++) {
        const struct texture_binding_gl *binding = &texture_bindings[i];
        const struct texture_gl *texture_gl = (const struct texture_gl *)binding->texture;
        if (!texture_gl)
            continue;
        const struct pipeline_texture_desc *desc = &binding->desc;
        if (desc->access & NGLI_ACCESS_WRITE_BIT)
            barriers |= texture_gl->barriers;
        if (gl->workaround_radeonsi_sync)
            barriers |= (texture_gl->barriers & GL_FRAMEBUFFER_BARRIER_BIT);
    }

    return barriers;
}
#endif

struct pipeline *ngli_pipeline_gl_create(struct gpu_ctx *gpu_ctx)
{
    struct pipeline_gl *s = ngli_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->parent.gpu_ctx = gpu_ctx;
    return (struct pipeline *)s;
}

int ngli_pipeline_gl_init(struct pipeline *s, const struct pipeline_params *params)
{
    struct pipeline_gl *s_priv = (struct pipeline_gl *)s;

    s->type     = params->type;
    s->graphics = params->graphics;
    s->program  = params->program;
    int ret = ngli_pipeline_layout_copy(&s->layout, &params->layout);
    if (ret < 0)
        return ret;

    ngli_darray_init(&s_priv->attribute_bindings, sizeof(struct attribute_binding_gl), 0);

    if (params->type == NGLI_PIPELINE_TYPE_GRAPHICS) {
        ret = pipeline_graphics_init(s);
        if (ret < 0)
            return ret;
    } else if (params->type == NGLI_PIPELINE_TYPE_COMPUTE) {
        ret = pipeline_compute_init(s);
        if (ret < 0)
            return ret;
    } else {
        ngli_assert(0);
    }

    return 0;
}

static void get_scissor(struct pipeline *s, int32_t *scissor)
{
    struct gpu_ctx *gpu_ctx = s->gpu_ctx;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)gpu_ctx;
    const struct ngl_config *config = &gpu_ctx->config;
    struct rendertarget *rendertarget = gpu_ctx_gl->current_rt;

    memcpy(scissor, gpu_ctx_gl->scissor, sizeof(gpu_ctx_gl->scissor));
    if (config->offscreen) {
        scissor[1] = NGLI_MAX(rendertarget->height - scissor[1] - scissor[3], 0);
    }
}

static void set_graphics_state(struct pipeline *s)
{
    struct gpu_ctx *gpu_ctx = s->gpu_ctx;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct glstate *glstate = &gpu_ctx_gl->glstate;
    struct pipeline_graphics *graphics = &s->graphics;

    ngli_glstate_update(gl, glstate, &graphics->state);
    int32_t scissor[4];
    get_scissor(s, scissor);
    ngli_glstate_update_viewport(gl, glstate, gpu_ctx_gl->viewport);
    ngli_glstate_update_scissor(gl, glstate, scissor);
}

void ngli_pipeline_gl_draw(struct pipeline *s, int nb_vertices, int nb_instances)
{
    struct gpu_ctx *gpu_ctx = s->gpu_ctx;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct glstate *glstate = &gpu_ctx_gl->glstate;
    struct pipeline_graphics *graphics = &s->graphics;
    struct program_gl *program_gl = (struct program_gl *)s->program;

    set_graphics_state(s);
    ngli_glstate_use_program(gl, glstate, program_gl->id);

    bind_vertex_attribs(s, gl);

    struct bindgroup *bindgroup = gpu_ctx_gl->current_bindgroup;
    // CHECK COMPATIBILITY
    ngli_bindgroup_gl_insert_memory_barriers(bindgroup);

    const GLenum gl_topology = get_gl_topology(graphics->topology);
    if (nb_instances > 1)
        ngli_glDrawArraysInstanced(gl, gl_topology, 0, nb_vertices, nb_instances);
    else
        ngli_glDrawArrays(gl, gl_topology, 0, nb_vertices);

    ngli_bindgroup_gl_insert_memory_barriers(bindgroup);
}

void ngli_pipeline_gl_draw_indexed(struct pipeline *s, int nb_indices, int nb_instances)
{
    struct gpu_ctx *gpu_ctx = s->gpu_ctx;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct glstate *glstate = &gpu_ctx_gl->glstate;
    struct pipeline_graphics *graphics = &s->graphics;
    struct program_gl *program_gl = (struct program_gl *)s->program;

    set_graphics_state(s);
    ngli_glstate_use_program(gl, glstate, program_gl->id);

    bind_vertex_attribs(s, gl);

    const struct buffer_gl *indices_gl = (const struct buffer_gl *)gpu_ctx_gl->index_buffer;
    const GLenum gl_indices_type = get_gl_indices_type(gpu_ctx_gl->index_format);
    ngli_glBindBuffer(gl, GL_ELEMENT_ARRAY_BUFFER, indices_gl->id);

    struct bindgroup *bindgroup = gpu_ctx_gl->current_bindgroup;
    // CHECK COMPATIBILITY
    ngli_bindgroup_gl_insert_memory_barriers(bindgroup);

    const GLenum gl_topology = get_gl_topology(graphics->topology);
    if (nb_instances > 1)
        ngli_glDrawElementsInstanced(gl, gl_topology, nb_indices, gl_indices_type, 0, nb_instances);
    else
        ngli_glDrawElements(gl, gl_topology, nb_indices, gl_indices_type, 0);

    ngli_bindgroup_gl_insert_memory_barriers(bindgroup);
}

void ngli_pipeline_gl_dispatch(struct pipeline *s, uint32_t nb_group_x, uint32_t nb_group_y, uint32_t nb_group_z)
{
    struct gpu_ctx *gpu_ctx = s->gpu_ctx;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct glstate *glstate = &gpu_ctx_gl->glstate;
    struct program_gl *program_gl = (struct program_gl *)s->program;

    ngli_glstate_use_program(gl, glstate, program_gl->id);

    struct bindgroup *bindgroup = gpu_ctx_gl->current_bindgroup;
    // CHECK COMPATIBILITY
    ngli_bindgroup_gl_insert_memory_barriers(bindgroup);
    
    ngli_assert(gl->features & NGLI_FEATURE_GL_COMPUTE_SHADER);

    ngli_glDispatchCompute(gl, nb_group_x, nb_group_y, nb_group_z);

    ngli_bindgroup_gl_insert_memory_barriers(bindgroup);
}

void ngli_pipeline_gl_freep(struct pipeline **sp)
{
    if (!*sp)
        return;

    struct pipeline *s = *sp;
    struct pipeline_gl *s_priv = (struct pipeline_gl *)s;

    ngli_pipeline_layout_reset(&s->layout);

    ngli_darray_reset(&s_priv->attribute_bindings);

    struct gpu_ctx *gpu_ctx = s->gpu_ctx;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    ngli_glDeleteVertexArrays(gl, 1, &s_priv->vao_id);

    ngli_freep(sp);
}
