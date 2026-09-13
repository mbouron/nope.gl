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
    };
    const struct ngpu_pgcraft_params craft_params = {
        .vert_base = "void main() { float x = -1.0 + float((ngl_vertex_index & 1) << 2);"
                     "float y = -1.0 + float((ngl_vertex_index & 2) << 1); ngl_out_pos = vec4(x,y,0.0,1.0); }",
        .frag_base = "void main() { ngl_out_color = vec4(first_ts, second_ts, first_dimensions.x / 16.0, 1.0); }",
        .textures=textures, .nb_textures=2,
    };
    struct ngpu_pgcraft *crafter = ngpu_pgcraft_create(gpu);
    ngli_assert(crafter && ngpu_pgcraft_craft(crafter, &craft_params) == 0);
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
        .resources=ngpu_pgcraft_get_bindgroup_resources(crafter),
        .texture_infos=ngpu_pgcraft_get_texture_infos(crafter),
    };
    ngli_assert(pipeline && ngli_pipeline_init(pipeline, &pipeline_params) == 0);
    struct ngpu_staging_buffer *staging = ngpu_staging_buffer_create(gpu);
    ngli_assert(staging);

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
        struct ngli_image images[2] = {
            {.params={.layout=NGLI_IMAGE_LAYOUT_DEFAULT, .width=4, .height=2}, .planes={texture}, .ts=0.25},
            {.params={.layout=NGLI_IMAGE_LAYOUT_DEFAULT, .width=4, .height=2}, .planes={texture}, .ts=0.5},
        };
        ngpu_ctx_begin_render_pass(gpu, ngpu_ctx_get_default_rendertarget(gpu));
        ngpu_ctx_set_scissor(gpu, &(struct ngpu_scissor){.width=2, .height=1});
        for (unsigned half = 0; half < 2; half++) {
            if (half) {
                /* New metadata for identical planes, plus staging-buffer growth. */
                images[0].ts = 0.75;
                images[0].params.width = 8;
                images[1].ts = 0.25;
                size_t unused_offset;
                ngpu_staging_buffer_reserve(staging, 128 * 1024, &unused_offset);
            }
            ngli_pipeline_update_image(pipeline, 0, &images[0]);
            ngli_pipeline_update_image(pipeline, 1, &images[1]);
            ngpu_ctx_set_viewport(gpu, &(struct ngpu_viewport){.x=(float)half, .width=1, .height=1});
            ngli_pipeline_draw(pipeline, staging, 3, 1, 0);
        }
        /* Discard while both draws are still recorded, then reactivate next frame. */
        ngli_pipeline_discard_resources(pipeline);
        ngpu_texture_freep(&texture);
        ngpu_ctx_end_render_pass(gpu);
        ngli_assert(ngpu_staging_buffer_flush(staging) == 0);
        ngli_assert(ngpu_ctx_end_draw(gpu, 0, NULL, NULL) == 0);
        ngpu_ctx_wait_idle(gpu);
        ngli_assert(!memcmp(pixels, (const uint8_t[]){64, 128, 64, 255, 191, 64, 128, 255}, sizeof(pixels)));
    }
    ngli_pipeline_freep(&pipeline);
    ngpu_pgcraft_freep(&crafter);
    ngpu_staging_buffer_freep(&staging);
    ngpu_ctx_freep(&gpu);
    return 0;
}
