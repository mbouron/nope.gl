/*
 * Copyright 2024-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include "config.h"
#include <vulkan/vulkan_core.h>

#if defined(TARGET_LINUX)
# define VK_USE_PLATFORM_XLIB_KHR
# define VK_USE_PLATFORM_WAYLAND_KHR
#elif defined(TARGET_ANDROID)
# define VK_USE_PLATFORM_ANDROID_KHR
#elif defined(TARGET_WINDOWS)
# define VK_USE_PLATFORM_WIN32_KHR
#elif defined(TARGET_DARWIN) || defined(TARGET_IPHONE)
# include <MoltenVK/mvk_vulkan.h>
# include "wsi_apple.h"
#endif

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if !NGPU_VULKAN_STATIC
# if defined(TARGET_WINDOWS)
#  include <windows.h>
# else
#  include <dlfcn.h>
# endif
#endif

#include "utils/log.h"
#include "ctx.h"
#include "ngpu/ngpu.h"
#include "vulkan/vkcontext.h"
#include "vulkan/vkutils.h"
#include "utils/bstr.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT type,
                                                     const VkDebugUtilsMessengerCallbackDataEXT *cb_data,
                                                     void *user_data)
{
    /*
     * Silence VUID-VkSwapchainCreateInfoKHR-imageExtent-01274 as it is considered
     * a false positive.
     * See: https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/1340
     */
    if (cb_data->messageIdNumber == 0x7cd0911d)
        return VK_FALSE;

    /*
     * Silence VUID-VkShaderModuleCreateInfo-pCode-08737 as it can be
     * considered a false positive. It happens when loading shader modules
     * produced by 16.2.0 in the CI (Ubuntu 24.04).
     */
    if (cb_data->messageIdNumber == 0xa5625282)
        return VK_FALSE;

    enum ngpu_log_level level = NGPU_LOG_INFO;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)        level = NGPU_LOG_ERROR;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) level = NGPU_LOG_WARNING;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)    level = NGPU_LOG_INFO;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) level = NGPU_LOG_VERBOSE;

    const char *msg_type = "GENERAL";
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)       msg_type = "VALIDATION";
    else if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) msg_type = "PERFORMANCE";

    size_t msg_len = strlen(cb_data->pMessage);
    while (msg_len > 0 && strchr(" \r\n", cb_data->pMessage[msg_len - 1]))
        msg_len--;
    if (msg_len > INT_MAX)
        return VK_TRUE;
    ngpu_log_print(level, __FILE__, __LINE__, "debug_callback", "%s: %.*s", msg_type, (int)msg_len, cb_data->pMessage);

    /* Make the Vulkan call fail if the validation layer has returned an error */
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) &&
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT))
        return VK_TRUE;

    return VK_FALSE;
}

static int has_layer(struct vkcontext *s, const char *name)
{
    for (uint32_t i = 0; i < s->nb_layers; i++)
        if (!strcmp(s->layers[i].layerName, name))
            return 1;
    return 0;
}

static const char *platform_ext_names[] = {
    [NGPU_PLATFORM_XLIB]    = "VK_KHR_xlib_surface",
    [NGPU_PLATFORM_ANDROID] = "VK_KHR_android_surface",
    [NGPU_PLATFORM_MACOS]   = "VK_MVK_macos_surface",
    [NGPU_PLATFORM_IOS]     = "VK_MVK_ios_surface",
    [NGPU_PLATFORM_WINDOWS] = "VK_KHR_win32_surface",
    [NGPU_PLATFORM_WAYLAND] = "VK_KHR_wayland_surface",
};

#define NGPU_VK_FN_LOAD_INFO(req_inst, req_dev, optional, name) \
    {"vk" #name, offsetof(struct vk_functions, name), req_inst, req_dev, optional},

static const struct vk_function_load_info vk_load_infos[] = {
    NGPU_VK_FN_LIST(NGPU_VK_FN_LOAD_INFO)
    NGPU_VK_FN_LIST_ANDROID(NGPU_VK_FN_LOAD_INFO)
    NGPU_VK_FN_LIST_MACOS(NGPU_VK_FN_LOAD_INFO)
};
#undef NGPU_VK_FN_LOAD_INFO

static VkResult load_functions(struct vkcontext *s, bool instance, bool device)
{
    struct vk_functions *funcs = &s->funcs;

    for (size_t i = 0; i < NGPU_ARRAY_NB(vk_load_infos); i++) {
        const struct vk_function_load_info *info = &vk_load_infos[i];
        if (info->instance != instance || info->device != device)
            continue;

        PFN_vkVoidFunction func = NULL;
        if (info->device)
            func = funcs->GetDeviceProcAddr(s->device, info->name);
        else if (info->instance)
            func = funcs->GetInstanceProcAddr(s->instance, info->name);
        else
            func = funcs->GetInstanceProcAddr(NULL, info->name);

        if (!func && !info->optional) {
            LOG(ERROR, "could not load required Vulkan function: %s", info->name);
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        *(PFN_vkVoidFunction *)((uintptr_t)funcs + info->offset) = func;
    }
    return VK_SUCCESS;
}

#if NGPU_VULKAN_STATIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *pName);

static VkResult load_libvulkan(struct vkcontext *s)
{
    s->funcs.GetInstanceProcAddr = vkGetInstanceProcAddr;
    return VK_SUCCESS;
}
#else
static VkResult load_libvulkan(struct vkcontext *s)
{
    static const char *lib_names[] = {
#if defined(TARGET_WINDOWS)
        "vulkan-1.dll",
#elif defined(TARGET_DARWIN)
        "libMoltenVK.dylib",
#elif defined(TARGET_IPHONE)
        "libMoltenVK.dylib",
#else
        "libvulkan.so.1",
        "libvulkan.so",
#endif
    };

    for (size_t i = 0; i < NGPU_ARRAY_NB(lib_names); i++) {
#if defined(TARGET_WINDOWS)
        s->libvulkan = (void *)LoadLibraryA(lib_names[i]);
#else
        s->libvulkan = dlopen(lib_names[i], RTLD_NOW | RTLD_LOCAL);
#endif
        if (s->libvulkan)
            break;
    }

    if (!s->libvulkan) {
        LOG(ERROR, "could not load the Vulkan library");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

#if defined(TARGET_WINDOWS)
    s->funcs.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)
        GetProcAddress((HMODULE)s->libvulkan, "vkGetInstanceProcAddr");
#else
    s->funcs.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
        dlsym(s->libvulkan, "vkGetInstanceProcAddr");
#endif

    if (!s->funcs.GetInstanceProcAddr) {
        LOG(ERROR, "could not resolve vkGetInstanceProcAddr");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return VK_SUCCESS;
}
#endif

static VkResult create_instance(struct vkcontext *s, enum ngpu_platform_type platform, int debug)
{
    s->api_version = VK_API_VERSION_1_0;

    if (s->funcs.EnumerateInstanceVersion) {
        VkResult res = s->funcs.EnumerateInstanceVersion(&s->api_version);
        if (res != VK_SUCCESS) {
            LOG(ERROR, "could not enumerate Vulkan instance version");
        }
    }

    LOG(DEBUG, "available instance version: %d.%d.%d",
        (int)VK_VERSION_MAJOR(s->api_version),
        (int)VK_VERSION_MINOR(s->api_version),
        (int)VK_VERSION_PATCH(s->api_version));

    if (s->api_version < VK_API_VERSION_1_2) {
        LOG(ERROR, "instance API version (%d.%d.%d) is lower than the minimum supported version (%d.%d.%d)",
                    (int)VK_VERSION_MAJOR(s->api_version),
                    (int)VK_VERSION_MINOR(s->api_version),
                    (int)VK_VERSION_PATCH(s->api_version),
                    (int)VK_VERSION_MAJOR(VK_API_VERSION_1_2),
                    (int)VK_VERSION_MINOR(VK_API_VERSION_1_2),
                    (int)VK_VERSION_PATCH(VK_API_VERSION_1_2));
        return NGPU_ERROR_GRAPHICS_UNSUPPORTED;
    }

    VkResult res = s->funcs.EnumerateInstanceLayerProperties(&s->nb_layers, NULL);
    if (res != VK_SUCCESS)
        return res;

    s->layers = ngpu_calloc(s->nb_layers, sizeof(*s->layers));
    if (!s->layers)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    res = s->funcs.EnumerateInstanceLayerProperties(&s->nb_layers, s->layers);
    if (res != VK_SUCCESS)
        return res;

    LOG(DEBUG, "available layers:");
    for (uint32_t i = 0; i < s->nb_layers; i++)
        LOG(DEBUG, "  %u/%u: %s", i+1, s->nb_layers, s->layers[i].layerName);

    res = s->funcs.EnumerateInstanceExtensionProperties(NULL, &s->nb_extensions, NULL);
    if (res != VK_SUCCESS)
        return res;

    s->extensions = ngpu_calloc(s->nb_extensions, sizeof(*s->extensions));
    if (!s->extensions)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    res = s->funcs.EnumerateInstanceExtensionProperties(NULL, &s->nb_extensions, s->extensions);
    if (res != VK_SUCCESS)
        return res;

    LOG(DEBUG, "available instance extensions:");
    for (uint32_t i = 0; i < s->nb_extensions; i++) {
        LOG(DEBUG, "  %u/%u: %s v%u", i+1, s->nb_extensions,
            s->extensions[i].extensionName, s->extensions[i].specVersion);
    }

    if (platform < 0 || platform >= NGPU_ARRAY_NB(platform_ext_names)) {
        LOG(ERROR, "unsupported platform: %u", platform);
        return VK_ERROR_UNKNOWN;
    }

    const char *surface_extension_name = platform_ext_names[platform];
    if (!surface_extension_name) {
        LOG(ERROR, "unsupported platform: %u", platform);
        return VK_ERROR_UNKNOWN;
    }

    const char *mandatory_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        surface_extension_name,
#if defined(VK_USE_PLATFORM_MACOS_MVK) || defined(VK_USE_PLATFORM_IOS_MVK)
        "VK_EXT_metal_surface",
#endif
    };

    NGPU_DARRAY(const char *) extensions = {0};
    NGPU_DARRAY(const char *) layers = {0};

    for (size_t i = 0; i < NGPU_ARRAY_NB(mandatory_extensions); i++) {
        if (ngpu_darray_push(&extensions, mandatory_extensions[i]) < 0) {
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto end;
        }
    }

    static const char *debug_ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const int has_debug_extension = ngpu_vkcontext_has_extension(s, debug_ext, 0);
    if (debug) {
        if (has_debug_extension && ngpu_darray_push(&extensions, debug_ext) < 0) {
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto end;
        }

        static const char *debug_layer = "VK_LAYER_KHRONOS_validation";
        const int has_validation_layer = has_layer(s, debug_layer);
        if (has_validation_layer && ngpu_darray_push(&layers, debug_layer) < 0) {
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto end;
        }

        if (!has_validation_layer)
            LOG(WARNING, "missing validation layer: %s", debug_layer);
    }

    const VkApplicationInfo app_info = {
        .sType         = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pEngineName   = "nope.gl",
        .engineVersion = 0,
        .apiVersion    = s->api_version,
    };

    const VkInstanceCreateInfo instance_create_info = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &app_info,
        .enabledExtensionCount   = (uint32_t)extensions.count,
        .ppEnabledExtensionNames = extensions.data,
        .enabledLayerCount       = (uint32_t)layers.count,
        .ppEnabledLayerNames     = layers.data,
    };

    res = s->funcs.CreateInstance(&instance_create_info, NULL, &s->instance);
    if (res != VK_SUCCESS)
        goto end;

    res = load_functions(s, true, false);
    if (res != VK_SUCCESS)
        goto end;

    if (debug && has_debug_extension) {
        if (!s->funcs.CreateDebugUtilsMessengerEXT) {
            res = VK_ERROR_EXTENSION_NOT_PRESENT;
            goto end;
        }

        const VkDebugUtilsMessengerCreateInfoEXT info = {
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_callback,
            .pUserData       = NULL,
        };

        res = s->funcs.CreateDebugUtilsMessengerEXT(s->instance, &info, NULL, &s->debug_callback);
        if (res != VK_SUCCESS)
            goto end;
    }

end:
    ngpu_darray_reset(&extensions);
    ngpu_darray_reset(&layers);

    return res;
}

static VkResult create_window_surface(struct vkcontext *s, const struct ngpu_ctx_params *params)
{
    if (params->offscreen)
        return VK_SUCCESS;

    if (!params->window)
        return VK_ERROR_UNKNOWN;

    const enum ngpu_platform_type platform = params->platform;
    if (platform == NGPU_PLATFORM_XLIB) {
#if defined(TARGET_LINUX)
        s->x11_display = (Display *)params->display;
        if (!s->x11_display) {
            s->x11_display = XOpenDisplay(NULL);
            if (!s->x11_display) {
                LOG(ERROR, "could not open X11 display");
                return VK_ERROR_UNKNOWN;
            }
            s->own_x11_display = 1;
        }

        const VkXlibSurfaceCreateInfoKHR surface_create_info = {
            .sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
            .dpy    = s->x11_display,
            .window = params->window,
        };

        PFN_vkCreateXlibSurfaceKHR CreateXlibSurfaceKHR =
            (PFN_vkCreateXlibSurfaceKHR)(void *)s->funcs.GetInstanceProcAddr(s->instance, "vkCreateXlibSurfaceKHR");
        if (!CreateXlibSurfaceKHR) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        VkResult res = CreateXlibSurfaceKHR(s->instance, &surface_create_info, NULL, &s->surface);
        if (res != VK_SUCCESS)
            return res;
#else
        return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
    } else if (platform == NGPU_PLATFORM_ANDROID) {
#if defined(TARGET_ANDROID)
        const VkAndroidSurfaceCreateInfoKHR surface_create_info = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
            .window = (struct ANativeWindow *)params->window,
        };

        PFN_vkCreateAndroidSurfaceKHR CreateAndroidSurfaceKHR =
            (PFN_vkCreateAndroidSurfaceKHR)(void *)s->funcs.GetInstanceProcAddr(s->instance, "vkCreateAndroidSurfaceKHR");
        if (!CreateAndroidSurfaceKHR) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        VkResult res = CreateAndroidSurfaceKHR(s->instance, &surface_create_info, NULL, &s->surface);
        if (res != VK_SUCCESS)
            return res;
#else
        return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
    } else if (platform == NGPU_PLATFORM_MACOS || platform == NGPU_PLATFORM_IOS) {
#if defined(TARGET_DARWIN) || defined(TARGET_IPHONE)
        const void *view = (const void *)params->window;
        const CAMetalLayer *layer = ngpu_window_get_metal_layer(view);
        if (!layer)
            return VK_ERROR_UNKNOWN;

        VkMetalSurfaceCreateInfoEXT surface_create_info = {
            .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
            .pLayer = layer,
        };

        PFN_vkCreateMetalSurfaceEXT CreateMetalSurfaceEXT =
            (PFN_vkCreateMetalSurfaceEXT)(void *)s->funcs.GetInstanceProcAddr(s->instance, "vkCreateMetalSurfaceEXT");
        if (!CreateMetalSurfaceEXT) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        VkResult res = CreateMetalSurfaceEXT(s->instance, &surface_create_info, NULL, &s->surface);
        if (res != VK_SUCCESS)
            return res;
#endif
    } else if (platform == NGPU_PLATFORM_WINDOWS) {
#if defined(TARGET_WINDOWS)
        const VkWin32SurfaceCreateInfoKHR surface_create_info = {
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hwnd = (HWND)params->window,
        };

        PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR =
            (PFN_vkCreateWin32SurfaceKHR)(void *)s->funcs.GetInstanceProcAddr(s->instance, "vkCreateWin32SurfaceKHR");
        if (!CreateWin32SurfaceKHR) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        VkResult res = CreateWin32SurfaceKHR(s->instance, &surface_create_info, NULL, &s->surface);
        if (res != VK_SUCCESS)
            return res;
#endif
    } else if (platform == NGPU_PLATFORM_WAYLAND) {
#if defined(HAVE_WAYLAND)
        const VkWaylandSurfaceCreateInfoKHR surface_create_info = {
            .sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
            .display = (struct wl_display *)params->display,
            .surface = (struct wl_surface *)params->window,
        };

        PFN_vkCreateWaylandSurfaceKHR CreateWaylandSurfaceKHR =
            (PFN_vkCreateWaylandSurfaceKHR)(void *)s->funcs.GetInstanceProcAddr(s->instance, "vkCreateWaylandSurfaceKHR");
        if (!CreateWaylandSurfaceKHR) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        VkResult res = CreateWaylandSurfaceKHR(s->instance, &surface_create_info, NULL, &s->surface);
        if (res != VK_SUCCESS)
            return res;
#else
        return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
    } else {
        ngpu_assert(0);
    }

    return VK_SUCCESS;
}

static VkResult enumerate_physical_devices(struct vkcontext *s, const struct ngpu_ctx_params *ctx_params)
{
    VkResult res = s->funcs.EnumeratePhysicalDevices(s->instance, &s->nb_phy_devices, NULL);
    if (res != VK_SUCCESS)
        return res;

    if (!s->nb_phy_devices)
        return VK_ERROR_DEVICE_LOST;

    s->phy_devices = ngpu_calloc(s->nb_phy_devices, sizeof(*s->phy_devices));
    if (!s->phy_devices)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    return s->funcs.EnumeratePhysicalDevices(s->instance, &s->nb_phy_devices, s->phy_devices);
}

static void get_memory_property_flags_str(struct bstr *bstr, VkMemoryPropertyFlags flags)
{
    static const struct {
        VkMemoryPropertyFlagBits property;
        const char *name;
    } vk_mem_prop_map[] = {
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,        "device_local"},
        {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,        "host_visible"},
        {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,       "host_coherent"},
        {VK_MEMORY_PROPERTY_HOST_CACHED_BIT,         "host_cached"},
        {VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT,    "lazy_allocated"},
        {VK_MEMORY_PROPERTY_PROTECTED_BIT,           "protected"},
        {VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD, "device_coherent_amd"},
        {VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD, "device_uncached_amd"},
        {VK_MEMORY_PROPERTY_RDMA_CAPABLE_BIT_NV,     "rdma_capable_nv"},
    };

    int nb_props = 0;
    for (size_t i = 0; i < NGPU_ARRAY_NB(vk_mem_prop_map); i++) {
        if (vk_mem_prop_map[i].property & flags) {
            ngpu_bstr_printf(bstr, "%s%s", nb_props == 0 ? "" : "|", vk_mem_prop_map[i].name);
            nb_props++;
        }
    }
}

static VkResult select_physical_device(struct vkcontext *s, const struct ngpu_ctx_params *ctx_params)
{
    static const struct {
        const char *name;
        uint32_t priority;
    } types[] = {
        [VK_PHYSICAL_DEVICE_TYPE_OTHER]          = {"other",      1},
        [VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU] = {"integrated", 4},
        [VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU]   = {"discrete",   5},
        [VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU]    = {"virtual",    3},
        [VK_PHYSICAL_DEVICE_TYPE_CPU]            = {"cpu",        2},
    };

    uint32_t priority = 0;
    for (uint32_t i = 0; i < s->nb_phy_devices; i++) {
        VkPhysicalDevice phy_device = s->phy_devices[i];

        VkPhysicalDeviceProperties dev_props;
        s->funcs.GetPhysicalDeviceProperties(phy_device, &dev_props);

        VkPhysicalDeviceVulkan12Features dev_features_vk12 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext = NULL,
        };

        VkPhysicalDeviceVulkan11Features dev_features_vk11 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
            .pNext = &dev_features_vk12,
        };

        VkPhysicalDeviceFeatures2 dev_features2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &dev_features_vk11,
        };

        s->funcs.GetPhysicalDeviceFeatures2(phy_device, &dev_features2);

        VkPhysicalDeviceMemoryProperties mem_props;
        s->funcs.GetPhysicalDeviceMemoryProperties(phy_device, &mem_props);

        if (dev_props.deviceType >= NGPU_ARRAY_NB(types)) {
            LOG(ERROR, "device %s has unknown type: 0x%x, skipping", dev_props.deviceName, dev_props.deviceType);
            continue;
        }

        if (dev_props.apiVersion < VK_API_VERSION_1_2) {
            LOG(DEBUG, "device %s API version (%d.%d.%d) is lower than the minimum supported version (%d.%d.%d), skipping",
                dev_props.deviceName,
                (int)VK_VERSION_MAJOR(dev_props.apiVersion),
                (int)VK_VERSION_MINOR(dev_props.apiVersion),
                (int)VK_VERSION_PATCH(dev_props.apiVersion),
                (int)VK_VERSION_MAJOR(VK_API_VERSION_1_2),
                (int)VK_VERSION_MINOR(VK_API_VERSION_1_2),
                (int)VK_VERSION_PATCH(VK_API_VERSION_1_2));
            continue;
        }

        uint32_t qfamily_count = 0;
        s->funcs.GetPhysicalDeviceQueueFamilyProperties(phy_device, &qfamily_count, NULL);
        VkQueueFamilyProperties *qfamily_props = ngpu_calloc(qfamily_count, sizeof(*qfamily_props));
        if (!qfamily_props)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        s->funcs.GetPhysicalDeviceQueueFamilyProperties(phy_device, &qfamily_count, qfamily_props);

        bool found_queues = false;
        uint32_t queue_family_graphics_id = UINT32_MAX;
        uint32_t queue_family_present_id = UINT32_MAX;
        for (uint32_t j = 0; j < qfamily_count; j++) {
            const VkQueueFamilyProperties props = qfamily_props[j];
            const VkQueueFlags flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if (NGPU_HAS_ALL_FLAGS(props.queueFlags, flags))
                queue_family_graphics_id = j;
            if (s->surface) {
                VkBool32 support;
                s->funcs.GetPhysicalDeviceSurfaceSupportKHR(phy_device, j, s->surface, &support);
                if (support)
                    queue_family_present_id = j;
            }
            found_queues = queue_family_graphics_id != UINT32_MAX && (!s->surface || queue_family_present_id != UINT32_MAX);
            if (found_queues)
                break;
        }
        ngpu_free(qfamily_props);

        if (!found_queues)
            continue;

        if (types[dev_props.deviceType].priority > priority) {
            priority = types[dev_props.deviceType].priority;
            s->phy_device = phy_device;
            s->phy_device_props = dev_props;
            s->graphics_queue_index = queue_family_graphics_id;
            s->present_queue_index = queue_family_present_id;
            s->dev_features = dev_features2.features;
            s->dev_features_vk11 = dev_features_vk11;
            s->dev_features_vk11.pNext = NULL;
            s->dev_features_vk12 = dev_features_vk12;
            s->dev_features_vk12.pNext = NULL;
            s->phydev_mem_props = mem_props;
        }
    }

    if (!s->phy_device) {
        LOG(ERROR, "no valid physical device found");
        return VK_ERROR_DEVICE_LOST;
    }

    LOG(DEBUG, "select physical device: %s, graphics queue: %u, present queue: %u",
        s->phy_device_props.deviceName, s->graphics_queue_index, s->present_queue_index);

    struct bstr *type = ngpu_bstr_create();
    struct bstr *props = ngpu_bstr_create();
    if (!type || !props) {
        ngpu_bstr_freep(&type);
        ngpu_bstr_freep(&props);
        return NGPU_ERROR_MEMORY;
    }
    LOG(DEBUG, "available memory types:");
    for (uint32_t i = 0; i < s->phydev_mem_props.memoryTypeCount; i++) {
        get_memory_property_flags_str(type, 1 << i);
        get_memory_property_flags_str(props, s->phydev_mem_props.memoryTypes[i].propertyFlags);
        LOG(DEBUG, "\t%s:\t%s", ngpu_bstr_strptr(type), ngpu_bstr_strptr(props));
        ngpu_bstr_clear(type);
        ngpu_bstr_clear(props);
    }
    ngpu_bstr_freep(&type);
    ngpu_bstr_freep(&props);

    return VK_SUCCESS;
}

static VkResult enumerate_extensions(struct vkcontext *s)
{
    VkResult res = s->funcs.EnumerateDeviceExtensionProperties(s->phy_device, NULL, &s->nb_device_extensions, NULL);
    if (res != VK_SUCCESS)
        return res;

    s->device_extensions = ngpu_calloc(s->nb_device_extensions, sizeof(*s->device_extensions));
    if (!s->device_extensions)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    res = s->funcs.EnumerateDeviceExtensionProperties(s->phy_device, NULL, &s->nb_device_extensions, s->device_extensions);
    if (res != VK_SUCCESS)
        return res;

    LOG(DEBUG, "available device extensions:");
    for (uint32_t i = 0; i < s->nb_device_extensions; i++) {
        LOG(DEBUG, "  %u/%u: %s v%u", i+1, s->nb_device_extensions,
            s->device_extensions[i].extensionName, s->device_extensions[i].specVersion);
    }

    return VK_SUCCESS;
}

static VkResult create_device(struct vkcontext *s)
{
    uint32_t nb_queues = 0;
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queues_create_info[2];

    const VkDeviceQueueCreateInfo graphics_queue_create_info = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = s->graphics_queue_index,
        .queueCount       = 1,
        .pQueuePriorities = &queue_priority,
    };
    queues_create_info[nb_queues++] = graphics_queue_create_info;

    if (s->present_queue_index != -1 &&
        s->graphics_queue_index != s->present_queue_index) {
        const VkDeviceQueueCreateInfo present_queue_create_info = {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = s->present_queue_index,
            .queueCount       = 1,
            .pQueuePriorities = &queue_priority,
        };
        queues_create_info[nb_queues++] = present_queue_create_info;
    }

    VkPhysicalDeviceVulkan12Features dev_features_vk12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };

    VkPhysicalDeviceVulkan11Features dev_features_vk11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &dev_features_vk12,
    };

    VkPhysicalDeviceFeatures2 dev_features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &dev_features_vk11,
    };

#define ENABLE_FEATURE(dst, query, feature, mandatory) do {                      \
    if (query.feature) {                                                         \
        LOG(DEBUG, "optional feature " #feature " is not supported by device");  \
        dst.feature = VK_TRUE;                                                   \
    } else if (mandatory) {                                                      \
        LOG(ERROR, "mandatory feature " #feature " is not supported by device"); \
        return VK_ERROR_FEATURE_NOT_PRESENT;                                     \
    }                                                                            \
} while (0)                                                                      \

    ENABLE_FEATURE(dev_features2.features, s->dev_features, samplerAnisotropy, 0);
    ENABLE_FEATURE(dev_features2.features, s->dev_features, vertexPipelineStoresAndAtomics, 0);
    ENABLE_FEATURE(dev_features2.features, s->dev_features, fragmentStoresAndAtomics, 0);
    ENABLE_FEATURE(dev_features2.features, s->dev_features, shaderStorageImageExtendedFormats, 0);
    ENABLE_FEATURE(dev_features_vk11, s->dev_features_vk11, samplerYcbcrConversion, 0);
    ENABLE_FEATURE(dev_features_vk12, s->dev_features_vk12, timelineSemaphore, 1);

#undef ENABLE_FEATURE

    NGPU_DARRAY(const char *) enabled_extensions = {0};

    static const char *mandatory_device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#if defined(VK_USE_PLATFORM_MACOS_MVK) || defined(VK_USE_PLATFORM_IOS_MVK)
        VK_EXT_METAL_OBJECTS_EXTENSION_NAME,
#endif
    };

    for (size_t i = 0; i < NGPU_ARRAY_NB(mandatory_device_extensions); i++) {
        if (ngpu_darray_push(&enabled_extensions, mandatory_device_extensions[i]) < 0) {
            ngpu_darray_reset(&enabled_extensions);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    static const char *optional_device_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
#if defined(TARGET_ANDROID)
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
#endif
    };

    for (size_t i = 0; i < NGPU_ARRAY_NB(optional_device_extensions); i++) {
        if (ngpu_vkcontext_has_extension(s, optional_device_extensions[i], 1)) {
            if (ngpu_darray_push(&enabled_extensions, optional_device_extensions[i]) < 0) {
                ngpu_darray_reset(&enabled_extensions);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
    }

    const VkDeviceCreateInfo device_create_info = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &dev_features2,
        .pQueueCreateInfos       = queues_create_info,
        .queueCreateInfoCount    = nb_queues,
        .enabledExtensionCount   = (uint32_t)enabled_extensions.count,
        .ppEnabledExtensionNames = enabled_extensions.data,
    };
    VkResult res = s->funcs.CreateDevice(s->phy_device, &device_create_info, NULL, &s->device);

    ngpu_darray_reset(&enabled_extensions);

    if (res != VK_SUCCESS)
        return res;

    res = load_functions(s, true, true);
    if (res != VK_SUCCESS)
        return res;

    s->funcs.GetDeviceQueue(s->device, s->graphics_queue_index, 0, &s->graphic_queue);
    if (s->present_queue_index != -1)
        s->funcs.GetDeviceQueue(s->device, s->present_queue_index, 0, &s->present_queue);

    return VK_SUCCESS;
}

VkFormat ngpu_vkcontext_find_supported_format(struct vkcontext *s, const VkFormat *formats,
                                              VkImageTiling tiling, VkFormatFeatureFlags features)
{
    uint32_t i = 0;
    while (formats[i]) {
        VkFormatProperties properties;
        s->funcs.GetPhysicalDeviceFormatProperties(s->phy_device, formats[i], &properties);
        if (tiling == VK_IMAGE_TILING_LINEAR &&
            NGPU_HAS_ALL_FLAGS(properties.linearTilingFeatures, features))
            return formats[i];
        if (tiling == VK_IMAGE_TILING_OPTIMAL &&
            NGPU_HAS_ALL_FLAGS(properties.optimalTilingFeatures, features))
            return formats[i];
        i++;
    }
    return VK_FORMAT_UNDEFINED;
}

uint32_t ngpu_vkcontext_find_memory_type(struct vkcontext *s, uint32_t type, VkMemoryPropertyFlags props)
{
    for (uint32_t i = 0; i < s->phydev_mem_props.memoryTypeCount; i++)
        if ((type & (1 << i)) && NGPU_HAS_ALL_FLAGS(s->phydev_mem_props.memoryTypes[i].propertyFlags, props))
            return i;
    return UINT32_MAX;
}

static VkResult query_swapchain_support(struct vkcontext *s)
{
    if (!s->surface)
        return VK_SUCCESS;

    s->funcs.GetPhysicalDeviceSurfaceCapabilitiesKHR(s->phy_device, s->surface, &s->surface_caps);

    s->funcs.GetPhysicalDeviceSurfaceFormatsKHR(s->phy_device, s->surface, &s->nb_surface_formats, NULL);
    if (!s->nb_surface_formats)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    s->surface_formats = ngpu_calloc(s->nb_surface_formats, sizeof(*s->surface_formats));
    if (!s->surface_formats)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->funcs.GetPhysicalDeviceSurfaceFormatsKHR(s->phy_device, s->surface, &s->nb_surface_formats, s->surface_formats);

    s->funcs.GetPhysicalDeviceSurfacePresentModesKHR(s->phy_device, s->surface, &s->nb_present_modes, NULL);
    if (!s->nb_present_modes)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    s->present_modes = ngpu_calloc(s->nb_present_modes, sizeof(*s->present_modes));
    if (!s->present_modes)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->funcs.GetPhysicalDeviceSurfacePresentModesKHR(s->phy_device, s->surface, &s->nb_present_modes, s->present_modes);

    return VK_SUCCESS;
}

VkBool32 ngpu_vkcontext_support_present_mode(const struct vkcontext *s, VkPresentModeKHR mode)
{
    for (uint32_t i = 0; i < s->nb_present_modes; i++) {
        if (s->present_modes[i] == mode) {
            return VK_TRUE;
        }
    }
    return VK_FALSE;
}

static enum ngpu_format ngpu_format_from_vk_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_D32_SFLOAT:         return NGPU_FORMAT_D32_SFLOAT;
    case VK_FORMAT_D16_UNORM:          return NGPU_FORMAT_D16_UNORM;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return NGPU_FORMAT_D32_SFLOAT_S8_UINT;
    case VK_FORMAT_D24_UNORM_S8_UINT:  return NGPU_FORMAT_D24_UNORM_S8_UINT;
    default:
        ngpu_assert(0);
    }
}

static VkResult select_preferred_formats(struct vkcontext *s)
{
    const VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    const VkFormatFeatureFlags features = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;

    const VkFormat depth_stencil_formats[] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        0
    };
    VkFormat format = ngpu_vkcontext_find_supported_format(s, depth_stencil_formats, tiling, features);
    if (!format)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    s->preferred_depth_stencil_format = ngpu_format_from_vk_format(format);

    const VkFormat depth_formats[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D16_UNORM,
        0
    };
    format = ngpu_vkcontext_find_supported_format(s, depth_formats, tiling, features);
    if (!format)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    s->preferred_depth_format = ngpu_format_from_vk_format(format);

    return VK_SUCCESS;
}

struct vk_ext_function {
    const char *name;
    size_t offset;
    int device;
};

#define DECLARE_EXT_FUNC(n, d) {                         \
    .name = "vk" #n,                                     \
    .offset = offsetof(struct vk_functions, n),          \
    .device = d,                                         \
}                                                        \

struct vk_extension {
    const char *name;
    int device;
    const struct vk_ext_function *functions;
} vk_extensions[] = {
    {
        .name = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        .device = 1,
        .functions = (const struct vk_ext_function[]) {
            DECLARE_EXT_FUNC(GetMemoryFdKHR, 1),
            DECLARE_EXT_FUNC(GetMemoryFdPropertiesKHR, 1),
            {0},
        },
    }, {
        .name = VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
        .device = 1,
        .functions = (const struct vk_ext_function[]) {
            DECLARE_EXT_FUNC(GetRefreshCycleDurationGOOGLE, 1),
            DECLARE_EXT_FUNC(GetPastPresentationTimingGOOGLE, 1),
            {0},
        },
    },
#if defined(TARGET_ANDROID)
    {
        .name = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        .device = 1,
        .functions = (const struct vk_ext_function[]) {
            DECLARE_EXT_FUNC(GetAndroidHardwareBufferPropertiesANDROID, 1),
            DECLARE_EXT_FUNC(GetMemoryAndroidHardwareBufferANDROID, 1),
            {0},
        },
    },
#endif
#if defined(TARGET_DARWIN) || defined(TARGET_IPHONE)
    {
        .name = VK_EXT_METAL_OBJECTS_EXTENSION_NAME,
        .device = 1,
        .functions = (const struct vk_ext_function[]) {
            DECLARE_EXT_FUNC(ExportMetalObjectsEXT, 1),
            {0},
        },
    },
#endif
};

static int load_ext_function(struct vkcontext *s, const struct vk_ext_function *func)
{
    PFN_vkVoidFunction *func_ptr = (void *)((uintptr_t)&s->funcs + func->offset);
    if (func->device)
        *func_ptr = s->funcs.GetDeviceProcAddr(s->device, func->name);
    else
        *func_ptr = s->funcs.GetInstanceProcAddr(s->instance, func->name);

    if (!*func_ptr)
        return 0;

    return 1;
}

static VkResult load_ext_functions(struct vkcontext *s)
{
    for (size_t i = 0; i < NGPU_ARRAY_NB(vk_extensions); i++) {
        struct vk_extension *ext = &vk_extensions[i];
        if (!ngpu_vkcontext_has_extension(s, ext->name, ext->device))
            continue;
        for (const struct vk_ext_function *func = ext->functions; func && func->name; func++) {
            if (!load_ext_function(s, func)) {
                LOG(ERROR, "could not load %s() required by extension %s",
                    func->name, ext->name);
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
        }
    }
    return VK_SUCCESS;
}

struct vkcontext *ngpu_vkcontext_create(void)
{
    struct vkcontext *s = ngpu_calloc(1, sizeof(*s));
    return s;
}

VkResult ngpu_vkcontext_init(struct vkcontext *s, const struct ngpu_ctx_params *params)
{
    VkResult res = load_libvulkan(s);
    if (res != VK_SUCCESS)
        return res;

    res = load_functions(s, false, false);
    if (res != VK_SUCCESS) {
        LOG(ERROR, "failed to load global Vulkan functions");
        return res;
    }

    res = create_instance(s, params->platform, params->debug);
    if (res != VK_SUCCESS) {
        LOG(ERROR, "failed to create instance: %s", ngpu_vk_res2str(res));
        return res;
    }

    res = create_window_surface(s, params);
    if (res != VK_SUCCESS) {
        LOG(ERROR, "failed to create window surface: %s", ngpu_vk_res2str(res));
        return res;
    }

    res = enumerate_physical_devices(s, params);
    if (res != VK_SUCCESS)
        return res;

    res = select_physical_device(s, params);
    if (res != VK_SUCCESS)
        return res;

    res = enumerate_extensions(s);
    if (res != VK_SUCCESS)
        return res;

    res = create_device(s);
    if (res != VK_SUCCESS)
        return res;

    res = load_ext_functions(s);
    if (res != VK_SUCCESS)
        return res;

    res = query_swapchain_support(s);
    if (res != VK_SUCCESS)
        return res;

    res = select_preferred_formats(s);
    if (res != VK_SUCCESS)
        return res;

    return VK_SUCCESS;
}

void *ngpu_vkcontext_get_proc_addr(struct vkcontext *s, const char *name)
{
    return (void *)s->funcs.GetInstanceProcAddr(s->instance, name);
}

int ngpu_vkcontext_has_extension(const struct vkcontext *s, const char *name, int device)
{
    uint32_t nb_extensions = device ? s->nb_device_extensions : s->nb_extensions;
    VkExtensionProperties *extensions = device ? s->device_extensions : s->extensions;
    for (uint32_t i = 0; i < nb_extensions; i++) {
        if (!strcmp(extensions[i].extensionName, name))
            return 1;
    }
    return 0;
}

int ngpu_vkcontext_has_extensions(const struct vkcontext *s, size_t extension_count, const char * const *extensions, int device)
{
    for (size_t i = 0; i < extension_count; i++) {
        if (!ngpu_vkcontext_has_extension(s, extensions[i], device))
            return 0;
    }
    return 1;
}

void ngpu_vkcontext_freep(struct vkcontext **sp)
{
    struct vkcontext *s = *sp;
    if (!s)
        return;

    if (s->device) {
        s->funcs.DeviceWaitIdle(s->device);
        s->funcs.DestroyDevice(s->device, NULL);
    }

    if (s->surface && s->funcs.DestroySurfaceKHR)
        s->funcs.DestroySurfaceKHR(s->instance, s->surface, NULL);

    if (s->debug_callback && s->funcs.DestroyDebugUtilsMessengerEXT)
        s->funcs.DestroyDebugUtilsMessengerEXT(s->instance, s->debug_callback, NULL);

    ngpu_freep(&s->present_modes);
    ngpu_freep(&s->surface_formats);
    ngpu_freep(&s->device_extensions);
    ngpu_freep(&s->phy_devices);
    ngpu_freep(&s->layers);
    ngpu_freep(&s->extensions);

    if (s->funcs.DestroyInstance)
        s->funcs.DestroyInstance(s->instance, NULL);

#if defined(TARGET_LINUX)
    if (s->own_x11_display)
        XCloseDisplay(s->x11_display);
#endif

#if !NGPU_VULKAN_STATIC
    if (s->libvulkan) {
# if defined(TARGET_WINDOWS)
        FreeLibrary((HMODULE)s->libvulkan);
# else
        dlclose(s->libvulkan);
# endif
    }
#endif

    ngpu_freep(sp);
}
