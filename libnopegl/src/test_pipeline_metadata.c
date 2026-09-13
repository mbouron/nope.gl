/*
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

#include "config.h"
#include "image.h"
#include "pipeline.h"

static void test_compute(struct ngpu_ctx *gpu)
{
    if (!(ngpu_ctx_get_features(gpu) & NGPU_FEATURE_COMPUTE_BIT))
        return;

    struct ngpu_block_desc block = {0};
    ngpu_block_desc_init(gpu, &block, NGPU_BLOCK_LAYOUT_STD140);
    ngli_assert(ngpu_block_desc_add_field(&block, "value", NGPU_TYPE_VEC4, 0) == 0);
    struct ngpu_buffer *output = ngpu_buffer_create(gpu);
    ngli_assert(output && ngpu_buffer_init(output, 4 * sizeof(float),
                                          NGPU_BUFFER_USAGE_STORAGE_BUFFER_BIT | NGPU_BUFFER_USAGE_MAP_READ) == 0);
    const struct ngpu_pgcraft_block blocks[] = {
        {.name="regular", .type=NGPU_TYPE_UNIFORM_BUFFER, .stage=NGPU_PROGRAM_STAGE_COMP, .block=&block},
        {.name="uniforms", .type=NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC, .stage=NGPU_PROGRAM_STAGE_COMP, .block=&block},
        {.name="storage", .type=NGPU_TYPE_STORAGE_BUFFER_DYNAMIC, .stage=NGPU_PROGRAM_STAGE_COMP, .block=&block},
        {.name="result", .type=NGPU_TYPE_STORAGE_BUFFER, .stage=NGPU_PROGRAM_STAGE_COMP, .block=&block,
         .writable=1},
    };
    const struct ngpu_pgcraft_texture texture = {
        .name="first", .type=NGPU_PGCRAFT_TEXTURE_TYPE_2D, .stage=NGPU_PROGRAM_STAGE_COMP,
    };
    const struct ngpu_pgcraft_params craft_params = {
        .comp_base="void main() { result.value = vec4(uniforms.value.x, storage.value.x, regular.value.x, first_ts); }",
        .workgroup_size={1, 1, 1},
        .blocks=blocks, .nb_blocks=NGLI_ARRAY_NB(blocks),
        .textures=&texture, .nb_textures=1,
    };
    struct ngpu_pgcraft *crafter = ngpu_pgcraft_create(gpu);
    ngli_assert(crafter && ngpu_pgcraft_craft(crafter, &craft_params) == 0);
    const struct ngpu_bindgroup_layout_desc layout = ngpu_pgcraft_get_bindgroup_layout_desc(crafter);
    const struct ngpu_pgcraft_texture_infos infos = ngpu_pgcraft_get_texture_infos(crafter);
    ngli_assert(layout.nb_buffers == 5 && infos.block_index == 4);
    if (ngpu_ctx_get_backend_type(gpu) == NGPU_BACKEND_VULKAN) {
        for (size_t i = 1; i < layout.nb_buffers; i++)
            ngli_assert(layout.buffers[i - 1].binding < layout.buffers[i].binding);

        /* Reject layouts whose array order disagrees with Vulkan's dynamic-offset order. */
        struct ngpu_bindgroup_layout_entry entries[] = {layout.buffers[2], layout.buffers[0], layout.buffers[1]};
        struct ngpu_bindgroup_layout_desc unordered = {.buffers=entries, .nb_buffers=NGLI_ARRAY_NB(entries)};
        struct ngpu_bindgroup_layout *invalid = ngpu_bindgroup_layout_create(gpu);
        ngli_assert(invalid && ngpu_bindgroup_layout_init(invalid, &unordered) == NGPU_ERROR_INVALID_ARG);
        ngpu_bindgroup_layout_freep(&invalid);
    }

    struct ngli_pipeline *pipeline = ngli_pipeline_create(gpu);
    const struct ngli_pipeline_params params = {
        .type=NGPU_PIPELINE_TYPE_COMPUTE,
        .program=ngpu_pgcraft_get_program(crafter),
        .layout_desc=layout,
        .texture_infos=infos,
    };
    ngli_assert(pipeline && ngli_pipeline_init(pipeline, &params) == 0);
    const int32_t output_index = ngpu_pgcraft_get_block_index(crafter, "result", NGPU_PROGRAM_STAGE_COMP);
    ngli_assert(ngli_pipeline_update_buffer(pipeline, output_index, output, 0, 4 * sizeof(float)) == 0);
    struct image_resource *source = ngli_image_resource_create();
    ngli_assert(source && ngli_pipeline_set_image_source(pipeline, 0, source) == 0);
    struct ngpu_staging_buffer *staging = ngpu_staging_buffer_create(gpu);
    ngli_assert(staging);
    const struct pipeline_execution execution = {.staging = staging};

    for (unsigned frame = 0; frame < 2; frame++) {
        ngpu_ctx_advance_frame(gpu);
        ngpu_staging_buffer_reset(staging);
        ngli_assert(ngpu_ctx_begin_draw(gpu) == 0);
        const float values[][4] = {{0.125f}, {0.25f + (float)frame * 0.125f}, {0.5f + (float)frame * 0.125f}};
        for (size_t i = 0; i < NGLI_ARRAY_NB(values); i++) {
            const size_t offset = ngpu_staging_buffer_push(staging, values[i], sizeof(values[i]));
            struct ngpu_buffer *buffer = ngpu_staging_buffer_get_buffer(staging);
            ngli_assert(ngli_pipeline_update_buffer(pipeline, (int32_t)i, buffer, offset, sizeof(values[i])) == 0);
        }
        const struct ngli_image_params image_params = {.ts=frame ? 0.25 : 0.75};
        struct ngli_image *image = ngli_image_create(&image_params);
        ngli_assert(image);
        ngli_image_resource_set(source, image);
        ngli_image_unrefp(&image);
        ngli_assert(ngli_pipeline_dispatch(pipeline, &execution, 1, 1, 1) == 0);
        ngli_assert(ngpu_staging_buffer_flush(staging) == 0);
        ngli_assert(ngpu_ctx_end_draw(gpu, 0, NULL, NULL) == 0);
        ngpu_ctx_wait_idle(gpu);

        const float expected[] = {values[1][0], values[2][0], values[0][0], (float)image_params.ts};
        void *data = NULL;
        ngli_assert(ngpu_buffer_map(output, 0, sizeof(expected), &data) == 0);
        ngli_assert(!memcmp(data, expected, sizeof(expected)));
        ngpu_buffer_unmap(output);
    }

    ngli_image_resource_freep(&source);
    ngli_pipeline_freep(&pipeline);
    ngpu_pgcraft_freep(&crafter);
    ngpu_staging_buffer_freep(&staging);
    ngpu_buffer_freep(&output);
    ngpu_block_desc_reset(&block);
}

int main(int argc, char **argv)
{
    enum ngpu_backend_type backend = NGPU_BACKEND_OPENGL;
    if (argc > 1) {
        if (!strcmp(argv[1], "vulkan"))
            backend = NGPU_BACKEND_VULKAN;
        else if (!strcmp(argv[1], "opengles"))
            backend = NGPU_BACKEND_OPENGLES;
    }
    uint8_t pixels[8] = {0};
    const struct ngpu_ctx_params params = {
#if defined(TARGET_DARWIN)
        .platform=NGPU_PLATFORM_MACOS,
#elif defined(TARGET_IPHONE)
        .platform=NGPU_PLATFORM_IOS,
#elif defined(TARGET_ANDROID)
        .platform=NGPU_PLATFORM_ANDROID,
#elif defined(TARGET_WINDOWS)
        .platform=NGPU_PLATFORM_WINDOWS,
#endif
        .backend=backend, .offscreen=1, .width=2, .height=1, .capture_buffer=pixels, .debug=1,
    };
    struct ngpu_ctx *gpu = ngpu_ctx_create(&params);
    if (!gpu || ngpu_ctx_init(gpu) < 0) {
        ngpu_ctx_freep(&gpu);
        return 77;
    }
    const struct ngpu_pgcraft_texture textures[] = {
        {.name="first", .type=NGPU_PGCRAFT_TEXTURE_TYPE_2D, .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="second", .type=NGPU_PGCRAFT_TEXTURE_TYPE_2D, .stage=NGPU_PROGRAM_STAGE_FRAG},
        {.name="plain", .type=NGPU_PGCRAFT_TEXTURE_TYPE_2D, .stage=NGPU_PROGRAM_STAGE_VERT, .no_metadata=true},
    };
    struct ngpu_block_desc block = {0};
    ngpu_block_desc_init(gpu, &block, NGPU_BLOCK_LAYOUT_STD140);
    ngli_assert(ngpu_block_desc_add_field(&block, "value", NGPU_TYPE_VEC4, 0) == 0);
    const struct ngpu_pgcraft_block blocks[] = {
        {.name="regular", .type=NGPU_TYPE_UNIFORM_BUFFER, .stage=NGPU_PROGRAM_STAGE_FRAG, .block=&block},
        {.name="frag", .type=NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC, .stage=NGPU_PROGRAM_STAGE_FRAG, .block=&block},
        {.name="vert", .type=NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC, .stage=NGPU_PROGRAM_STAGE_VERT, .block=&block},
    };
    const struct ngpu_pgcraft_iovar iovar = {.name="value", .type=NGPU_TYPE_F32};
    const struct ngpu_pgcraft_params craft_params = {
        .vert_base = "void main() { float x = -1.0 + float((ngl_vertex_index & 1) << 2);"
                     "float y = -1.0 + float((ngl_vertex_index & 2) << 1); ngl_out_pos = vec4(x,y,0.0,1.0);"
                     "value = vert.value.x + first_dimensions.x / 16.0; }",
        .frag_base = "void main() { ngl_out_color = vec4(first_ts + frag.value.x, second_ts + regular.value.x, value, 1.0); }",
        .textures=textures, .nb_textures=NGLI_ARRAY_NB(textures),
        .blocks=blocks, .nb_blocks=NGLI_ARRAY_NB(blocks),
        .vert_out_vars=&iovar, .nb_vert_out_vars=1,
    };
    struct ngpu_pgcraft *crafter = ngpu_pgcraft_create(gpu);
    ngli_assert(crafter && ngpu_pgcraft_craft(crafter, &craft_params) == 0);
    const int32_t regular_index = ngpu_pgcraft_get_block_index(crafter, "regular", NGPU_PROGRAM_STAGE_FRAG);
    const int32_t frag_index = ngpu_pgcraft_get_block_index(crafter, "frag", NGPU_PROGRAM_STAGE_FRAG);
    const int32_t vert_index = ngpu_pgcraft_get_block_index(crafter, "vert", NGPU_PROGRAM_STAGE_VERT);
    const struct ngpu_bindgroup_layout_desc layout = ngpu_pgcraft_get_bindgroup_layout_desc(crafter);
    const struct ngpu_pgcraft_texture_infos infos = ngpu_pgcraft_get_texture_infos(crafter);
    ngli_assert(vert_index == 0 && regular_index == 1 && frag_index == 2);
    ngli_assert(layout.nb_buffers == 4 && infos.block_index == 3 && infos.nb_metadata == 2);
    ngli_assert(infos.infos[2].metadata_index == -1);
    ngli_assert(layout.buffers[3].type == NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC);
    ngli_assert(layout.buffers[3].binding > layout.buffers[frag_index].binding);
    struct ngli_pipeline *pipeline = ngli_pipeline_create(gpu);
    const struct ngli_pipeline_params pipeline_params = {
        .type=NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology=NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state.color_write_mask=15,
            .rt_layout=*ngpu_ctx_get_default_rendertarget_layout(gpu),
        },
        .program=ngpu_pgcraft_get_program(crafter),
        .layout_desc=ngpu_pgcraft_get_bindgroup_layout_desc(crafter),
        .texture_infos=ngpu_pgcraft_get_texture_infos(crafter),
    };
    ngli_assert(pipeline && ngli_pipeline_init(pipeline, &pipeline_params) == 0);
    struct ngpu_staging_buffer *staging = ngpu_staging_buffer_create(gpu);
    ngli_assert(staging);

    struct image_resource *sources[NGLI_ARRAY_NB(textures)] = {0};
    for (int32_t i = 0; i < NGLI_ARRAY_NB(sources); i++) {
        sources[i] = ngli_image_resource_create();
        ngli_assert(sources[i]);
        ngli_assert(ngli_pipeline_set_image_source(pipeline, i, sources[i]) == 0);
    }
    const struct pipeline_execution execution = {.staging = staging};

    for (unsigned frame = 0; frame < 2; frame++) {
        ngpu_ctx_advance_frame(gpu);
        ngpu_staging_buffer_reset(staging);
        ngli_assert(ngpu_ctx_begin_draw(gpu) == 0);
        struct ngpu_texture *texture = ngpu_texture_create(gpu);
        const struct ngpu_texture_params texture_params = {
            .type=NGPU_TEXTURE_TYPE_2D, .format=NGPU_FORMAT_R8G8B8A8_UNORM,
            .width=1, .height=1, .depth=1, .usage=NGPU_TEXTURE_USAGE_SAMPLED_BIT,
        };
        ngli_assert(texture && ngpu_texture_init(texture, &texture_params) == 0);
        struct ngli_image_params image_params[2] = {
            {.layout=NGLI_IMAGE_LAYOUT_DEFAULT, .width=4, .height=2, .planes={texture}, .ts=0.25},
            {.layout=NGLI_IMAGE_LAYOUT_DEFAULT, .width=4, .height=2, .planes={texture}, .ts=0.5},
        };
        ngpu_ctx_begin_render_pass(gpu, ngpu_ctx_get_default_rendertarget(gpu));
        ngpu_ctx_set_scissor(gpu, &(struct ngpu_scissor){.width=2, .height=1});
        for (unsigned half = 0; half < 2; half++) {
            if (half) {
                /* New metadata for identical planes, plus staging-buffer growth. */
                image_params[0].ts = 0.75;
                image_params[0].width = 8;
                image_params[1].ts = 0.25;
                size_t unused_offset;
                ngpu_staging_buffer_reserve(staging, 128 * 1024, &unused_offset);
            }
            /* Upload in a different order from the layout, with a regular UBO between dynamic UBOs. */
            const int32_t indices[] = {frag_index, vert_index, regular_index};
            const float values[][4] = {{half ? 0.125f : 0.25f}, {half ? 0.0625f : 0.125f}, {0.125f}};
            for (size_t i = 0; i < NGLI_ARRAY_NB(indices); i++) {
                const size_t offset = ngpu_staging_buffer_push(staging, values[i], sizeof(values[i]));
                struct ngpu_buffer *buffer = ngpu_staging_buffer_get_buffer(staging);
                ngli_assert(ngli_pipeline_update_buffer(pipeline, indices[i], buffer, offset, sizeof(values[i])) == 0);
            }
            for (size_t i = 0; i < 2; i++) {
                struct ngli_image *image = ngli_image_create(&image_params[i]);
                ngli_assert(image);
                ngli_image_resource_set(sources[i], image);
                ngli_image_unrefp(&image);
            }
            ngpu_ctx_set_viewport(gpu, &(struct ngpu_viewport){.x=(float)half, .width=1, .height=1});
            ngli_assert(ngli_pipeline_draw(pipeline, &execution, 3, 1, 0) == 0);
        }
        /* Discard while both draws are still recorded, then reactivate next frame. */
        ngli_pipeline_discard_resources(pipeline);
        for (size_t i = 0; i < 2; i++)
            ngli_image_resource_set(sources[i], NULL);
        ngpu_texture_freep(&texture);
        ngpu_ctx_end_render_pass(gpu);
        ngli_assert(ngpu_staging_buffer_flush(staging) == 0);
        ngli_assert(ngpu_ctx_end_draw(gpu, 0, NULL, NULL) == 0);
        ngpu_ctx_wait_idle(gpu);
        ngli_assert(!memcmp(pixels, (const uint8_t[]){128, 159, 96, 255, 223, 96, 143, 255}, sizeof(pixels)));
    }
    for (size_t i = 0; i < NGLI_ARRAY_NB(sources); i++)
        ngli_image_resource_freep(&sources[i]);
    ngli_pipeline_freep(&pipeline);
    ngpu_pgcraft_freep(&crafter);
    ngpu_staging_buffer_freep(&staging);
    ngpu_block_desc_reset(&block);
    test_compute(gpu);
    ngpu_ctx_freep(&gpu);
    return 0;
}
