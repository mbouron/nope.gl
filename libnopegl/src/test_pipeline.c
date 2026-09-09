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
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "geometry.h"
#include "image.h"
#include "internal.h"
#include "node_buffer.h"
#include "pipeline.h"

#define WIDTH 16
#define HEIGHT 8
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        abort(); \
    } \
} while (0)

struct fixture {
    struct ngpu_ctx *gpu;
    struct ngpu_staging_buffer *staging;
    uint8_t pixels[WIDTH * HEIGHT * 4];
};

static void begin_frame(struct fixture *f)
{
    ngpu_ctx_advance_frame(f->gpu);
    ngpu_staging_buffer_reset(f->staging);
    CHECK(ngpu_ctx_begin_draw(f->gpu) == 0);
    ngpu_ctx_begin_render_pass(f->gpu, ngpu_ctx_get_default_rendertarget(f->gpu));
    const struct ngpu_scissor scissor = {0, 0, WIDTH, HEIGHT};
    ngpu_ctx_set_scissor(f->gpu, &scissor);
}

static void viewport(struct fixture *f, unsigned half)
{
    const struct ngpu_viewport vp = {(float)(half * WIDTH / 2), 0, WIDTH / 2, HEIGHT};
    ngpu_ctx_set_viewport(f->gpu, &vp);
}

static void end_frame(struct fixture *f)
{
    ngpu_ctx_end_render_pass(f->gpu);
    CHECK(ngpu_staging_buffer_flush(f->staging) == 0);
    CHECK(ngpu_ctx_end_draw(f->gpu, 0, NULL, NULL) == 0);
    ngpu_ctx_wait_idle(f->gpu);
}

static void check_pixel(const struct fixture *f, unsigned half, const uint8_t expected[4])
{
    const uint8_t *pixel = f->pixels + ((HEIGHT / 2) * WIDTH + half * WIDTH / 2 + WIDTH / 4) * 4;
    for (size_t i = 0; i < 4; i++) {
        if (abs((int)pixel[i] - expected[i]) > 2) {
            fprintf(stderr, "half %u: pixel=(%u,%u,%u,%u), expected=(%u,%u,%u,%u)\n",
                    half, pixel[0], pixel[1], pixel[2], pixel[3],
                    expected[0], expected[1], expected[2], expected[3]);
            abort();
        }
    }
}

static struct ngli_pipeline *create_pipeline(struct fixture *f, struct ngpu_pgcraft *crafter)
{
    struct ngli_pipeline *pipeline = ngli_pipeline_create(f->gpu);
    CHECK(pipeline);
    const struct ngli_pipeline_params params = {
        .type = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state = NGPU_GRAPHICS_STATE_DEFAULTS,
            .rt_layout = *ngpu_ctx_get_default_rendertarget_layout(f->gpu),
            .vertex_state = ngpu_pgcraft_get_vertex_state(crafter),
        },
        .program = ngpu_pgcraft_get_program(crafter),
        .layout_desc = ngpu_pgcraft_get_bindgroup_layout_desc(crafter),
        .texture_infos = ngpu_pgcraft_get_texture_infos(crafter),
    };
    CHECK(ngli_pipeline_init(pipeline, &params) == 0);
    return pipeline;
}

static void publish_buffer(struct fixture *f, struct resource *resource, size_t size, const void *data, uint32_t usage)
{
    struct ngpu_buffer *buffer = ngpu_buffer_create(f->gpu);
    CHECK(buffer);
    CHECK(ngpu_buffer_init(buffer, size, usage | NGPU_BUFFER_USAGE_TRANSFER_DST_BIT) == 0);
    CHECK(ngpu_buffer_upload(buffer, data, 0, size) == 0);
    ngli_resource_set_buffer(resource, buffer);
    ngpu_buffer_freep(&buffer);
}

static void test_buffer_sources(struct fixture *f)
{
    const uint64_t initial_buffers = ngpu_ctx_get_memory_stats(f->gpu)->buffer_count;
    struct ngpu_block_desc block = {0};
    ngpu_block_desc_init(f->gpu, &block, NGPU_BLOCK_LAYOUT_STD140);
    CHECK(ngpu_block_desc_add_field(&block, "color", NGPU_TYPE_VEC4, 0) >= 0);
    const struct ngpu_pgcraft_block blocks[] = {{
        .name = "params", .instance_name = "", .stage = NGPU_PROGRAM_STAGE_FRAG,
        .type = NGPU_TYPE_UNIFORM_BUFFER, .block = &block,
    }};
    const struct ngpu_pgcraft_attribute attribute = {
        .name = "position", .type = NGPU_TYPE_VEC2, .format = NGPU_FORMAT_R32G32_SFLOAT,
        .stride = 4 * sizeof(float), .offset = 4 * sizeof(float),
    };
    const struct ngpu_pgcraft_params params = {
        .vert_base = "void main() { ngl_out_pos = vec4(position, 0.0, 1.0); }",
        .frag_base = "void main() { ngl_out_color = color; }",
        .blocks = blocks, .nb_blocks = 1, .attributes = &attribute, .nb_attributes = 1,
    };
    struct ngpu_pgcraft *crafter = ngpu_pgcraft_create(f->gpu);
    CHECK(crafter && ngpu_pgcraft_craft(crafter, &params) == 0);
    struct resource *owner = ngli_resource_create(NGLI_RESOURCE_BUFFER);
    struct resource *indices = ngli_resource_create(NGLI_RESOURCE_BUFFER);
    CHECK(owner && indices);
    /* Exercise the same Block -> Buffer view -> Geometry indirection as nodes. */
    const struct buffer_info view = {
        .resource = owner,
        .layout = {.type = NGPU_TYPE_VEC2, .format = NGPU_FORMAT_R32G32_SFLOAT,
                   .stride = 16, .offset = 16, .count = 3},
    };
    struct geometry *geometry = ngli_geometry_create(f->gpu);
    CHECK(geometry);
    ngli_geometry_set_vertices_buffer(geometry, view.resource, view.layout);
    CHECK(ngli_geometry_init(geometry, NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) == 0);

    struct ngli_pipeline *a = create_pipeline(f, crafter);
    struct ngli_pipeline *b = create_pipeline(f, crafter);
    const int32_t block_index = ngpu_pgcraft_get_block_index(crafter, "params", NGPU_PROGRAM_STAGE_FRAG);
    const int32_t vertex_index = ngpu_pgcraft_get_vertex_buffer_index(crafter, "position");
    const struct pipeline_buffer_source source = {.resource = owner, .size = NGPU_BUFFER_WHOLE_SIZE};
    CHECK(ngli_pipeline_set_buffer_source(a, -1, NULL) == NGL_ERROR_NOT_FOUND);
    CHECK(ngli_pipeline_set_buffer_source(a, 99, &source) == NGL_ERROR_INVALID_ARG);
    CHECK(ngli_pipeline_set_vertex_source(a, 99, owner) == NGL_ERROR_INVALID_ARG);
    struct ngli_pipeline *pipelines[] = {a, b};
    for (size_t i = 0; i < 2; i++) {
        CHECK(ngli_pipeline_set_buffer_source(pipelines[i], block_index, &source) == 0);
        CHECK(ngli_pipeline_set_vertex_source(pipelines[i], vertex_index, geometry->vertices) == 0);
        CHECK(ngli_pipeline_set_index_source(pipelines[i], indices, NGPU_FORMAT_R32_UINT) == 0);
    }
    struct resource *wrong_type = ngli_resource_create(NGLI_RESOURCE_IMAGE);
    CHECK(wrong_type);
    const struct pipeline_buffer_source wrong_source = {.resource = wrong_type, .size = 16};
    CHECK(ngli_pipeline_set_buffer_source(a, block_index, &wrong_source) == NGL_ERROR_INVALID_ARG);
    CHECK(ngli_pipeline_set_vertex_source(a, vertex_index, wrong_type) == NGL_ERROR_INVALID_ARG);
    CHECK(ngli_pipeline_set_index_source(a, wrong_type, NGPU_FORMAT_R32_UINT) == NGL_ERROR_INVALID_ARG);
    ngli_resource_freep(&wrong_type);
    CHECK(ngli_pipeline_update_buffer(a, block_index, NULL, 0, 0) == NGL_ERROR_INVALID_USAGE);
    CHECK(ngli_pipeline_update_vertex_buffer(a, vertex_index, NULL) == NGL_ERROR_INVALID_USAGE);
    CHECK(ngli_pipeline_update_index_buffer(a, NULL, NGPU_FORMAT_R32_UINT) == NGL_ERROR_INVALID_USAGE);

    begin_frame(f);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) < 0);
    CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) < 0);
    CHECK(ngpu_ctx_get_frame_stats(f->gpu)->draw_calls == 0);
    end_frame(f);

    const uint32_t index_data[] = {0, 1, 2};
    publish_buffer(f, indices, sizeof(index_data), index_data, NGPU_BUFFER_USAGE_INDEX_BUFFER_BIT);
    float data[32] = {1, 0, 0, 1, -1, -1, 0, 0, 3, -1, 0, 0, -1, 3, 0, 0};
    const uint32_t usage = NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT | NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    publish_buffer(f, owner, 64, data, usage);
    begin_frame(f);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) == 0);
    /* Replace the allocation while the first draw still retains its inputs. */
    data[0] = 0; data[1] = 1;
    publish_buffer(f, owner, sizeof(data), data, usage);
    viewport(f, 1);
    CHECK(ngli_pipeline_draw_indexed(b, NULL, 3, 1) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){255, 0, 0, 255});
    check_pixel(f, 1, (const uint8_t[]){0, 255, 0, 255});

    /* Both consumers follow the replacement, including a skipped consumer. */
    begin_frame(f);
    viewport(f, 1);
    CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) == 0);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw_indexed(b, NULL, 3, 1) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){0, 255, 0, 255});
    check_pixel(f, 1, (const uint8_t[]){0, 255, 0, 255});

    /* Content updates do not reconstruct the buffer from its initial CPU data. */
    data[1] = 0; data[2] = 1;
    CHECK(ngpu_buffer_upload(ngli_resource_get_buffer(owner), data, 0, sizeof(data)) == 0);
    begin_frame(f);
    viewport(f, 0);
    for (size_t i = 0; i < 40; i++)
        CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) == 0);
    viewport(f, 1);
    CHECK(ngli_pipeline_draw_indexed(b, NULL, 3, 1) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){0, 0, 255, 255});
    check_pixel(f, 1, (const uint8_t[]){0, 0, 255, 255});

    /* Range errors repeat without drawing stale bindings, and are recoverable. */
    const struct pipeline_buffer_source invalid = {.resource = owner, .offset = SIZE_MAX, .size = 16};
    CHECK(ngli_pipeline_set_buffer_source(a, block_index, &invalid) == 0);
    begin_frame(f);
    CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) < 0);
    CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) < 0);
    CHECK(ngpu_ctx_get_frame_stats(f->gpu)->draw_calls == 0);
    CHECK(ngli_pipeline_set_buffer_source(a, block_index, &source) == 0);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){0, 0, 255, 255});

    ngli_resource_clear(owner);
    begin_frame(f);
    CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) < 0);
    CHECK(ngli_pipeline_draw_indexed(b, NULL, 3, 1) < 0);
    CHECK(ngpu_ctx_get_frame_stats(f->gpu)->draw_calls == 0);
    end_frame(f);
    data[0] = 1; data[2] = 0;
    publish_buffer(f, owner, 64, data, usage); /* Whole-size binding shrinks. */
    ngli_pipeline_discard_resources(a);
    ngli_pipeline_discard_resources(b);
    begin_frame(f);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) == 0);
    viewport(f, 1);
    CHECK(ngli_pipeline_draw_indexed(b, NULL, 3, 1) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){255, 0, 0, 255});
    check_pixel(f, 1, (const uint8_t[]){255, 0, 0, 255});

    /* Direct mode requires the caller to supply its value after cache discard. */
    CHECK(ngli_pipeline_set_buffer_source(a, block_index, NULL) == 0);
    CHECK(ngli_pipeline_update_buffer(a, block_index, ngli_resource_get_buffer(owner), 0, 0) == 0);
    CHECK(ngli_pipeline_set_buffer_source(a, block_index, &source) == NGL_ERROR_INVALID_USAGE);
    ngli_pipeline_discard_resources(a);
    begin_frame(f);
    CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) < 0);
    CHECK(ngli_pipeline_update_buffer(a, block_index, ngli_resource_get_buffer(owner), 0, 16) == 0);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw_indexed(a, NULL, 3, 1) == 0);
    end_frame(f);

    ngli_resource_freep(&owner);
    ngli_resource_freep(&indices);
    ngli_geometry_freep(&geometry);
    ngli_pipeline_discard_resources(b);
    begin_frame(f);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw_indexed(b, NULL, 3, 1) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){255, 0, 0, 255});
    ngli_pipeline_freep(&a);
    ngli_pipeline_freep(&b);
    ngpu_pgcraft_freep(&crafter);
    ngpu_block_desc_reset(&block);
    /* Retire recorded work before checking the cache's allocation ownership. */
    for (size_t i = 0; i <= ngpu_ctx_get_nb_in_flight_frames(f->gpu); i++) {
        begin_frame(f);
        end_frame(f);
    }
    fprintf(stderr, "buffer retention: before=%llu after=%llu bytes=%llu\n",
            (unsigned long long)initial_buffers,
            (unsigned long long)ngpu_ctx_get_memory_stats(f->gpu)->buffer_count,
            (unsigned long long)ngpu_ctx_get_memory_stats(f->gpu)->buffer_bytes);
    CHECK(ngpu_ctx_get_memory_stats(f->gpu)->buffer_count == initial_buffers);
}

static struct ngpu_texture *create_texture(struct fixture *f, const uint8_t rgba[4])
{
    struct ngpu_texture *texture = ngpu_texture_create(f->gpu);
    const struct ngpu_texture_params params = {
        .type = NGPU_TEXTURE_TYPE_2D, .format = NGPU_FORMAT_R8G8B8A8_UNORM,
        .width = 1, .height = 1,
        .usage = NGPU_TEXTURE_USAGE_SAMPLED_BIT | NGPU_TEXTURE_USAGE_TRANSFER_DST_BIT,
    };
    CHECK(texture && ngpu_texture_init(texture, &params) == 0);
    CHECK(ngpu_texture_upload(texture, rgba, 0) == 0);
    return texture;
}

static void test_image_sources(struct fixture *f)
{
    const struct ngpu_pgcraft_texture texture_desc = {
        .name = "tex", .type = NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO,
        .stage = NGPU_PROGRAM_STAGE_FRAG,
    };
    const struct ngpu_pgcraft_params params = {
        .vert_base = "void main() { vec2 p = vec2((ngl_vertex_index << 1) & 2, ngl_vertex_index & 2); ngl_out_pos = vec4(p * 2.0 - 1.0, 0.0, 1.0); }",
        .frag_base = "void main() { vec4 c = ngl_texvideo(tex, vec2(0.5)); ngl_out_color = vec4(tex_ts, tex_coord_matrix[3].x, c.b, c.a); }",
        .textures = &texture_desc, .nb_textures = 1,
    };
    struct ngpu_pgcraft *crafter = ngpu_pgcraft_create(f->gpu);
    CHECK(crafter && ngpu_pgcraft_craft(crafter, &params) == 0);
    struct ngli_pipeline *a = create_pipeline(f, crafter);
    struct ngli_pipeline *b = create_pipeline(f, crafter);
    struct ngpu_texture *texture = create_texture(f, (const uint8_t[]){0, 0, 64, 255});
    struct ngpu_texture *chroma = create_texture(f, (const uint8_t[]){64, 192, 0, 255});
    struct image image;
    const struct image_params image_params = {
        .layout = NGLI_IMAGE_LAYOUT_DEFAULT, .width = 1, .height = 1,
        .color_scale = 1, .color_info = NGLI_COLOR_INFO_DEFAULTS,
    };
    ngli_image_init(&image, &image_params, &texture);
    image.ts = .25f;
    image.coordinates_matrix.m[12] = .5f;
    struct image *selected = malloc(sizeof(*selected));
    CHECK(selected);
    *selected = image;
    struct resource *source_a = ngli_resource_create(NGLI_RESOURCE_IMAGE);
    struct resource *source_b = ngli_resource_create(NGLI_RESOURCE_IMAGE);
    struct resource *raw = ngli_resource_create(NGLI_RESOURCE_TEXTURE);
    CHECK(source_a && source_b && raw);
    ngli_resource_set_image(source_a, &image);
    ngli_resource_set_image(source_b, selected);
    ngli_resource_set_texture(raw, texture);
    CHECK(ngli_pipeline_set_image_source(a, 0, source_a) == 0);
    CHECK(ngli_pipeline_set_image_source(b, 0, source_b) == 0);
    CHECK(ngli_pipeline_set_image_source(a, -1, NULL) == NGL_ERROR_NOT_FOUND);
    CHECK(ngli_pipeline_set_image_source(a, 1, source_a) == NGL_ERROR_INVALID_ARG);
    CHECK(ngli_pipeline_set_image_source(a, 0, raw) == NGL_ERROR_INVALID_ARG);
    const struct ngpu_pgcraft_texture_info *info = ngpu_pgcraft_get_texture_infos(crafter).infos;
    CHECK(ngli_pipeline_update_texture(a, info->sampler_index, texture) == NGL_ERROR_INVALID_USAGE);
    CHECK(ngli_pipeline_update_buffer(a, info->block_index, NULL, 0, 0) == NGL_ERROR_INVALID_USAGE);
    CHECK(ngli_pipeline_set_texture_source(a, info->sampler_index, raw) == NGL_ERROR_INVALID_USAGE);
    const struct pipeline_execution execution = {.staging = f->staging};
    begin_frame(f);
    CHECK(ngli_pipeline_draw(a, NULL, 3, 1, 0) < 0); /* Metadata requires staging. */
    viewport(f, 0);
    ngli_resource_set_image(source_a, &image);
    CHECK(ngli_pipeline_draw(a, &execution, 3, 1, 0) == 0);
    viewport(f, 1);
    CHECK(ngli_pipeline_draw(b, &execution, 3, 1, 0) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){64, 128, 64, 255});
    check_pixel(f, 1, (const uint8_t[]){64, 128, 64, 255});
    CHECK(image.coordinates_matrix.m[12] == .5f);

    /* Same planes, different metadata within one frame. */
    begin_frame(f);
    viewport(f, 0);
    image.ts = .5f;
    ngli_resource_set_image(source_a, &image);
    CHECK(ngli_pipeline_draw(a, &execution, 3, 1, 0) == 0);
    viewport(f, 1);
    image.ts = .75f;
    image.coordinates_matrix.m[12] = .25f;
    ngli_resource_set_image(source_a, &image);
    CHECK(ngli_pipeline_draw(a, &execution, 3, 1, 0) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){128, 128, 64, 255});
    check_pixel(f, 1, (const uint8_t[]){191, 64, 64, 255});

    /* Multi-plane selection then single-plane selection clears obsolete slots. */
    image.params.layout = NGLI_IMAGE_LAYOUT_NV12;
    image.planes[1] = chroma;
    image.nb_planes = 2;
    begin_frame(f);
    viewport(f, 0);
    ngli_resource_set_image(source_a, &image);
    CHECK(ngli_pipeline_draw(a, &execution, 3, 1, 0) == 0);
    image.params.layout = NGLI_IMAGE_LAYOUT_DEFAULT;
    image.planes[1] = NULL;
    image.nb_planes = 1;
    viewport(f, 1);
    ngli_resource_set_image(source_a, &image);
    CHECK(ngli_pipeline_draw(a, &execution, 3, 1, 0) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){191, 64, 192, 255});
    check_pixel(f, 1, (const uint8_t[]){191, 64, 64, 255});

    /* Publication owns a snapshot even after the CPU image owner is freed. */
    ngli_resource_clear(source_b);
    free(selected);
    selected = malloc(sizeof(*selected));
    CHECK(selected);
    *selected = image;
    selected->ts = .5f;
    selected->coordinates_matrix.m[12] = .5f;
    ngli_resource_set_image(source_b, selected);
    free(selected);
    selected = NULL;
    begin_frame(f);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw(b, &execution, 3, 1, 0) == 0);
    ngli_resource_clear(source_b);
    viewport(f, 1);
    CHECK(ngli_pipeline_draw(b, &execution, 3, 1, 0) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){128, 128, 64, 255});
    check_pixel(f, 1, (const uint8_t[]){0, 0, 0, 0});

    /* The source and plane outlive both producer references and CPU storage. */
    ngli_resource_set_image(source_a, ngli_resource_get_image(source_a));
    ngli_resource_freep(&source_a);
    ngli_resource_freep(&raw);
    ngpu_texture_freep(&texture);
    ngli_pipeline_discard_resources(a);
    begin_frame(f);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw(a, &execution, 3, 1, 0) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){191, 64, 64, 255});
    ngli_resource_freep(&source_b);
    ngli_pipeline_freep(&a);
    ngli_pipeline_freep(&b);
    ngpu_texture_freep(&chroma);
    ngpu_texture_freep(&texture);
    ngpu_pgcraft_freep(&crafter);
}

static void test_texture_sources(struct fixture *f)
{
    const struct ngpu_pgcraft_texture texture_desc = {
        .name = "tex", .type = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
        .stage = NGPU_PROGRAM_STAGE_FRAG, .no_metadata = true,
    };
    const struct ngpu_pgcraft_params params = {
        .vert_base = "void main() { vec2 p = vec2((ngl_vertex_index << 1) & 2, ngl_vertex_index & 2); ngl_out_pos = vec4(p * 2.0 - 1.0, 0.0, 1.0); }",
        .frag_base = "void main() { ngl_out_color = texture(tex, vec2(0.5)); }",
        .textures = &texture_desc, .nb_textures = 1,
    };
    struct ngpu_pgcraft *crafter = ngpu_pgcraft_create(f->gpu);
    CHECK(crafter && ngpu_pgcraft_craft(crafter, &params) == 0);
    struct ngli_pipeline *pipeline = create_pipeline(f, crafter);
    struct resource *source = ngli_resource_create(NGLI_RESOURCE_TEXTURE);
    struct resource *wrong_type = ngli_resource_create(NGLI_RESOURCE_BUFFER);
    CHECK(source && wrong_type);
    const int32_t index = ngpu_pgcraft_get_texture_infos(crafter).infos[0].sampler_index;
    CHECK(ngli_pipeline_set_texture_source(pipeline, index, source) == 0);
    CHECK(ngli_pipeline_set_texture_source(pipeline, index, wrong_type) == NGL_ERROR_INVALID_ARG);
    ngli_resource_freep(&wrong_type);

    struct ngpu_texture *texture = create_texture(f, (const uint8_t[]){255, 0, 0, 255});
    ngli_resource_set_texture(source, texture);
    ngpu_texture_freep(&texture);
    begin_frame(f);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw(pipeline, NULL, 3, 1, 0) == 0);
    texture = create_texture(f, (const uint8_t[]){0, 255, 0, 255});
    ngli_resource_set_texture(source, texture);
    ngpu_texture_freep(&texture);
    viewport(f, 1);
    CHECK(ngli_pipeline_draw(pipeline, NULL, 3, 1, 0) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){255, 0, 0, 255});
    check_pixel(f, 1, (const uint8_t[]){0, 255, 0, 255});

    ngli_resource_set_texture(source, ngli_resource_get_texture(source));
    ngli_resource_freep(&source);
    ngli_pipeline_discard_resources(pipeline);
    begin_frame(f);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw(pipeline, NULL, 3, 1, 0) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){0, 255, 0, 255});
    ngli_pipeline_freep(&pipeline);
    ngpu_pgcraft_freep(&crafter);
}

static void test_dynamic_ranges(struct fixture *f)
{
    struct ngpu_block_desc block = {0};
    ngpu_block_desc_init(f->gpu, &block, NGPU_BLOCK_LAYOUT_STD140);
    CHECK(ngpu_block_desc_add_field(&block, "color", NGPU_TYPE_VEC4, 0) >= 0);
    const struct ngpu_pgcraft_block block_desc = {
        .name = "params", .instance_name = "", .stage = NGPU_PROGRAM_STAGE_FRAG,
        .type = NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC, .block = &block,
    };
    const struct ngpu_pgcraft_params params = {
        .vert_base = "void main() { vec2 p = vec2((ngl_vertex_index << 1) & 2, ngl_vertex_index & 2); ngl_out_pos = vec4(p * 2.0 - 1.0, 0.0, 1.0); }",
        .frag_base = "void main() { ngl_out_color = color; }",
        .blocks = &block_desc, .nb_blocks = 1,
    };
    struct ngpu_pgcraft *crafter = ngpu_pgcraft_create(f->gpu);
    CHECK(crafter && ngpu_pgcraft_craft(crafter, &params) == 0);
    struct ngli_pipeline *pipeline = create_pipeline(f, crafter);
    struct resource *owner = ngli_resource_create(NGLI_RESOURCE_BUFFER);
    CHECK(owner);
    const size_t alignment = NGLI_MAX(16, ngpu_ctx_get_limits(f->gpu)->min_uniform_block_offset_alignment);
    const size_t size = alignment + 16;
    uint8_t *data = calloc(1, size);
    CHECK(data);
    memcpy(data, (const float[]){1, 0, 0, 1}, 16);
    memcpy(data + alignment, (const float[]){0, 1, 0, 1}, 16);
    publish_buffer(f, owner, size, data, NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    free(data);
    const struct pipeline_buffer_source source = {.resource = owner, .size = 16};
    const int32_t index = ngpu_pgcraft_get_block_index(crafter, "params", NGPU_PROGRAM_STAGE_FRAG);
    CHECK(ngli_pipeline_set_buffer_source(pipeline, index, &source) == 0);

    begin_frame(f);
    CHECK(ngli_pipeline_draw(pipeline, NULL, 3, 1, 0) < 0); /* Missing dynamic offset */
    uint32_t offset = (uint32_t)alignment * 2;
    CHECK(ngli_pipeline_update_dynamic_offsets(pipeline, &offset, 1) == 0);
    CHECK(ngli_pipeline_draw(pipeline, NULL, 3, 1, 0) < 0); /* Effective range overflows */
    if (ngpu_ctx_get_limits(f->gpu)->min_uniform_block_offset_alignment > 1) {
        offset = 1;
        CHECK(ngli_pipeline_update_dynamic_offsets(pipeline, &offset, 1) == 0);
        CHECK(ngli_pipeline_draw(pipeline, NULL, 3, 1, 0) < 0); /* Misalignment */
    }
    CHECK(ngpu_ctx_get_frame_stats(f->gpu)->draw_calls == 0);
    offset = 0;
    CHECK(ngli_pipeline_update_dynamic_offsets(pipeline, &offset, 1) == 0);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw(pipeline, NULL, 3, 1, 0) == 0);
    offset = (uint32_t)alignment;
    CHECK(ngli_pipeline_update_dynamic_offsets(pipeline, &offset, 1) == 0);
    viewport(f, 1);
    CHECK(ngli_pipeline_draw(pipeline, NULL, 3, 1, 0) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){255, 0, 0, 255});
    check_pixel(f, 1, (const uint8_t[]){0, 255, 0, 255});

    ngli_pipeline_discard_resources(pipeline);
    begin_frame(f);
    CHECK(ngli_pipeline_draw(pipeline, NULL, 3, 1, 0) < 0);
    CHECK(ngli_pipeline_update_dynamic_offsets(pipeline, &offset, 1) == 0);
    viewport(f, 0);
    CHECK(ngli_pipeline_draw(pipeline, NULL, 3, 1, 0) == 0);
    end_frame(f);
    check_pixel(f, 0, (const uint8_t[]){0, 255, 0, 255});
    ngli_pipeline_freep(&pipeline);
    ngli_resource_freep(&owner);
    ngpu_pgcraft_freep(&crafter);
    ngpu_block_desc_reset(&block);
}

static void test_required_storage_image(struct fixture *f)
{
    if (!(ngpu_ctx_get_features(f->gpu) & NGPU_FEATURE_COMPUTE_BIT)) {
        fprintf(stderr, "storage-image validation skipped: compute unavailable\n");
        return;
    }
    const struct ngpu_pgcraft_texture texture_desc = {
        .name = "tex", .type = NGPU_PGCRAFT_TEXTURE_TYPE_IMAGE_2D,
        .stage = NGPU_PROGRAM_STAGE_COMP, .format = NGPU_FORMAT_R8G8B8A8_UNORM,
        .writable = 1, .no_metadata = true,
    };
    const struct ngpu_pgcraft_params params = {
        .comp_base = "void main() { imageStore(tex, ivec2(0), vec4(1.0)); }",
        .textures = &texture_desc, .nb_textures = 1, .workgroup_size = {1, 1, 1},
    };
    struct ngpu_pgcraft *crafter = ngpu_pgcraft_create(f->gpu);
    CHECK(crafter && ngpu_pgcraft_craft(crafter, &params) == 0);
    struct ngli_pipeline *pipeline = ngli_pipeline_create(f->gpu);
    const struct ngli_pipeline_params pipeline_params = {
        .type = NGPU_PIPELINE_TYPE_COMPUTE,
        .program = ngpu_pgcraft_get_program(crafter),
        .layout_desc = ngpu_pgcraft_get_bindgroup_layout_desc(crafter),
        .texture_infos = ngpu_pgcraft_get_texture_infos(crafter),
    };
    CHECK(pipeline && ngli_pipeline_init(pipeline, &pipeline_params) == 0);
    struct resource *source = ngli_resource_create(NGLI_RESOURCE_IMAGE);
    CHECK(source);
    CHECK(ngli_pipeline_set_image_source(pipeline, 0, source) == 0);
    ngpu_ctx_advance_frame(f->gpu);
    CHECK(ngpu_ctx_begin_update(f->gpu) == 0);
    CHECK(ngli_pipeline_dispatch(pipeline, NULL, 1, 1, 1) < 0);
    CHECK(ngli_pipeline_dispatch(pipeline, NULL, 1, 1, 1) < 0);
    CHECK(ngpu_ctx_get_frame_stats(f->gpu)->compute_dispatches == 0);
    CHECK(ngpu_ctx_end_update(f->gpu, NULL) == 0);

    struct ngpu_texture *sampled = create_texture(f, (const uint8_t[]){0, 0, 0, 0});
    struct image image;
    const struct image_params image_params = {
        .layout = NGLI_IMAGE_LAYOUT_DEFAULT, .width = 1, .height = 1,
        .color_scale = 1, .color_info = NGLI_COLOR_INFO_DEFAULTS,
    };
    ngli_image_init(&image, &image_params, &sampled);
    ngli_resource_set_image(source, &image);
    ngpu_ctx_advance_frame(f->gpu);
    CHECK(ngpu_ctx_begin_update(f->gpu) == 0);
    CHECK(ngli_pipeline_dispatch(pipeline, NULL, 1, 1, 1) < 0); /* Incompatible usage */
    CHECK(ngpu_ctx_end_update(f->gpu, NULL) == 0);

    struct ngpu_texture *wrong_format = ngpu_texture_create(f->gpu);
    const struct ngpu_texture_params wrong_params = {
        .type = NGPU_TEXTURE_TYPE_2D, .format = NGPU_FORMAT_R32_UINT,
        .width = 1, .height = 1, .usage = NGPU_TEXTURE_USAGE_STORAGE_BIT,
    };
    CHECK(wrong_format && ngpu_texture_init(wrong_format, &wrong_params) == 0);
    image.planes[0] = wrong_format;
    ngli_resource_set_image(source, &image);
    ngpu_ctx_advance_frame(f->gpu);
    CHECK(ngpu_ctx_begin_update(f->gpu) == 0);
    CHECK(ngli_pipeline_dispatch(pipeline, NULL, 1, 1, 1) < 0);
    CHECK(ngpu_ctx_get_frame_stats(f->gpu)->compute_dispatches == 0);
    CHECK(ngpu_ctx_end_update(f->gpu, NULL) == 0);

    struct ngpu_texture *storage = ngpu_texture_create(f->gpu);
    const struct ngpu_texture_params storage_params = {
        .type = NGPU_TEXTURE_TYPE_2D, .format = NGPU_FORMAT_R8G8B8A8_UNORM,
        .width = 1, .height = 1, .usage = NGPU_TEXTURE_USAGE_STORAGE_BIT,
    };
    CHECK(storage && ngpu_texture_init(storage, &storage_params) == 0);
    image.planes[0] = storage;
    ngli_resource_set_image(source, &image);
    ngpu_ctx_advance_frame(f->gpu);
    CHECK(ngpu_ctx_begin_update(f->gpu) == 0);
    CHECK(ngli_pipeline_dispatch(pipeline, NULL, 1, 1, 1) == 0);
    CHECK(ngpu_ctx_get_frame_stats(f->gpu)->compute_dispatches == 1);
    CHECK(ngpu_ctx_end_update(f->gpu, NULL) == 0);
    ngpu_ctx_wait_idle(f->gpu);
    ngli_resource_freep(&source);
    ngli_pipeline_freep(&pipeline);
    ngpu_texture_freep(&sampled);
    ngpu_texture_freep(&wrong_format);
    ngpu_texture_freep(&storage);
    ngpu_pgcraft_freep(&crafter);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    struct fixture f = {0};
    const struct ngpu_ctx_params params = {
#if defined(TARGET_LINUX)
        .platform = NGPU_PLATFORM_XLIB,
#elif defined(TARGET_IPHONE)
        .platform = NGPU_PLATFORM_IOS,
#elif defined(TARGET_DARWIN)
        .platform = NGPU_PLATFORM_MACOS,
#elif defined(TARGET_ANDROID)
        .platform = NGPU_PLATFORM_ANDROID,
#elif defined(TARGET_WINDOWS)
        .platform = NGPU_PLATFORM_WINDOWS,
#else
#error no default platform
#endif
        .backend = (enum ngpu_backend_type)atoi(argv[1]),
        .offscreen = 1, .width = WIDTH, .height = HEIGHT,
        .capture_buffer = f.pixels,
    };
    f.gpu = ngpu_ctx_create(&params);
    if (!f.gpu || ngpu_ctx_init(f.gpu) < 0) {
        ngpu_ctx_freep(&f.gpu);
        return 77;
    }
    f.staging = ngpu_staging_buffer_create(f.gpu);
    CHECK(f.staging);
    for (size_t i = 0; i < ngpu_ctx_get_nb_in_flight_frames(f.gpu); i++) {
        begin_frame(&f);
        end_frame(&f);
    }
    test_buffer_sources(&f);
    test_image_sources(&f);
    test_texture_sources(&f);
    test_dynamic_ranges(&f);
    test_required_storage_image(&f);
    ngpu_ctx_wait_idle(f.gpu);
    ngpu_staging_buffer_freep(&f.staging);
    ngpu_ctx_freep(&f.gpu);
    return 0;
}
