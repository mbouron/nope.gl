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

#include <string.h>

#include "ctx.h"
#include "utils/utils.h"

#if defined(BACKEND_VK)
#include "vulkan/ctx_vk.h"
static PFN_vkMapMemory map_memory;
static PFN_vkUnmapMemory unmap_memory;
static unsigned map_calls, unmap_calls;
static bool fail_map;

static VKAPI_ATTR VkResult VKAPI_CALL count_map_memory(VkDevice device, VkDeviceMemory memory,
                                                       VkDeviceSize offset, VkDeviceSize size,
                                                       VkMemoryMapFlags flags, void **data)
{
    map_calls++;
    if (fail_map)
        return VK_ERROR_MEMORY_MAP_FAILED;
    return map_memory(device, memory, offset, size, flags, data);
}

static VKAPI_ATTR void VKAPI_CALL count_unmap_memory(VkDevice device, VkDeviceMemory memory)
{
    unmap_calls++;
    unmap_memory(device, memory);
}
#endif

static void test_uploads(struct ngpu_ctx *ctx, uint32_t usage)
{
    const struct ngpu_memory_stats initial = *ngpu_ctx_get_memory_stats(ctx);
#if defined(BACKEND_VK)
    map_calls = unmap_calls = 0;
#endif
    uint8_t expected[256];
    for (size_t i = 0; i < sizeof(expected); i++)
        expected[i] = (uint8_t)i;

    struct ngpu_buffer *buffer = ngpu_buffer_create(ctx);
    ngpu_assert(buffer);
    usage |= NGPU_BUFFER_USAGE_TRANSFER_DST_BIT | NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT
           | NGPU_BUFFER_USAGE_MAP_READ | NGPU_BUFFER_USAGE_MAP_WRITE;
    ngpu_assert(ngpu_buffer_init(buffer, sizeof(expected), usage) == 0);
    ngpu_assert(ngpu_buffer_upload(buffer, expected, 0, NGPU_BUFFER_WHOLE_SIZE) == 0);

    for (size_t i = 0; i < 8; i++) {
        /* Partial uploads must preserve both ends of the allocation. */
        const size_t offset = 17 + i * 13;
        memset(expected + offset, (int)(128 + i), 16);
        ngpu_assert(ngpu_buffer_upload(buffer, expected + offset, offset, 16) == 0);

        /* Explicit maps must also apply their offset and coexist with uploads. */
        uint8_t *data;
        ngpu_assert(ngpu_buffer_map(buffer, 64, 32, (void **)&data) == 0);
        ngpu_assert(!memcmp(data, expected + 64, 32));
        memset(data, (int)i, 32);
        memset(expected + 64, (int)i, 32);
        ngpu_buffer_unmap(buffer);

        ngpu_assert(ngpu_buffer_map(buffer, 0, NGPU_BUFFER_WHOLE_SIZE, (void **)&data) == 0);
        ngpu_assert(!memcmp(data, expected, sizeof(expected)));
        ngpu_buffer_unmap(buffer);
    }
    ngpu_buffer_freep(&buffer);
    ngpu_assert(ngpu_ctx_get_memory_stats(ctx)->buffer_count == initial.buffer_count);
    ngpu_assert(ngpu_ctx_get_memory_stats(ctx)->buffer_bytes == initial.buffer_bytes);
#if defined(BACKEND_VK)
    if (ngpu_ctx_get_backend_type(ctx) == NGPU_BACKEND_VULKAN && usage & NGPU_BUFFER_USAGE_DYNAMIC_BIT) {
        /* The native mapping survives all uploads and public map/unmap pairs. */
        ngpu_assert(map_calls == 1 && unmap_calls == 1);
    }
#endif
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
        .backend=backend, .offscreen=1, .width=1, .height=1, .debug=1,
    };
    struct ngpu_ctx *ctx = ngpu_ctx_create(&params);
    if (!ctx || ngpu_ctx_init(ctx) < 0) {
        ngpu_ctx_freep(&ctx);
        return 77;
    }
#if defined(BACKEND_VK)
    if (backend == NGPU_BACKEND_VULKAN) {
        struct vkcontext *vk = ((struct ngpu_ctx_vk *)ctx)->vkcontext;
        map_memory = vk->funcs.MapMemory;
        unmap_memory = vk->funcs.UnmapMemory;
        vk->funcs.MapMemory = count_map_memory;
        vk->funcs.UnmapMemory = count_unmap_memory;

        /* A failed initial mapping must leave the buffer safe to destroy. */
        fail_map = true;
        struct ngpu_buffer *buffer = ngpu_buffer_create(ctx);
        ngpu_assert(buffer);
        ngpu_assert(ngpu_buffer_init(buffer, 256, NGPU_BUFFER_USAGE_DYNAMIC_BIT |
                                                NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT) < 0);
        ngpu_buffer_freep(&buffer);
        ngpu_assert(map_calls == 1 && unmap_calls == 0);
        fail_map = false;
    }
#endif
    test_uploads(ctx, 0);
    test_uploads(ctx, NGPU_BUFFER_USAGE_DYNAMIC_BIT);
    if (ngpu_ctx_get_features(ctx) & NGPU_FEATURE_BUFFER_MAP_PERSISTENT_BIT)
        test_uploads(ctx, NGPU_BUFFER_USAGE_DYNAMIC_BIT | NGPU_BUFFER_USAGE_MAP_PERSISTENT);
    ngpu_ctx_freep(&ctx);
    return 0;
}
