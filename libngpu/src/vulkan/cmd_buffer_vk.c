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

#include "vulkan/buffer_vk.h"
#include "vulkan/cmd_buffer_vk.h"
#include "vulkan/fence_vk.h"
#include "vulkan/ctx_vk.h"
#include "utils/darray.h"
#include "utils/memory.h"

static void cmd_buffer_vk_freep(void **sp)
{
    struct ngpu_cmd_buffer_vk *s = *sp;
    if (!s)
        return;

    struct ngpu_ctx_vk *gpu_ctx_vk = (struct ngpu_ctx_vk *)s->gpu_ctx;
    struct vkcontext *vk = gpu_ctx_vk->vkcontext;

    ngpu_darray_reset(&s->refs);
    ngpu_darray_reset(&s->buffer_refs);

    ngpu_darray_reset(&s->wait_sems);
    ngpu_darray_reset(&s->wait_stages);
    ngpu_darray_reset(&s->signal_sems);

    vk->funcs.FreeCommandBuffers(vk->device, s->pool, 1, &s->cmd_buf);

    ngpu_fence_freep(&s->fence);
    ngpu_freep(sp);
}

struct ngpu_cmd_buffer_vk *ngpu_cmd_buffer_vk_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngpu_cmd_buffer_vk *s = ngpu_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->rc = NGPU_RC_CREATE(cmd_buffer_vk_freep);
    s->gpu_ctx = gpu_ctx;
    return s;
}

static void unref_rc(void *user_arg, void *data)
{
    struct ngpu_rc **rcp = data;
    NGPU_RC_UNREFP(rcp);
}

static void unref_buffer(void *user_arg, void *data)
{
    struct ngpu_cmd_buffer_vk *cmd_buffer = user_arg;
    struct ngpu_buffer **bufferp = data;

    if (!*bufferp)
        return;

    ngpu_buffer_vk_unref_cmd_buffer(*bufferp, cmd_buffer);
    ngpu_buffer_freep(bufferp);
}

void ngpu_cmd_buffer_vk_freep(struct ngpu_cmd_buffer_vk **sp)
{
    NGPU_RC_UNREFP(sp);
}

VkResult ngpu_cmd_buffer_vk_init(struct ngpu_cmd_buffer_vk *s, int type)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;
    struct ngpu_ctx_vk *gpu_ctx_vk = (struct ngpu_ctx_vk *)gpu_ctx;
    struct vkcontext *vk = gpu_ctx_vk->vkcontext;

    s->type = type;
    s->pool = gpu_ctx_vk->cmd_pool;

    const VkCommandBufferAllocateInfo allocate_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = s->pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkResult res = vk->funcs.AllocateCommandBuffers(vk->device, &allocate_info, &s->cmd_buf);
    if (res != VK_SUCCESS)
        return res;

    s->fence = ngpu_fence_create(gpu_ctx);
    if (!s->fence)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    ngpu_darray_set_free_func(&s->refs, unref_rc, NULL);
    ngpu_darray_set_free_func(&s->buffer_refs, unref_buffer, s);

    return VK_SUCCESS;
}

VkResult ngpu_cmd_buffer_vk_add_wait_sem(struct ngpu_cmd_buffer_vk *s, VkSemaphore sem, VkPipelineStageFlags stage)
{
    if (ngpu_darray_push(&s->wait_sems, sem) < 0)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    if (ngpu_darray_push(&s->wait_stages, stage) < 0)
        return NGPU_ERROR_MEMORY;

    return VK_SUCCESS;
}

VkResult ngpu_cmd_buffer_vk_add_signal_sem(struct ngpu_cmd_buffer_vk *s, VkSemaphore sem)
{
    if (ngpu_darray_push(&s->signal_sems, sem) < 0)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    return VK_SUCCESS;
}

VkResult ngpu_cmd_buffer_vk_ref(struct ngpu_cmd_buffer_vk *s, struct ngpu_rc *rc)
{
    if (ngpu_darray_push(&s->refs, rc) < 0)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    NGPU_RC_REF(rc);

    return VK_SUCCESS;
}

VkResult ngpu_cmd_buffer_vk_ref_buffer(struct ngpu_cmd_buffer_vk *s, struct ngpu_buffer *buffer)
{
    VkResult res = ngpu_cmd_buffer_vk_ref(s, (struct ngpu_rc *)buffer);
    if (res != VK_SUCCESS)
        return res;

    if (ngpu_darray_push(&s->buffer_refs, buffer) < 0)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    NGPU_RC_REF(buffer);

    ngpu_buffer_vk_ref_cmd_buffer(buffer, s);

    return VK_SUCCESS;
}

VkResult ngpu_cmd_buffer_vk_begin(struct ngpu_cmd_buffer_vk *s)
{
    ngpu_darray_clear(&s->refs);
    ngpu_darray_clear(&s->buffer_refs);

    ngpu_darray_clear(&s->wait_sems);
    ngpu_darray_clear(&s->wait_stages);
    ngpu_darray_clear(&s->signal_sems);

    struct ngpu_ctx_vk *gpu_ctx_vk = (struct ngpu_ctx_vk *)s->gpu_ctx;
    struct vkcontext *vk = gpu_ctx_vk->vkcontext;

    vk->funcs.ResetCommandBuffer(s->cmd_buf, 0);

    const VkCommandBufferBeginInfo cmd_buf_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
    };
    return vk->funcs.BeginCommandBuffer(s->cmd_buf, &cmd_buf_begin_info);
}

VkResult ngpu_cmd_buffer_vk_submit(struct ngpu_cmd_buffer_vk *s)
{
    struct ngpu_ctx_vk *gpu_ctx_vk = (struct ngpu_ctx_vk *)s->gpu_ctx;
    struct vkcontext *vk = gpu_ctx_vk->vkcontext;

    VkResult res = vk->funcs.EndCommandBuffer(s->cmd_buf);
    if (res != VK_SUCCESS)
        return res;

    int ret = ngpu_fence_reset(s->fence);
    if (ret < 0)
        return VK_ERROR_UNKNOWN;

    const VkSubmitInfo submit_info = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = (uint32_t)s->wait_sems.count,
        .pWaitSemaphores      = s->wait_sems.data,
        .pWaitDstStageMask    = s->wait_stages.data,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &s->cmd_buf,
        .signalSemaphoreCount = (uint32_t)s->signal_sems.count,
        .pSignalSemaphores    = s->signal_sems.data,
    };

    struct ngpu_fence_vk *fence_vk = (struct ngpu_fence_vk *)s->fence;
    res = vk->funcs.QueueSubmit(vk->graphic_queue, 1, &submit_info, fence_vk->fence);
    if (res != VK_SUCCESS)
        return res;

    s->submitted = VK_TRUE;

    if (ngpu_darray_push(&gpu_ctx_vk->pending_cmd_buffers, s) < 0)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    ngpu_darray_clear(&s->wait_sems);
    ngpu_darray_clear(&s->wait_stages);
    ngpu_darray_clear(&s->signal_sems);

    return VK_SUCCESS;
}

VkResult ngpu_cmd_buffer_vk_wait(struct ngpu_cmd_buffer_vk *s)
{
    struct ngpu_ctx_vk *gpu_ctx_vk = (struct ngpu_ctx_vk *)s->gpu_ctx;
    struct vkcontext *vk = gpu_ctx_vk->vkcontext;

    if (s->submitted) {
        struct ngpu_fence_vk *fence_vk = (struct ngpu_fence_vk *)s->fence;
        VkResult res = vk->funcs.WaitForFences(vk->device, 1, &fence_vk->fence, VK_TRUE, UINT64_MAX);
        if (res != VK_SUCCESS)
            return res;
    }
    s->submitted = VK_FALSE;

    ngpu_darray_clear(&s->refs);
    ngpu_darray_clear(&s->buffer_refs);

    size_t i = 0;
    while (i < gpu_ctx_vk->pending_cmd_buffers.count) {
        if (gpu_ctx_vk->pending_cmd_buffers.data[i] == s) {
            ngpu_darray_remove(&gpu_ctx_vk->pending_cmd_buffers, i);
            continue;
        }
        i++;
    }

    return VK_SUCCESS;
}

VkResult ngpu_cmd_buffer_vk_begin_transient(struct ngpu_ctx *gpu_ctx, int type, struct ngpu_cmd_buffer_vk **sp)
{
    struct ngpu_cmd_buffer_vk *s = ngpu_cmd_buffer_vk_create(gpu_ctx);
    if (!s)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    VkResult res = ngpu_cmd_buffer_vk_init(s, type);
    if (res != VK_SUCCESS)
        goto fail;

    res = ngpu_cmd_buffer_vk_begin(s);
    if (res != VK_SUCCESS)
        goto fail;

    *sp = s;
    return VK_SUCCESS;

fail:
    ngpu_cmd_buffer_vk_freep(&s);
    return res;
}

VkResult ngpu_cmd_buffer_vk_execute_transient(struct ngpu_cmd_buffer_vk **sp)
{
    struct ngpu_cmd_buffer_vk *s = *sp;
    if (!s)
        return VK_SUCCESS;

    VkResult res = ngpu_cmd_buffer_vk_submit(s);
    if (res != VK_SUCCESS)
        goto done;

    res = ngpu_cmd_buffer_vk_wait(s);

done:
    ngpu_cmd_buffer_vk_freep(sp);
    return res;
}
