/*
 * Copyright 2026 Matthieu Bouron <matthieu@mojo.video>
 * Copyright 2018-2022 GoPro Inc.
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

#include <stdio.h>
#include <string.h>

#include "bindgroup.h"
#include "ctx.h"
#include "utils/utils.h"

static void test_validation(struct ngpu_ctx *ctx, enum ngpu_type type)
{
    const struct ngpu_limits *limits = ngpu_ctx_get_limits(ctx);
    const bool uniform = type == NGPU_TYPE_UNIFORM_BUFFER || type == NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC;
    const size_t alignment = uniform ? limits->min_uniform_block_offset_alignment : limits->min_storage_block_offset_alignment;
    const size_t stride = NGPU_MAX(16, alignment);
    struct ngpu_buffer *buffer = ngpu_buffer_create(ctx);
    const uint32_t usage = uniform ? NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT : NGPU_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    ngpu_assert(buffer && ngpu_buffer_init(buffer, stride + 16, usage) == 0);
    struct ngpu_bindgroup_layout_entry buffer_entry = {.type=type, .binding=0, .stage_flags=NGPU_PROGRAM_STAGE_FRAGMENT_BIT};
    struct ngpu_bindgroup_layout_entry texture_entry = {.type=NGPU_TYPE_SAMPLER_2D, .binding=1, .stage_flags=NGPU_PROGRAM_STAGE_FRAGMENT_BIT};
    struct ngpu_bindgroup_layout_desc desc = {
        .buffers=&buffer_entry, .nb_buffers=1, .textures=&texture_entry, .nb_textures=1,
    };
    struct ngpu_bindgroup_layout *layout = ngpu_bindgroup_layout_create(ctx);
    ngpu_assert(layout && ngpu_bindgroup_layout_init(layout, &desc) == 0);
    const struct ngpu_texture_params texture_params = {
        .type=NGPU_TEXTURE_TYPE_2D, .format=NGPU_FORMAT_R8G8B8A8_UNORM,
        .width=1, .height=1, .depth=1, .usage=NGPU_TEXTURE_USAGE_SAMPLED_BIT,
    };
    struct ngpu_texture *texture = ngpu_texture_create(ctx);
    ngpu_assert(texture && ngpu_texture_init(texture, &texture_params) == 0);
    const size_t texture_refs = texture->rc.count;
    const struct ngpu_texture_binding texture_binding = {.texture=texture};
    const size_t buffer_refs = buffer->rc.count;
    const struct ngpu_buffer_binding buffer_binding = {.buffer=buffer, .offset=stride, .size=16};
    const struct ngpu_bindgroup_desc bindgroup_desc = {
        .layout=layout, .buffers=&buffer_binding, .nb_buffers=1, .textures=&texture_binding, .nb_textures=1,
    };
    struct ngpu_bindgroup *bindgroup = ngpu_bindgroup_create(ctx, &bindgroup_desc);
    ngpu_assert(bindgroup);
    ngpu_assert(buffer->rc.count == buffer_refs + 1);
    ngpu_assert(texture->rc.count == texture_refs + 1);

    ngpu_bindgroup_freep(&bindgroup);
    ngpu_assert(buffer->rc.count == buffer_refs);
    ngpu_assert(texture->rc.count == texture_refs);
    ngpu_bindgroup_layout_freep(&layout);
    ngpu_texture_freep(&texture);
    ngpu_buffer_freep(&buffer);
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
    uint8_t capture[8] = {0};
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
        .backend=backend, .offscreen=1, .width=2, .height=1, .capture_buffer=capture, .debug=1,
    };
    struct ngpu_ctx *ctx = ngpu_ctx_create(&params);
    if (!ctx || ngpu_ctx_init(ctx) < 0) {
        ngpu_ctx_freep(&ctx);
        return 77;
    }
    test_validation(ctx, NGPU_TYPE_UNIFORM_BUFFER);
    test_validation(ctx, NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC);
    if (ngpu_ctx_get_features(ctx) & NGPU_FEATURE_COMPUTE_BIT) {
        test_validation(ctx, NGPU_TYPE_STORAGE_BUFFER);
        test_validation(ctx, NGPU_TYPE_STORAGE_BUFFER_DYNAMIC);
    }
    ngpu_ctx_freep(&ctx);
    return 0;
}
