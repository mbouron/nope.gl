/*
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

#ifndef GPU_CTX_VK_H
#define GPU_CTX_VK_H

#include "gpu_ctx.h"
#include "vkcontext.h"
#include "cmd_buffer_vk.h"

struct gpu_ctx_vk {
    struct gpu_ctx parent;
    struct vkcontext *vkcontext;

    VkSemaphore *image_avail_sems;
    VkSemaphore *update_finished_sems;
    VkSemaphore *render_finished_sems;
    struct darray pending_wait_sems;

    VkCommandPool cmd_pool;

    struct cmd_buffer_vk **cmd_buffers;
    struct cmd_buffer_vk **update_cmd_buffers;
    struct darray pending_cmd_buffers;
    struct cmd_buffer_vk *cur_cmd_buffer;
    int cur_cmd_buffer_is_transient;

    VkQueryPool query_pool;

    VkSurfaceCapabilitiesKHR surface_caps;
    VkSurfaceFormatKHR surface_format;
    VkPresentModeKHR present_mode;
    VkSwapchainKHR swapchain;
    int recreate_swapchain;
    VkImage *images;
    uint32_t nb_images;
    uint32_t cur_image_index;
    int64_t present_time_offset;

    int32_t width;
    int32_t height;

    uint32_t nb_in_flight_frames;
    uint32_t cur_frame_index;

    struct darray colors;
    struct darray ms_colors;
    struct darray depth_stencils;
    struct darray rts;
    struct darray rts_load;
    struct gpu_buffer *capture_buffer;
    int capture_buffer_size;
    void *mapped_data;

    struct gpu_rendertarget *default_rt;
    struct gpu_rendertarget *default_rt_load;
    struct gpu_rendertarget_layout default_rt_layout;

    /*
     * The nope.gl pipeline API allows executing a pipeline with unbound
     * textures. The Vulkan API doesn't allow this, thus, to overcome this
     * restriction we allocate a dummy texture and bind it to any unbound
     * binding point of a pipeline.
     */
    struct gpu_texture *dummy_texture;
};

#endif
