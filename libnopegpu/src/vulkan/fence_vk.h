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

#ifndef NGPU_FENCE_VK_H
#define NGPU_FENCE_VK_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "fence.h"

struct ngpu_ctx;

struct ngpu_cmd_buffer_vk;

struct ngpu_fence_vk {
    struct ngpu_fence parent;
    uint64_t value;
    VkSemaphore export_sem;
    bool exported;
};

struct ngpu_fence *ngpu_fence_vk_create(struct ngpu_ctx *ctx);
int ngpu_fence_vk_reset(struct ngpu_fence *fence);
int ngpu_fence_vk_wait(struct ngpu_fence *fence);
int ngpu_fence_vk_is_signaled(struct ngpu_fence *fence);
VkResult ngpu_fence_vk_signal_export_sem(struct ngpu_fence *fence, struct ngpu_cmd_buffer_vk *cmd_buffer);
int ngpu_fence_vk_export_sync_file(struct ngpu_fence *fence, int *fdp);
void ngpu_fence_vk_freep(struct ngpu_fence **sp);

#endif
