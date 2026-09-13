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

#include <string.h>

#include "image.h"
#include <ngpu/ngpu.h>
#include "nopegl/nopegl.h"
#include "pipeline.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

#define NB_BINDGROUPS 16

NGLI_DECLARE_DARRAY_WITH_NAME(bindgroup_darray, struct ngpu_bindgroup *);

struct ngli_pipeline {
    struct ngpu_ctx *gpu_ctx;
    enum ngpu_pipeline_type type;
    struct ngpu_pipeline_graphics graphics;
    const struct ngpu_program *program;
    struct ngpu_pipeline *pipeline;
    struct ngpu_bindgroup_layout_desc bindgroup_layout_desc;
    struct ngpu_bindgroup_layout *bindgroup_layout;
    struct bindgroup_darray bindgroups;
    struct ngpu_bindgroup *cur_bindgroup;
    size_t cur_bindgroup_index;
    const struct ngpu_buffer **vertex_buffers;
    size_t nb_vertex_buffers;
    struct ngpu_texture_binding *textures;
    size_t nb_textures;
    struct ngpu_buffer_binding *buffers;
    size_t nb_buffers;
    uint32_t dynamic_offsets[NGPU_MAX_DYNAMIC_OFFSETS];
    size_t nb_dynamic_offsets;
    int updated;
    int need_pipeline_recreation;
    struct ngpu_pgcraft_texture_infos texture_infos;
    const struct ngli_image **images;
};

struct ngli_pipeline *ngli_pipeline_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngli_pipeline *s = ngli_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->gpu_ctx = gpu_ctx;
    return s;
}

static void free_bindgroup(void *user_arg, void *data)
{
    struct ngpu_bindgroup **bindgroup = data;
    ngpu_bindgroup_freep(bindgroup);
}

static int grow_bindgroup_array(struct ngli_pipeline *s)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    size_t count = s->bindgroups.count;
    if (count == 0) {
        ngli_darray_set_free_func(&s->bindgroups, free_bindgroup, NULL);
        count = NB_BINDGROUPS;
    }

    for (size_t i = 0; i < count; i++) {
        struct ngpu_bindgroup *bindgroup = ngpu_bindgroup_create(gpu_ctx);
        if (!bindgroup)
            return NGL_ERROR_MEMORY;

        struct ngpu_bindgroup_params params = {
            .layout    = s->bindgroup_layout,
            .resources = {
                .textures    = s->textures,
                .nb_textures = s->nb_textures,
                .buffers     = s->buffers,
                .nb_buffers  = s->nb_buffers,
            },
        };

        int ret = ngpu_bindgroup_init(bindgroup, &params);
        if (ret < 0) {
            ngpu_bindgroup_freep(&bindgroup);
            return ret;
        }

        if (ngli_darray_try_push(&s->bindgroups, bindgroup) < 0) {
            ngpu_bindgroup_freep(&bindgroup);
            return NGL_ERROR_MEMORY;
        }
    }

    return 0;
}

static int create_pipeline(struct ngli_pipeline *s)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    s->bindgroup_layout = ngpu_bindgroup_layout_create(gpu_ctx);
    if (!s->bindgroup_layout)
        return NGL_ERROR_MEMORY;

    int ret = ngpu_bindgroup_layout_init(s->bindgroup_layout, &s->bindgroup_layout_desc);
    if (ret < 0)
        return ret;

    s->pipeline = ngpu_pipeline_create(gpu_ctx);
    if (!s->pipeline)
        return NGL_ERROR_MEMORY;

    const struct ngpu_pipeline_params pipeline_params = {
        .type     = s->type,
        .graphics = s->graphics,
        .program  = s->program,
        .layout   = {
            .bindgroup_layout = s->bindgroup_layout,
        }
    };

    ret = ngpu_pipeline_init(s->pipeline, &pipeline_params);
    if (ret < 0)
        return ret;

    ret = grow_bindgroup_array(s);
    if (ret < 0)
        return ret;

    s->cur_bindgroup = *ngli_darray_get(&s->bindgroups, 0);
    s->cur_bindgroup_index = 0;

    /* Initialize bindgroup before first pipeline execution */
    s->updated = 1;

    return 0;
}

static void reset_pipeline(struct ngli_pipeline *s)
{
    ngpu_pipeline_freep(&s->pipeline);
    ngli_darray_clear(&s->bindgroups);
    s->cur_bindgroup = NULL;
    s->cur_bindgroup_index = 0;
    ngpu_bindgroup_layout_freep(&s->bindgroup_layout);
}

int ngli_pipeline_init(struct ngli_pipeline *s, const struct ngli_pipeline_params *params)
{
    s->type = params->type;

    int ret = ngpu_pipeline_graphics_copy(&s->graphics, &params->graphics);
    if (ret < 0)
        return ret;

    s->program = params->program;

    NGLI_ARRAY_MEMDUP(&s->bindgroup_layout_desc, &params->layout_desc, textures);
    NGLI_ARRAY_MEMDUP(&s->bindgroup_layout_desc, &params->layout_desc, buffers);

    const struct ngpu_bindgroup_resources *bindgroup_resources = &params->resources;
    NGLI_ARRAY_MEMDUP(s, bindgroup_resources, buffers);
    NGLI_ARRAY_MEMDUP(s, bindgroup_resources, textures);

    const struct ngpu_vertex_resources *vertex_resources = &params->vertex_resources;
    NGLI_ARRAY_MEMDUP(s, vertex_resources, vertex_buffers);

    s->texture_infos = params->texture_infos;

    if (s->texture_infos.nb_infos) {
        s->images = ngli_try_calloc(s->texture_infos.nb_infos, sizeof(*s->images));
        if (!s->images)
            return NGL_ERROR_MEMORY;
    }

    ret = create_pipeline(s);
    if (ret < 0)
        return ret;

    return 0;
}

int ngli_pipeline_update_vertex_buffer(struct ngli_pipeline *s, int32_t index, const struct ngpu_buffer *buffer)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;

    ngli_assert(index >= 0 && index < s->nb_vertex_buffers);
    s->vertex_buffers[index] = buffer;
    return 0;
}

static int update_texture(struct ngli_pipeline *s, int32_t index, const struct ngpu_texture_binding *binding)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;

    ngli_assert(index >= 0 && index < s->nb_textures);

    if (s->textures[index].immutable_sampler != binding->immutable_sampler) {
        struct ngpu_bindgroup_layout_entry *entry = &s->bindgroup_layout_desc.textures[index];
        entry->immutable_sampler = binding->immutable_sampler;
        s->need_pipeline_recreation = 1;
    }

    s->textures[index] = *binding;
    s->updated = 1;

    return 0;
}

int ngli_pipeline_update_texture(struct ngli_pipeline *s, int32_t index, const struct ngpu_texture *texture)
{
    const struct ngpu_texture_binding binding = {.texture = texture};
    return update_texture(s, index, &binding);
}

int ngli_pipeline_update_dynamic_offsets(struct ngli_pipeline *s, const uint32_t *offsets, size_t nb_offsets)
{
    ngli_assert(ngpu_bindgroup_layout_get_nb_dynamic_offsets(s->bindgroup_layout) == nb_offsets);
    memcpy(s->dynamic_offsets, offsets, nb_offsets * sizeof(*s->dynamic_offsets));
    s->nb_dynamic_offsets = nb_offsets;
    return 0;
}

static void push_texture_info_block(struct ngli_pipeline *s, struct ngpu_staging_buffer *staging_buffer)
{
    if (!s->texture_infos.nb_metadata)
        return;

    const size_t size = s->texture_infos.nb_metadata * sizeof(struct ngpu_pgcraft_texture_info_block);
    size_t offset = 0;
    struct ngpu_pgcraft_texture_info_block *metadata = ngpu_staging_buffer_reserve(staging_buffer, size, &offset);
    for (size_t i = 0; i < s->texture_infos.nb_infos; i++) {
        const struct ngli_image *image = s->images[i];
        const int32_t index = s->texture_infos.infos[i].metadata_index;
        if (index < 0)
            continue;
        struct ngpu_pgcraft_texture_info_block info = {0};
        if (image) {
            memcpy(info.coord_matrix, image->coordinates_matrix.m, sizeof(info.coord_matrix));
            memcpy(info.color_matrix, image->color_matrix.m, sizeof(info.color_matrix));
            memcpy(info.mapping_color_matrix, image->mapping_color_matrix.m, sizeof(info.mapping_color_matrix));
            if (image->params.layout) {
                info.dimensions[0] = (float)image->params.width;
                info.dimensions[1] = (float)image->params.height;
            }
            info.timestamp = image->ts;
            info.sampling_mode = (int32_t)image->params.layout;
        }
        memcpy(&metadata[index], &info, sizeof(info));
    }
    struct ngpu_buffer *buffer = ngpu_staging_buffer_get_buffer(staging_buffer);
    ngli_pipeline_update_buffer(s, s->texture_infos.block_index, buffer, offset, size);
}

void ngli_pipeline_update_image(struct ngli_pipeline *s, int32_t index, const struct ngli_image *image)
{
    if (index == -1)
        return;

    ngli_assert(index >= 0 && index < s->texture_infos.nb_infos);
    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[index];
    const struct ngpu_texture_binding empty_binding = {0};

    switch (image->params.layout) {
    case NGLI_IMAGE_LAYOUT_DEFAULT:
        update_texture(s, info->sampler_index,        &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_1_index,      &empty_binding);
        update_texture(s, info->sampler_2_index,      &empty_binding);
        update_texture(s, info->sampler_oes_index,    &empty_binding);
        update_texture(s, info->sampler_rect_0_index, &empty_binding);
        update_texture(s, info->sampler_rect_1_index, &empty_binding);
        break;
    case NGLI_IMAGE_LAYOUT_MEDIACODEC:
        update_texture(s, info->sampler_index,        &empty_binding);
        update_texture(s, info->sampler_1_index,      &empty_binding);
        update_texture(s, info->sampler_2_index,      &empty_binding);
        update_texture(s, info->sampler_oes_index,    &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_rect_0_index, &empty_binding);
        update_texture(s, info->sampler_rect_1_index, &empty_binding);
        break;
    case NGLI_IMAGE_LAYOUT_NV12:
        update_texture(s, info->sampler_index,        &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_1_index,      &(struct ngpu_texture_binding){.texture = image->planes[1], .immutable_sampler = image->samplers[1]});
        update_texture(s, info->sampler_2_index,      &empty_binding);
        update_texture(s, info->sampler_oes_index,    &empty_binding);
        update_texture(s, info->sampler_rect_0_index, &empty_binding);
        update_texture(s, info->sampler_rect_1_index, &empty_binding);
        break;
    case NGLI_IMAGE_LAYOUT_NV12_RECTANGLE:
        update_texture(s, info->sampler_index,        &empty_binding);
        update_texture(s, info->sampler_1_index,      &empty_binding);
        update_texture(s, info->sampler_2_index,      &empty_binding);
        update_texture(s, info->sampler_oes_index,    &empty_binding);
        update_texture(s, info->sampler_rect_0_index, &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_rect_1_index, &(struct ngpu_texture_binding){.texture = image->planes[1], .immutable_sampler = image->samplers[1]});
        break;
    case NGLI_IMAGE_LAYOUT_YUV:
        update_texture(s, info->sampler_index,        &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_1_index,      &(struct ngpu_texture_binding){.texture = image->planes[1], .immutable_sampler = image->samplers[1]});
        update_texture(s, info->sampler_2_index,      &(struct ngpu_texture_binding){.texture = image->planes[2], .immutable_sampler = image->samplers[2]});
        update_texture(s, info->sampler_oes_index,    &empty_binding);
        update_texture(s, info->sampler_rect_0_index, &empty_binding);
        update_texture(s, info->sampler_rect_1_index, &empty_binding);
        break;
    case NGLI_IMAGE_LAYOUT_RECTANGLE:
        update_texture(s, info->sampler_index,        &empty_binding);
        update_texture(s, info->sampler_1_index,      &empty_binding);
        update_texture(s, info->sampler_2_index,      &empty_binding);
        update_texture(s, info->sampler_oes_index,    &empty_binding);
        update_texture(s, info->sampler_rect_0_index, &(struct ngpu_texture_binding){.texture = image->planes[0], .immutable_sampler = image->samplers[0]});
        update_texture(s, info->sampler_rect_1_index, &empty_binding);
        break;
    default:
        break;
    }

    s->images[index] = image;
}

int ngli_pipeline_update_buffer(struct ngli_pipeline *s, int32_t index, const struct ngpu_buffer *buffer, size_t offset, size_t size)
{
    if (index == -1)
        return NGL_ERROR_NOT_FOUND;

    ngli_assert(index >= 0 && index < s->nb_buffers);
    s->buffers[index] = (struct ngpu_buffer_binding) {
        .buffer = buffer,
        .offset = offset,
        .size   = size ? size : ngpu_buffer_get_size(buffer),
    };
    s->updated = 1;
    return 0;
}

static int select_next_available_bindgroup(struct ngli_pipeline *s)
{
    /* If current bindgroup is not in use, select it */
    if (ngpu_bindgroup_get_refcount(s->cur_bindgroup) == 1)
        return 0;

    /* Otherwhise, check if next bindgroup is available  */
    size_t bindgroup_index = (s->cur_bindgroup_index + 1) % s->bindgroups.count;
    struct ngpu_bindgroup *bindgroup = *ngli_darray_get(&s->bindgroups, bindgroup_index);
    if (ngpu_bindgroup_get_refcount(bindgroup) == 1) {
        s->cur_bindgroup = bindgroup;
        s->cur_bindgroup_index = bindgroup_index;
        return 0;
    }

    /*
     * If it is not, save next newly-allocated bind group index and increase
     * our bindgroup pool size
     */
    bindgroup_index = s->bindgroups.count;

    int ret = grow_bindgroup_array(s);
    if (ret < 0)
        return ret;

    /* Select bindgroup and assert that it is not in use */
    s->cur_bindgroup = *ngli_darray_get(&s->bindgroups, bindgroup_index);
    s->cur_bindgroup_index = bindgroup_index;
    ngli_assert(ngpu_bindgroup_get_refcount(s->cur_bindgroup) == 1);

    return 0;
}

static int prepare_bindgroup(struct ngli_pipeline *s)
{
    if (!s->updated)
        return 0;

    s->updated = 0;

    if (s->need_pipeline_recreation) {
        s->need_pipeline_recreation = 0;
        reset_pipeline(s);
        int ret = create_pipeline(s);
        if (ret < 0)
            return ret;
    }

    int ret = select_next_available_bindgroup(s);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < s->nb_textures; i++) {
        ret = ngpu_bindgroup_update_texture(s->cur_bindgroup, (int32_t) i, &s->textures[i]);
        if (ret < 0)
            return ret;
    }

    for (size_t i = 0; i < s->nb_buffers; i++) {
        ret = ngpu_bindgroup_update_buffer(s->cur_bindgroup, (int32_t) i, &s->buffers[i]);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int prepare_pipeline(struct ngli_pipeline *s, struct ngpu_staging_buffer *staging_buffer)
{
    push_texture_info_block(s, staging_buffer);

    int ret = prepare_bindgroup(s);
    if (ret < 0)
        return ret;

    return 0;
}

void ngli_pipeline_draw(struct ngli_pipeline *s, struct ngpu_staging_buffer *staging_buffer, uint32_t nb_vertices, uint32_t nb_instances, uint32_t first_vertex)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    int ret = prepare_pipeline(s, staging_buffer);
    if (ret < 0)
        return;

    ngpu_ctx_set_pipeline(gpu_ctx, s->pipeline);
    for (size_t i = 0; i < s->nb_vertex_buffers; i++)
        ngpu_ctx_set_vertex_buffer(gpu_ctx, (uint32_t)i, s->vertex_buffers[i]);
    ngpu_ctx_set_bindgroup(gpu_ctx, s->cur_bindgroup, s->dynamic_offsets, s->nb_dynamic_offsets);
    ngpu_ctx_draw(gpu_ctx, nb_vertices, nb_instances, first_vertex);
}

void ngli_pipeline_draw_indexed(struct ngli_pipeline *s, struct ngpu_staging_buffer *staging, const struct ngpu_buffer *indices, enum ngpu_format indices_format, uint32_t nb_indices, uint32_t nb_instances)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    int ret = prepare_pipeline(s, staging);
    if (ret < 0)
        return;

    ngpu_ctx_set_pipeline(gpu_ctx, s->pipeline);
    for (size_t i = 0; i < s->nb_vertex_buffers; i++)
        ngpu_ctx_set_vertex_buffer(gpu_ctx, (uint32_t)i, s->vertex_buffers[i]);
    ngpu_ctx_set_index_buffer(gpu_ctx, indices, indices_format);
    ngpu_ctx_set_bindgroup(gpu_ctx, s->cur_bindgroup, s->dynamic_offsets, s->nb_dynamic_offsets);
    ngpu_ctx_draw_indexed(gpu_ctx, nb_indices, nb_instances, 0);
}

void ngli_pipeline_dispatch(struct ngli_pipeline *s, struct ngpu_staging_buffer *staging_buffer, uint32_t nb_group_x, uint32_t nb_group_y, uint32_t nb_group_z)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    int ret = prepare_pipeline(s, staging_buffer);
    if (ret < 0)
        return;

    ngpu_ctx_set_pipeline(gpu_ctx, s->pipeline);
    ngpu_ctx_set_bindgroup(gpu_ctx, s->cur_bindgroup, s->dynamic_offsets, s->nb_dynamic_offsets);
    ngpu_ctx_dispatch(gpu_ctx, nb_group_x, nb_group_y, nb_group_z);
}

void ngli_pipeline_freep(struct ngli_pipeline **sp)
{
    struct ngli_pipeline *s = *sp;
    if (!s)
        return;

    reset_pipeline(s);
    ngli_darray_reset(&s->bindgroups);

    ngpu_pipeline_graphics_reset(&s->graphics);

    ngli_freep(&s->bindgroup_layout_desc.textures);
    ngli_freep(&s->bindgroup_layout_desc.buffers);

    ngli_freep(&s->vertex_buffers);
    ngli_freep(&s->textures);
    ngli_freep(&s->buffers);
    ngli_freep(&s->images);

    ngli_freep(sp);
}
