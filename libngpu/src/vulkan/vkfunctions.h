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

#ifndef VKFUNCTIONS_H
#define VKFUNCTIONS_H

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <vulkan/vulkan.h>
#if defined(TARGET_DARWIN) || defined(TARGET_IPHONE)
#include <vulkan/vulkan_metal.h>
#endif

#define NGPU_VK_FN_LIST(MACRO)                                               \
    /* Pre-instance globals */                                               \
    MACRO(false, false, false, GetInstanceProcAddr)                          \
    MACRO(false, false, true, EnumerateInstanceVersion)                      \
    MACRO(false, false, false, EnumerateInstanceLayerProperties)             \
    MACRO(false, false, false, EnumerateInstanceExtensionProperties)         \
    MACRO(false, false, false, CreateInstance)                               \
                                                                             \
    /* Instance-level */                                                     \
    MACRO(true, false, false, DestroyInstance)                               \
    MACRO(true, false, false, EnumeratePhysicalDevices)                      \
    MACRO(true, false, false, GetPhysicalDeviceProperties)                   \
    MACRO(true, false, false, GetPhysicalDeviceFeatures2)                    \
    MACRO(true, false, false, GetPhysicalDeviceMemoryProperties)             \
    MACRO(true, false, false, GetPhysicalDeviceQueueFamilyProperties)        \
    MACRO(true, false, false, GetPhysicalDeviceFormatProperties)             \
    MACRO(true, false, true, GetPhysicalDeviceImageFormatProperties2)        \
    MACRO(true, false, true, GetPhysicalDeviceSurfaceSupportKHR)             \
    MACRO(true, false, true, GetPhysicalDeviceSurfaceCapabilitiesKHR)        \
    MACRO(true, false, true, GetPhysicalDeviceSurfaceFormatsKHR)             \
    MACRO(true, false, true, GetPhysicalDeviceSurfacePresentModesKHR)        \
    MACRO(true, false, false, EnumerateDeviceExtensionProperties)            \
    MACRO(true, false, false, CreateDevice)                                  \
    MACRO(true, false, false, GetDeviceProcAddr)                             \
    MACRO(true, false, true, DestroySurfaceKHR)                              \
    MACRO(true, false, true, CreateDebugUtilsMessengerEXT)                   \
    MACRO(true, false, true, DestroyDebugUtilsMessengerEXT)                  \
                                                                             \
    /* Device-level */                                                       \
    MACRO(true, true, false, DestroyDevice)                                  \
    MACRO(true, true, false, DeviceWaitIdle)                                 \
    MACRO(true, true, false, GetDeviceQueue)                                 \
    MACRO(true, true, false, QueueSubmit)                                    \
    MACRO(true, true, true, QueuePresentKHR)                                 \
    MACRO(true, true, false, CreateCommandPool)                              \
    MACRO(true, true, false, DestroyCommandPool)                             \
    MACRO(true, true, false, AllocateCommandBuffers)                         \
    MACRO(true, true, false, FreeCommandBuffers)                             \
    MACRO(true, true, false, BeginCommandBuffer)                             \
    MACRO(true, true, false, EndCommandBuffer)                               \
    MACRO(true, true, false, ResetCommandBuffer)                             \
    MACRO(true, true, false, CreateFence)                                    \
    MACRO(true, true, false, DestroyFence)                                   \
    MACRO(true, true, false, WaitForFences)                                  \
    MACRO(true, true, false, ResetFences)                                    \
    MACRO(true, true, false, GetFenceStatus)                                 \
    MACRO(true, true, false, CreateSemaphore)                                \
    MACRO(true, true, false, DestroySemaphore)                               \
    MACRO(true, true, false, CreateBuffer)                                   \
    MACRO(true, true, false, DestroyBuffer)                                  \
    MACRO(true, true, false, GetBufferMemoryRequirements)                    \
    MACRO(true, true, false, AllocateMemory)                                 \
    MACRO(true, true, false, FreeMemory)                                     \
    MACRO(true, true, false, BindBufferMemory)                               \
    MACRO(true, true, false, MapMemory)                                      \
    MACRO(true, true, false, UnmapMemory)                                    \
    MACRO(true, true, false, CreateImage)                                    \
    MACRO(true, true, false, DestroyImage)                                   \
    MACRO(true, true, false, GetImageMemoryRequirements)                     \
    MACRO(true, true, true, GetImageMemoryRequirements2)                     \
    MACRO(true, true, false, BindImageMemory)                                \
    MACRO(true, true, false, CreateImageView)                                \
    MACRO(true, true, false, DestroyImageView)                               \
    MACRO(true, true, false, CreateSampler)                                  \
    MACRO(true, true, false, DestroySampler)                                 \
    MACRO(true, true, false, CreateRenderPass)                               \
    MACRO(true, true, false, DestroyRenderPass)                              \
    MACRO(true, true, false, CreateFramebuffer)                              \
    MACRO(true, true, false, DestroyFramebuffer)                             \
    MACRO(true, true, false, CreateShaderModule)                             \
    MACRO(true, true, false, DestroyShaderModule)                            \
    MACRO(true, true, false, CreatePipelineLayout)                           \
    MACRO(true, true, false, DestroyPipelineLayout)                          \
    MACRO(true, true, false, CreateGraphicsPipelines)                        \
    MACRO(true, true, false, CreateComputePipelines)                         \
    MACRO(true, true, false, DestroyPipeline)                                \
    MACRO(true, true, false, CreateDescriptorSetLayout)                      \
    MACRO(true, true, false, DestroyDescriptorSetLayout)                     \
    MACRO(true, true, false, CreateDescriptorPool)                           \
    MACRO(true, true, false, DestroyDescriptorPool)                          \
    MACRO(true, true, false, ResetDescriptorPool)                            \
    MACRO(true, true, false, AllocateDescriptorSets)                         \
    MACRO(true, true, false, UpdateDescriptorSets)                           \
    MACRO(true, true, false, CreateQueryPool)                                \
    MACRO(true, true, false, DestroyQueryPool)                               \
    MACRO(true, true, false, GetQueryPoolResults)                            \
    MACRO(true, true, true, CreateSwapchainKHR)                              \
    MACRO(true, true, true, DestroySwapchainKHR)                             \
    MACRO(true, true, true, GetSwapchainImagesKHR)                           \
    MACRO(true, true, true, AcquireNextImageKHR)                             \
                                                                             \
    /* Device-level command buffer recording */                              \
    MACRO(true, true, false, CmdBeginRenderPass)                             \
    MACRO(true, true, false, CmdEndRenderPass)                               \
    MACRO(true, true, false, CmdBindPipeline)                                \
    MACRO(true, true, false, CmdBindDescriptorSets)                          \
    MACRO(true, true, false, CmdBindVertexBuffers)                           \
    MACRO(true, true, false, CmdBindIndexBuffer)                             \
    MACRO(true, true, false, CmdDraw)                                        \
    MACRO(true, true, false, CmdDrawIndexed)                                 \
    MACRO(true, true, false, CmdDispatch)                                    \
    MACRO(true, true, false, CmdCopyBuffer)                                  \
    MACRO(true, true, false, CmdCopyBufferToImage)                           \
    MACRO(true, true, false, CmdCopyImageToBuffer)                           \
    MACRO(true, true, false, CmdBlitImage)                                   \
    MACRO(true, true, false, CmdPipelineBarrier)                             \
    MACRO(true, true, false, CmdSetViewport)                                 \
    MACRO(true, true, false, CmdSetScissor)                                  \
    MACRO(true, true, false, CmdSetLineWidth)                                \
    MACRO(true, true, false, CmdResetQueryPool)                              \
    MACRO(true, true, false, CmdWriteTimestamp)                              \
                                                                             \
    /* Device-level extension functions */                                   \
    MACRO(true, true, true, CreateSamplerYcbcrConversionKHR)                 \
    MACRO(true, true, true, DestroySamplerYcbcrConversionKHR)                \
    MACRO(true, true, true, GetMemoryFdKHR)                                  \
    MACRO(true, true, true, GetMemoryFdPropertiesKHR)                        \
    MACRO(true, true, true, GetRefreshCycleDurationGOOGLE)                   \
    MACRO(true, true, true, GetPastPresentationTimingGOOGLE)

#if defined(TARGET_ANDROID)
#define NGPU_VK_FN_LIST_ANDROID(MACRO)                                       \
    MACRO(true, true, true, GetAndroidHardwareBufferPropertiesANDROID)       \
    MACRO(true, true, true, GetMemoryAndroidHardwareBufferANDROID)
#else
#define NGPU_VK_FN_LIST_ANDROID(MACRO)
#endif

#if defined(TARGET_DARWIN) || defined(TARGET_IPHONE)
#define NGPU_VK_FN_LIST_MACOS(MACRO)                                         \
    MACRO(true, true, true, ExportMetalObjectsEXT)
#else
#define NGPU_VK_FN_LIST_MACOS(MACRO)
#endif

#define NGPU_VK_FN_DEF(req_inst, req_dev, optional, name) PFN_vk##name name;

struct vk_functions {
    NGPU_VK_FN_LIST(NGPU_VK_FN_DEF)
    NGPU_VK_FN_LIST_ANDROID(NGPU_VK_FN_DEF)
    NGPU_VK_FN_LIST_MACOS(NGPU_VK_FN_DEF)
};

#undef NGPU_VK_FN_DEF

struct vk_function_load_info {
    const char *name;
    size_t offset;
    bool instance;
    bool device;
    bool optional;
};

#endif /* VKFUNCTIONS_H */
