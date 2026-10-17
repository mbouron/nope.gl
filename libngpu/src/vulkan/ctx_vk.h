/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2020-2022 GoPro Inc.
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

#ifndef NGPU_CTX_VK_H
#define NGPU_CTX_VK_H

#include <stdbool.h>
#include <stdint.h>

#include "ctx.h"
#include "utils/darray.h"
#include "vulkan/cmd_buffer_vk.h"
#include "vulkan/vkcontext.h"

/*
 * Binary semaphore used to export an NGPU fence as a sync_file fd on Android.
 *
 * A sync_file fd allows another API to wait for our GPU work before using its
 * result, without blocking the CPU. NGPU fences use the context's timeline
 * semaphore, which cannot be exported as a sync_file fd. Binary semaphores
 * can.
 *
 * To allow exporting these fences, submissions with an output fence also
 * signal an exportable binary semaphore when vkGetSemaphoreFdKHR() is
 * available. Both semaphores signal the completion of the same GPU work. The
 * binary semaphore is kept in the fence so ngpu_fence_export_sync_file() can
 * export it later, even while the GPU work is still pending. It must be added
 * before submission, whether or not the user eventually exports it.
 *
 * Exporting consumes the binary semaphore's signal, leaving it unsignaled. The
 * returned fd keeps that signal independently. A fence can only be exported
 * once per signal; waiting on the fence still uses the timeline semaphore.
 *
 * The context keeps these binary semaphores in a pool. When a semaphore is
 * associated with a fence, `in_use` is set and `value` records the
 * submission's timeline signal value. Resetting or freeing the fence clears
 * `in_use` and sets `exported` according to whether its signal was exported.
 * This returns the semaphore to the pool without waiting for the GPU.
 *
 * A released semaphore can only be reused or destroyed once the completed
 * timeline value is greater than `value`. The two semaphore signals in a
 * submission are unordered, so reaching `value` alone does not guarantee that
 * the binary signal has completed. A later submission's timeline signal does.
 *
 * Once this condition is met, an exported semaphore is unsignaled and can be
 * reused for another fence. A semaphore that was not exported remains signaled
 * and is destroyed instead. If none is available, a new one is created.
 *
 * Access to the pool is protected by `export_sems_lock`, as fences can be
 * released from another thread. The remaining semaphores are destroyed when
 * the context is destroyed, after waiting for the device to become idle.
 */
struct ngpu_export_sem_vk {
    VkSemaphore sem;
    uint64_t value;
    bool in_use;
    bool exported;
};

struct ngpu_ctx_vk {
    struct ngpu_ctx parent;
    struct vkcontext *vkcontext;
    bool own_vkcontext;

    VkSurfaceKHR surface;
    VkSurfaceFormatKHR *surface_formats;
    uint32_t nb_surface_formats;
    VkPresentModeKHR *present_modes;
    uint32_t nb_present_modes;

    VkSemaphore timeline_sem;
    uint64_t timeline_value;
    uint64_t pending_wait_value;

    VkCommandPool cmd_pool;

    struct ngpu_cmd_buffer_vk **cmd_buffers;
    struct ngpu_cmd_buffer_vk **update_cmd_buffers;
    NGPU_DARRAY(struct ngpu_cmd_buffer_vk *) pending_cmd_buffers;
    NGPU_DARRAY(struct ngpu_export_sem_vk) export_sems;
    pthread_mutex_t export_sems_lock;
    struct ngpu_cmd_buffer_vk *cur_cmd_buffer;
    int cur_cmd_buffer_is_transient;

    VkQueryPool query_pool;

    VkSurfaceCapabilitiesKHR surface_caps;
    VkSurfaceFormatKHR surface_format;
    VkPresentModeKHR present_mode;
    VkSwapchainKHR swapchain;
    int recreate_swapchain;
    VkImage *images;
    uint32_t nb_images;
    VkSemaphore *image_avail_sems;
    VkSemaphore *image_present_sems;
    uint32_t cur_image_index;
    int64_t present_time_offset;

    NGPU_DARRAY(struct ngpu_texture *) colors;
    NGPU_DARRAY(struct ngpu_texture *) ms_colors;
    NGPU_DARRAY(struct ngpu_texture *) depth_stencils;
    NGPU_DARRAY(struct ngpu_rendertarget *) rts;

    struct ngpu_rendertarget *default_rt;
    struct ngpu_rendertarget_layout default_rt_layout;

    /*
     * The nope.gl pipeline API allows executing a pipeline with unbound
     * textures. The Vulkan API doesn't allow this, thus, to overcome this
     * restriction we allocate a dummy texture and bind it to any unbound
     * binding point of a pipeline.
     */
    struct ngpu_texture *dummy_texture;
};

VkResult ngpu_ctx_vk_get_export_sem(struct ngpu_ctx *s, uint64_t value, VkSemaphore *semp);
void ngpu_ctx_vk_release_export_sem(struct ngpu_ctx *s, VkSemaphore sem, bool exported);

#endif
