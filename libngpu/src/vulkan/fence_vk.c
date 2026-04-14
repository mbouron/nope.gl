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

#include <vulkan/vulkan.h>

#include "utils/log.h"
#include "vulkan/ctx_vk.h"
#include "vulkan/fence_vk.h"
#include "vulkan/vkcontext.h"
#include "vulkan/vkutils.h"
#include "utils/memory.h"
#include "utils/refcount.h"

static void fence_vk_release(void **sp)
{
    struct ngpu_fence_vk *s = *sp;
    if (!s)
        return;

    struct ngpu_ctx_vk *ctx_vk = (struct ngpu_ctx_vk *)s->parent.gpu_ctx;
    struct vkcontext *vk = ctx_vk->vkcontext;
    vk->funcs.DestroyFence(vk->device, s->fence, NULL);

    ngpu_freep(sp);
}

struct ngpu_fence *ngpu_fence_vk_create(struct ngpu_ctx *ctx)
{
    struct ngpu_ctx_vk *ctx_vk = (struct ngpu_ctx_vk *)ctx;
    struct vkcontext *vk = ctx_vk->vkcontext;

    struct ngpu_fence_vk *s = ngpu_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->parent.rc = NGPU_RC_CREATE(fence_vk_release);
    s->parent.gpu_ctx = ctx;

    const VkFenceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VkResult res = vk->funcs.CreateFence(vk->device, &create_info, NULL, &s->fence);
    if (res != VK_SUCCESS) {
        ngpu_free(s);
        return NULL;
    }

    return (struct ngpu_fence *)s;
}

int ngpu_fence_vk_reset(struct ngpu_fence *s)
{
    struct ngpu_fence_vk *s_priv = (struct ngpu_fence_vk *)s;
    struct ngpu_ctx_vk *ctx_vk = (struct ngpu_ctx_vk *)s->gpu_ctx;
    struct vkcontext *vk = ctx_vk->vkcontext;

    vk->funcs.ResetFences(vk->device, 1, &s_priv->fence);

    return 0;
}

int ngpu_fence_vk_wait(struct ngpu_fence *s)
{
    struct ngpu_fence_vk *s_priv = (struct ngpu_fence_vk *)s;
    struct ngpu_ctx_vk *ctx_vk = (struct ngpu_ctx_vk *)s->gpu_ctx;
    struct vkcontext *vk = ctx_vk->vkcontext;

    VkResult res = vk->funcs.WaitForFences(vk->device, 1, &s_priv->fence, VK_TRUE, UINT64_MAX);
    return ngpu_vk_res2ret(res);
}

int ngpu_fence_vk_is_signaled(struct ngpu_fence *s)
{
    struct ngpu_fence_vk *s_priv = (struct ngpu_fence_vk *)s;
    struct ngpu_ctx_vk *ctx_vk = (struct ngpu_ctx_vk *)s->gpu_ctx;
    struct vkcontext *vk = ctx_vk->vkcontext;

    VkResult res = vk->funcs.GetFenceStatus(vk->device, s_priv->fence);
    return res == VK_SUCCESS;
}

void ngpu_fence_vk_freep(struct ngpu_fence **sp) { NGPU_RC_UNREFP(sp); }
