/*
 * Copyright 2023-2024 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef CMD_BUFFER_VK_H
#define CMD_BUFFER_VK_H

#include <vulkan/vulkan.h>

#include "utils/darray.h"
#include "utils/refcount.h"
#include "utils/utils.h"

struct ngpu_buffer;

struct ngpu_cmd_buffer_vk {
    struct ngli_rc rc;
    struct ngpu_ctx *gpu_ctx;
    int type;
    VkCommandPool pool;
    VkCommandBuffer cmd_buf;
    VkFence fence;
    VkBool32 submitted;
    struct darray wait_sems;
    struct darray wait_stages;
    struct darray signal_sems;
    struct darray refs; // array of ngli_rc pointers
    struct darray buffer_refs; // array of ngpu_buffer pointers
};

struct ngpu_cmd_buffer_vk *ngpu_cmd_buffer_vk_create(struct ngpu_ctx *gpu_ctx);
void ngpu_cmd_buffer_vk_freep(struct ngpu_cmd_buffer_vk **sp);
VkResult ngpu_cmd_buffer_vk_init(struct ngpu_cmd_buffer_vk *s, int type);
VkResult ngpu_cmd_buffer_vk_add_wait_sem(struct ngpu_cmd_buffer_vk *s, VkSemaphore *sem, VkPipelineStageFlags stage);
VkResult ngpu_cmd_buffer_vk_add_signal_sem(struct ngpu_cmd_buffer_vk *s, VkSemaphore *sem);

#define NGPU_CMD_BUFFER_VK_REF(cmd, rc) ngpu_cmd_buffer_vk_ref((cmd), (struct ngli_rc *)(rc))
VkResult ngpu_cmd_buffer_vk_ref(struct ngpu_cmd_buffer_vk *s, struct ngli_rc *rc);
VkResult ngpu_cmd_buffer_vk_ref_buffer(struct ngpu_cmd_buffer_vk *s, struct ngpu_buffer *buffer);

VkResult ngpu_cmd_buffer_vk_begin(struct ngpu_cmd_buffer_vk *s);
VkResult ngpu_cmd_buffer_vk_submit(struct ngpu_cmd_buffer_vk *s);
VkResult ngpu_cmd_buffer_vk_wait(struct ngpu_cmd_buffer_vk *s);

VkResult ngpu_cmd_buffer_vk_begin_transient(struct ngpu_ctx *gpu_ctx, int type, struct ngpu_cmd_buffer_vk **sp);
VkResult ngpu_cmd_buffer_vk_execute_transient(struct ngpu_cmd_buffer_vk **sp);

#endif
