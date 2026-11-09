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
#include "utils/utils.h"
#include "vulkan/cmd_buffer_vk.h"
#include "vulkan/ctx_vk.h"
#include "vulkan/fence_vk.h"
#include "vulkan/priv_vk.h"
#include "vulkan/vkcontext.h"
#include "vulkan/vkutils.h"
#include "utils/memory.h"
#include "utils/refcount.h"

static void release_export_sem(struct ngpu_fence *s)
{
    struct ngpu_fence_vk *s_priv = NGPU_PRIV_VK(s);
    if (s_priv->export_sem == VK_NULL_HANDLE)
        return;
    ngpu_ctx_vk_release_export_sem(s->gpu_ctx, s_priv->export_sem, s_priv->exported);
    s_priv->export_sem = VK_NULL_HANDLE;
    s_priv->exported = false;
}

static void fence_vk_release(void **sp)
{
    struct ngpu_fence *s = *sp;
    if (!s)
        return;

    release_export_sem(s);
    ngpu_freep(sp);
}

struct ngpu_fence *ngpu_fence_vk_create(struct ngpu_ctx *ctx)
{
    struct ngpu_fence_vk *s = ngpu_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->parent.rc = NGPU_RC_CREATE(fence_vk_release);
    s->parent.gpu_ctx = ctx;

    return (struct ngpu_fence *)s;
}

int ngpu_fence_vk_reset(struct ngpu_fence *s)
{
    struct ngpu_fence_vk *s_priv = NGPU_PRIV_VK(s);
    struct ngpu_ctx_vk *ctx_vk = NGPU_PRIV_VK(s->gpu_ctx);

    s_priv->value = ++ctx_vk->timeline_value;
    release_export_sem(s);

    return 0;
}

int ngpu_fence_vk_wait(struct ngpu_fence *s)
{
    struct ngpu_fence_vk *s_priv = NGPU_PRIV_VK(s);
    struct ngpu_ctx_vk *ctx_vk = NGPU_PRIV_VK(s->gpu_ctx);
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
    struct ngpu_fence_vk *s_priv = NGPU_PRIV_VK(s);
    struct ngpu_ctx_vk *ctx_vk = NGPU_PRIV_VK(s->gpu_ctx);
    struct vkcontext *vk = ctx_vk->vkcontext;

    uint64_t value = 0;
    VkResult res = vk->funcs.GetSemaphoreCounterValue(vk->device, ctx_vk->timeline_sem, &value);
    if (res != VK_SUCCESS)
        return 0;
    return value >= s_priv->value;
}

VkResult ngpu_fence_vk_signal_export_sem(struct ngpu_fence *s, struct ngpu_cmd_buffer_vk *cmd_buffer)
{
    struct ngpu_fence_vk *s_priv = NGPU_PRIV_VK(s);

    ngpu_assert(s_priv->export_sem == VK_NULL_HANDLE);
    VkResult res = ngpu_ctx_vk_get_export_sem(s->gpu_ctx, cmd_buffer->signal_value, &s_priv->export_sem);
    if (res != VK_SUCCESS)
        return res;

    ngpu_cmd_buffer_vk_add_signal_sem(cmd_buffer, s_priv->export_sem);
    return VK_SUCCESS;
}

int ngpu_fence_vk_export_sync_file(struct ngpu_fence *s, int *fdp)
{
#if defined(TARGET_ANDROID)
    struct ngpu_fence_vk *s_priv = NGPU_PRIV_VK(s);
    struct ngpu_ctx_vk *ctx_vk = NGPU_PRIV_VK(s->gpu_ctx);
    struct vkcontext *vk = ctx_vk->vkcontext;

    if (!vk->funcs.GetSemaphoreFdKHR)
        return NGPU_ERROR_UNSUPPORTED;
    if (s_priv->export_sem == VK_NULL_HANDLE || s_priv->exported)
        return NGPU_ERROR_INVALID_USAGE;

    const VkSemaphoreGetFdInfoKHR fd_info = {
        .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore  = s_priv->export_sem,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkResult res = vk->funcs.GetSemaphoreFdKHR(vk->device, &fd_info, fdp);
    if (res != VK_SUCCESS) {
        LOG(ERROR, "could not export the fence: %s", ngpu_vk_res2str(res));
        return ngpu_vk_res2ret(res);
    }
    s_priv->exported = true;

    return 0;
#else
    return NGPU_ERROR_UNSUPPORTED;
#endif
}

void ngpu_fence_vk_freep(struct ngpu_fence **sp) { NGPU_RC_UNREFP(sp); }
