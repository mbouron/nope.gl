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

    ngpu_freep(sp);
}

struct ngpu_fence *ngpu_fence_vk_create(struct ngpu_ctx *ctx)
{
    struct ngpu_fence_vk *s = ngpu_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->parent.rc = NGPU_RC_CREATE(fence_vk_release);
    s->parent.gpu_ctx = ctx;

    return (struct ngpu_fence *)s;
}

int ngpu_fence_vk_reset(struct ngpu_fence *s)
{
    struct ngpu_fence_vk *s_priv = (struct ngpu_fence_vk *)s;
    struct ngpu_ctx_vk *ctx_vk = (struct ngpu_ctx_vk *)s->gpu_ctx;

    s_priv->value = ++ctx_vk->timeline_value;

    return 0;
}

int ngpu_fence_vk_wait(struct ngpu_fence *s)
{
    struct ngpu_fence_vk *s_priv = (struct ngpu_fence_vk *)s;
    struct ngpu_ctx_vk *ctx_vk = (struct ngpu_ctx_vk *)s->gpu_ctx;
    struct vkcontext *vk = ctx_vk->vkcontext;

    const VkSemaphoreWaitInfo wait_info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores    = &ctx_vk->timeline_sem,
        .pValues        = &s_priv->value,
    };
    VkResult res = vk->funcs.WaitSemaphores(vk->device, &wait_info, UINT64_MAX);
    return ngpu_vk_res2ret(res);
}

int ngpu_fence_vk_is_signaled(struct ngpu_fence *s)
{
    struct ngpu_fence_vk *s_priv = (struct ngpu_fence_vk *)s;
    struct ngpu_ctx_vk *ctx_vk = (struct ngpu_ctx_vk *)s->gpu_ctx;
    struct vkcontext *vk = ctx_vk->vkcontext;

    uint64_t value = 0;
    VkResult res = vk->funcs.GetSemaphoreCounterValue(vk->device, ctx_vk->timeline_sem, &value);
    if (res != VK_SUCCESS)
        return 0;
    return value >= s_priv->value;
}

void ngpu_fence_vk_freep(struct ngpu_fence **sp) { NGPU_RC_UNREFP(sp); }
