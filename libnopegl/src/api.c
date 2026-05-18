/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023 Nope Forge
 * Copyright 2016-2022 GoPro Inc.
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

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "ngl_config.h"
#include "utils/error.h"
#include "utils/time.h"

#include "distmap.h"
#include "internal.h"
#include "node2d.h"
#include "log.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "nopegl/nopegl.h"
#include "nopegl/nopegl_opengl.h"
#include "utils/darray.h"
#include "utils/hmap.h"
#include "utils/memory.h"
#include "utils/utils.h"

#if defined(BACKEND_GL) || defined(BACKEND_GLES)
#include "ngpu/ngpu_opengl.h"
#endif

#if defined(HAVE_VAAPI)
#include "vaapi_ctx.h"
#endif

// Ensure public enums are stored on 32-bit
NGLI_STATIC_ASSERT(sizeof(enum ngl_log_level)           == sizeof(int32_t), "32-bit loglevel enum");
NGLI_STATIC_ASSERT(sizeof(enum ngl_platform_type)       == sizeof(int32_t), "32-bit platform enum");
NGLI_STATIC_ASSERT(sizeof(enum ngl_backend_type)        == sizeof(int32_t), "32-bit backend enum");
NGLI_STATIC_ASSERT(sizeof(enum ngl_capture_buffer_type) == sizeof(int32_t), "32-bit capture enum");

#if defined(TARGET_IPHONE) || defined(TARGET_ANDROID)
# define DEFAULT_BACKEND NGL_BACKEND_OPENGLES
#else
# define DEFAULT_BACKEND NGL_BACKEND_OPENGL
#endif

extern const struct api_impl api_gl;
extern const struct api_impl api_vk;

static const struct api_impl *api_map[NGL_BACKEND_NB] = {
#ifdef BACKEND_GL
    [NGL_BACKEND_OPENGL] = &api_gl,
#endif
#ifdef BACKEND_GLES
    [NGL_BACKEND_OPENGLES] = &api_gl,
#endif
#ifdef BACKEND_VK
    [NGL_BACKEND_VULKAN] = &api_vk,
#endif
};

void ngl_log_set_callback(void *arg, ngl_log_callback_type callback)
{
    ngli_log_set_callback(arg, callback);
}

void ngl_log_set_min_level(enum ngl_log_level level)
{
    ngli_log_set_min_level(level);
}

const char *ngl_error_to_string(int error)
{
  return ngli_error_to_string(error);
}

static enum ngl_platform_type get_default_platform(void)
{
#if defined(TARGET_LINUX)
    return NGL_PLATFORM_XLIB;
#elif defined(TARGET_IPHONE)
    return NGL_PLATFORM_IOS;
#elif defined(TARGET_DARWIN)
    return NGL_PLATFORM_MACOS;
#elif defined(TARGET_ANDROID)
    return NGL_PLATFORM_ANDROID;
#elif defined(TARGET_WINDOWS)
    return NGL_PLATFORM_WINDOWS;
#else
    NGLI_STATIC_ASSERT(0, "no default platform");
#endif
}

static enum ngpu_platform_type ngl_platform_to_ngpu(enum ngl_platform_type platform)
{
    switch (platform) {
    case NGL_PLATFORM_XLIB:    return NGPU_PLATFORM_XLIB;
    case NGL_PLATFORM_ANDROID: return NGPU_PLATFORM_ANDROID;
    case NGL_PLATFORM_MACOS:   return NGPU_PLATFORM_MACOS;
    case NGL_PLATFORM_IOS:     return NGPU_PLATFORM_IOS;
    case NGL_PLATFORM_WINDOWS: return NGPU_PLATFORM_WINDOWS;
    case NGL_PLATFORM_WAYLAND: return NGPU_PLATFORM_WAYLAND;
    default: ngli_assert(0);
    }
}

static enum ngpu_backend_type ngl_backend_type_to_ngpu(enum ngl_backend_type backend_type)
{
    switch (backend_type) {
    case NGL_BACKEND_OPENGL:   return NGPU_BACKEND_OPENGL;
    case NGL_BACKEND_OPENGLES: return NGPU_BACKEND_OPENGLES;
    case NGL_BACKEND_VULKAN:   return NGPU_BACKEND_VULKAN;
    default: ngli_assert(0);
    }
}

static enum ngl_backend_type ngpu_backend_type_to_ngl(enum ngpu_backend_type backend_type)
{
    switch (backend_type) {
    case NGPU_BACKEND_OPENGL:   return NGL_BACKEND_OPENGL;
    case NGPU_BACKEND_OPENGLES: return NGL_BACKEND_OPENGLES;
    case NGPU_BACKEND_VULKAN:   return NGL_BACKEND_VULKAN;
    default: ngli_assert(0);
    }
}

static enum ngpu_capture_buffer_type ngl_capture_buffer_type_to_ngpu(enum ngl_capture_buffer_type type)
{
    switch (type) {
    case NGL_CAPTURE_BUFFER_TYPE_CPU:       return NGPU_CAPTURE_BUFFER_TYPE_CPU;
    case NGL_CAPTURE_BUFFER_TYPE_COREVIDEO: return NGPU_CAPTURE_BUFFER_TYPE_COREVIDEO;
    default: ngli_assert(0);
    }
}

static struct ngpu_ctx *gpu_ctx_create_from_config(const struct ngl_config *config)
{
    struct ngpu_ctx_params params = {
        .platform             = ngl_platform_to_ngpu(config->platform),
        .backend              = ngl_backend_type_to_ngpu(config->backend),
        .display              = config->display,
        .window               = config->window,
        .swap_interval        = config->swap_interval,
        .offscreen            = config->offscreen,
        .width                = config->width,
        .height               = config->height,
        .samples              = config->samples,
        .set_surface_pts      = config->set_surface_pts,
        .capture_buffer       = config->capture_buffer,
        .capture_buffer_type  = ngl_capture_buffer_type_to_ngpu(config->capture_buffer_type),
        .debug                = config->debug,
        .timer_queries        = config->hud,
        .shared_ctx           = config->shared_gpu_ctx,
    };
    memcpy(params.clear_color, config->clear_color, sizeof(params.clear_color));

    switch (config->backend) {
#if defined(BACKEND_GL) || defined(BACKEND_GLES)
    case NGL_BACKEND_OPENGL:
    case NGL_BACKEND_OPENGLES: {
        const struct ngl_config_gl *config_gl = config->backend_config;
        struct ngpu_ctx_params_gl backend_params = {0};
        if (config->backend_config) {
            backend_params.external = config_gl->external;
            backend_params.external_framebuffer = config_gl->external_framebuffer;
            params.backend_params = &backend_params;
        }
        return ngpu_ctx_create(&params);
    }
#endif
#if defined(BACKEND_VK)
    case NGL_BACKEND_VULKAN: {
        return ngpu_ctx_create(&params);
    }
#endif
    default:
        return NULL;
    }
}

static enum ngl_platform_type get_platform(enum ngl_platform_type platform_type)
{
    if (platform_type == NGL_PLATFORM_AUTO)
        return get_default_platform();
    return platform_type;
}

static const char *get_cap_string_id(unsigned cap_id)
{
    switch (cap_id) {
    case NGL_CAP_COMPUTE:                       return "compute";
    case NGL_CAP_DEPTH_STENCIL_RESOLVE:         return "depth_stencil_resolve";
    case NGL_CAP_MAX_COLOR_ATTACHMENTS:         return "max_color_attachments";
    case NGL_CAP_MAX_COMPUTE_GROUP_COUNT_X:     return "max_compute_group_count_x";
    case NGL_CAP_MAX_COMPUTE_GROUP_COUNT_Y:     return "max_compute_group_count_y";
    case NGL_CAP_MAX_COMPUTE_GROUP_COUNT_Z:     return "max_compute_group_count_z";
    case NGL_CAP_MAX_COMPUTE_GROUP_INVOCATIONS: return "max_compute_group_invocations";
    case NGL_CAP_MAX_COMPUTE_GROUP_SIZE_X:      return "max_compute_group_size_x";
    case NGL_CAP_MAX_COMPUTE_GROUP_SIZE_Y:      return "max_compute_group_size_y";
    case NGL_CAP_MAX_COMPUTE_GROUP_SIZE_Z:      return "max_compute_group_size_z";
    case NGL_CAP_MAX_COMPUTE_SHARED_MEMORY_SIZE:return "max_compute_shared_memory_size";
    case NGL_CAP_MAX_SAMPLES:                   return "max_samples";
    case NGL_CAP_MAX_TEXTURE_ARRAY_LAYERS:      return "max_texture_array_layers";
    case NGL_CAP_MAX_TEXTURE_DIMENSION_1D:      return "max_texture_dimension_1d";
    case NGL_CAP_MAX_TEXTURE_DIMENSION_2D:      return "max_texture_dimension_2d";
    case NGL_CAP_MAX_TEXTURE_DIMENSION_3D:      return "max_texture_dimension_3d";
    case NGL_CAP_MAX_TEXTURE_DIMENSION_CUBE:    return "max_texture_dimension_cube";
    case NGL_CAP_TEXT_LIBRARIES:                return "text_libraries";
    default:                                    ngli_assert(0);
    }
}

#define CAP(cap_id, value) {cap_id, get_cap_string_id(cap_id), value}

static int load_caps(struct ngl_backend *backend, const struct ngpu_ctx *gpu_ctx)
{
    const uint64_t features = ngpu_ctx_get_features(gpu_ctx);
    const uint32_t has_compute    = NGLI_HAS_ALL_FLAGS(features, NGPU_FEATURE_COMPUTE_BIT);
    const uint32_t has_ds_resolve = NGLI_HAS_ALL_FLAGS(features, NGPU_FEATURE_DEPTH_STENCIL_RESOLVE_BIT);

    const struct ngpu_limits *limits = ngpu_ctx_get_limits(gpu_ctx);
    const struct ngl_cap caps[] = {
        CAP(NGL_CAP_COMPUTE,                       has_compute),
        CAP(NGL_CAP_DEPTH_STENCIL_RESOLVE,         has_ds_resolve),
        CAP(NGL_CAP_MAX_COLOR_ATTACHMENTS,         limits->max_color_attachments),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_COUNT_X,     limits->max_compute_work_group_count[0]),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_COUNT_Y,     limits->max_compute_work_group_count[1]),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_COUNT_Z,     limits->max_compute_work_group_count[2]),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_INVOCATIONS, limits->max_compute_work_group_invocations),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_SIZE_X,      limits->max_compute_work_group_size[0]),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_SIZE_Y,      limits->max_compute_work_group_size[1]),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_SIZE_Z,      limits->max_compute_work_group_size[2]),
        CAP(NGL_CAP_MAX_COMPUTE_SHARED_MEMORY_SIZE,limits->max_compute_shared_memory_size),
        CAP(NGL_CAP_MAX_SAMPLES,                   limits->max_samples),
        CAP(NGL_CAP_MAX_TEXTURE_ARRAY_LAYERS,      limits->max_texture_array_layers),
        CAP(NGL_CAP_MAX_TEXTURE_DIMENSION_1D,      limits->max_texture_dimension_1d),
        CAP(NGL_CAP_MAX_TEXTURE_DIMENSION_2D,      limits->max_texture_dimension_2d),
        CAP(NGL_CAP_MAX_TEXTURE_DIMENSION_3D,      limits->max_texture_dimension_3d),
        CAP(NGL_CAP_MAX_TEXTURE_DIMENSION_CUBE,    limits->max_texture_dimension_cube),
        CAP(NGL_CAP_TEXT_LIBRARIES,                HAVE_TEXT_LIBRARIES),
    };

    backend->nb_caps = NGLI_ARRAY_NB(caps);
    backend->caps = ngli_memdup(caps, sizeof(caps));
    if (!backend->caps)
        return NGL_ERROR_MEMORY;

    return 0;
}

static int backend_init(struct ngl_backend *backend, struct ngpu_ctx *gpu_ctx)
{
    const enum ngpu_backend_type backend_type = ngpu_ctx_get_backend_type(gpu_ctx);
    const enum ngl_backend_type backend_id = ngpu_backend_type_to_ngl(backend_type);
    backend->id         = backend_id;
    backend->string_id  = ngpu_backend_get_string_id(backend_type);
    backend->name       = ngpu_backend_get_full_name(backend_type);
    backend->is_default = backend_id == DEFAULT_BACKEND;

    /* If GPU context is not initialized, return early */
    if (!ngpu_ctx_get_version(gpu_ctx))
        return 0;

    int ret = load_caps(backend, gpu_ctx);
    if (ret < 0)
        return ret;

    return 0;
}

static int backend_copy(struct ngl_backend *dst, const struct ngl_backend *src)
{
    *dst = *src;

    dst->caps = ngli_memdup(src->caps, src->nb_caps * sizeof(*src->caps));
    if (!dst->caps)
        return NGL_ERROR_MEMORY;

    return 0;
}

static void backend_reset(struct ngl_backend *backend)
{
    ngli_free(backend->caps);
    memset(backend, 0, sizeof(*backend));
}

static void reset_scene(struct ngl_ctx *s, int action)
{
    ngli_queue_wait(&s->background_queue);
    ngli_hud_freep(&s->hud);
    if (s->scene) {
        ngli_node_detach_ctx(s->scene->params.root, s);
        if (action == NGLI_ACTION_UNREF_SCENE)
            ngl_scene_unrefp(&s->scene);
    }
}

static struct ngpu_viewport compute_scene_viewport(const struct ngl_scene *scene, uint32_t w, uint32_t h)
{
    const float width = (float)w;
    const float height = (float)h;
    struct ngpu_viewport vp = {0.f, 0.f, width, height};

    if (!scene)
        return vp;

    const int32_t canvas_w = scene->params.width;
    const int32_t canvas_h = scene->params.height;
    if (canvas_w <= 0 || canvas_h <= 0)
        return vp;

    const float scale = NGLI_MIN(width / (float)canvas_w, height / (float)canvas_h);
    vp.width  = (float)canvas_w * scale;
    vp.height = (float)canvas_h * scale;
    vp.x = (width  - vp.width)  * 0.5f;
    vp.y = (height - vp.height) * 0.5f;

    return vp;
}

int ngli_ctx_set_scene(struct ngl_ctx *s, struct ngl_scene *scene)
{
    ngpu_ctx_wait_idle(s->gpu_ctx);
    reset_scene(s, NGLI_ACTION_UNREF_SCENE);

    s->default_graphics_state = NGPU_GRAPHICS_STATE_DEFAULTS;
    s->default_rendertarget_layout = *ngpu_ctx_get_default_rendertarget_layout(s->gpu_ctx);

    int ret = ngpu_ctx_begin_update(s->gpu_ctx);
    if (ret < 0)
        return ret;

    if (scene) {
        if (!scene->params.root) {
            LOG(ERROR, "specified scene doesn't contain a graph");
            ret = NGL_ERROR_INVALID_ARG;
            goto fail;
        }

        if (scene->params.root->ctx) {
            LOG(ERROR, "the specified scene is already associated with a rendering context");
            ret = NGL_ERROR_INVALID_USAGE;
            goto fail;
        }

        s->scene = ngl_scene_ref(scene);

        ret = ngli_node_attach_ctx(scene->params.root, s);
        if (ret < 0) {
            LOG(ERROR, "failed to attach scene");
            goto fail;
        }
    }

    // Re-compute the viewport according to the new scene aspect ratio
    uint32_t width, height;
    ngpu_ctx_get_default_rendertarget_size(s->gpu_ctx, &width, &height);
    s->viewport = compute_scene_viewport(s->scene, width, height);
    s->scissor = (struct ngpu_scissor){0, 0, width, height};

    const struct ngl_config *config = &s->config;
    if (config->hud) {
        s->hud = ngli_hud_create(s);
        if (!s->hud) {
            ret = NGL_ERROR_MEMORY;
            goto fail;
        }

        ret = ngli_hud_init(s->hud);
        if (ret < 0)
            goto fail;
    }

    ngpu_ctx_end_update(s->gpu_ctx, NULL);
    return 0;

fail:
    ngpu_ctx_end_update(s->gpu_ctx, NULL);
    reset_scene(s, NGLI_ACTION_UNREF_SCENE);
    return ret;
}

struct ngl_frame {
    struct ngl_ctx *ctx;
    uint32_t index;
    struct ngpu_texture *texture;
    struct ngpu_fence *signal_fence;
};

void ngli_ctx_reset(struct ngl_ctx *s, int action)
{
    if (s->gpu_ctx)
        ngpu_ctx_wait_idle(s->gpu_ctx);

    if (s->frame_slots) {
        for (uint32_t i = 0; i < s->nb_frame_slots; i++) {
            if (s->frame_slots[i].frame) {
                ngpu_texture_unrefp(&s->frame_slots[i].frame->texture);
                ngpu_fence_freep(&s->frame_slots[i].frame->signal_fence);
                ngli_freep(&s->frame_slots[i].frame);
            }
            if (s->frame_slots[i].release_fence)
                ngpu_fence_freep(&s->frame_slots[i].release_fence);
        }
        ngli_freep(&s->frame_slots);
        s->nb_frame_slots = 0;
    }
    reset_scene(s, action);
#if defined(HAVE_VAAPI)
    ngli_vaapi_ctx_reset(&s->vaapi_ctx);
#endif
#if defined(TARGET_ANDROID)
    ngli_android_ctx_reset(&s->android_ctx);
#endif
    if (s->gpu_ctx) {
        const uint32_t n = ngpu_ctx_get_nb_in_flight_frames(s->gpu_ctx);
        if (s->draw_staging_buffers) {
            for (uint32_t i = 0; i < n; i++)
                ngpu_staging_buffer_freep(&s->draw_staging_buffers[i]);
        }
        if (s->update_staging_buffers) {
            for (uint32_t i = 0; i < n; i++)
                ngpu_staging_buffer_freep(&s->update_staging_buffers[i]);
        }
    }
    ngli_freep(&s->draw_staging_buffers);
    ngli_freep(&s->update_staging_buffers);
    s->current_staging_buffer = NULL;
    ngli_hmap_freep(&s->text_builtin_atlasses);
#if HAVE_TEXT_LIBRARIES
    FT_Done_FreeType(s->ft_library);
#endif
    ngpu_ctx_freep(&s->gpu_ctx);
    ngli_config_reset(&s->config);
    backend_reset(&s->backend);
}

void ngli_free_text_builtin_atlas(void *user_arg, void *data)
{
    struct text_builtin_atlas *atlas = data;
    ngli_slug_freep(&atlas->slug);
    ngli_freep(&atlas);
}

int ngli_ctx_configure(struct ngl_ctx *s, const struct ngl_config *config)
{
    int ret = ngli_config_copy(&s->config, config);
    if (ret < 0)
        return ret;

    ret = ngli_config_set_debug_defaults(&s->config);
    if (ret < 0)
        return ret;

    s->gpu_ctx = gpu_ctx_create_from_config(&s->config);
    if (!s->gpu_ctx) {
        ngli_config_reset(&s->config);
        return NGL_ERROR_MEMORY;
    }

    ret = ngpu_ctx_init(s->gpu_ctx);
    if (ret < 0) {
        LOG(ERROR, "could not initialize gpu context: %s", NGLI_RET_STR(ret));
        ngpu_ctx_freep(&s->gpu_ctx);
        ngli_config_reset(&s->config);
        return ret;
    }

    s->text_builtin_atlasses = ngli_hmap_create(NGLI_HMAP_TYPE_STR);
    if (!s->text_builtin_atlasses) {
        ret = NGL_ERROR_MEMORY;
        goto fail;
    }
    ngli_hmap_set_free_func(s->text_builtin_atlasses, ngli_free_text_builtin_atlas, NULL);

#if HAVE_TEXT_LIBRARIES
    FT_Error ft_error = FT_Init_FreeType(&s->ft_library);
    if (ft_error) {
        LOG(ERROR, "unable to initialize FreeType");
        ret = NGL_ERROR_EXTERNAL;
        goto fail;
    }
#endif

#if defined(HAVE_VAAPI)
    ret = ngli_vaapi_ctx_init(s->gpu_ctx, &s->vaapi_ctx);
    if (ret < 0)
        LOG(WARNING, "could not initialize vaapi context");
#endif

#if defined(TARGET_ANDROID)
    struct android_ctx *android_ctx = &s->android_ctx;
    ret = ngli_android_ctx_init(s->gpu_ctx, android_ctx);
    if (ret < 0)
        LOG(WARNING, "could not initialize Android context");
#endif

    const uint32_t nb_in_flight_frames = ngpu_ctx_get_nb_in_flight_frames(s->gpu_ctx);

    s->frame_slots = ngli_calloc(nb_in_flight_frames, sizeof(*s->frame_slots));
    if (!s->frame_slots) {
        ret = NGL_ERROR_MEMORY;
        goto fail;
    }
    s->nb_frame_slots = nb_in_flight_frames;

    s->update_staging_buffers = ngli_calloc(nb_in_flight_frames, sizeof(*s->update_staging_buffers));
    s->draw_staging_buffers = ngli_calloc(nb_in_flight_frames, sizeof(*s->draw_staging_buffers));
    if (!s->update_staging_buffers || !s->draw_staging_buffers) {
        ret = NGL_ERROR_MEMORY;
        goto fail;
    }

    for (uint32_t i = 0; i < nb_in_flight_frames; i++) {
        s->update_staging_buffers[i] = ngpu_staging_buffer_create(s->gpu_ctx);
        s->draw_staging_buffers[i] = ngpu_staging_buffer_create(s->gpu_ctx);
        if (!s->update_staging_buffers[i] || !s->draw_staging_buffers[i]) {
            ret = NGL_ERROR_MEMORY;
            goto fail;
        }
    }

    s->current_staging_buffer = s->update_staging_buffers[0];

    ngpu_ctx_get_projection_matrix(s->gpu_ctx, s->default_projection_matrix.m);
    ngli_darray_clear(&s->projection_matrix_stack);
    if (ngli_darray_push(&s->projection_matrix_stack, s->default_projection_matrix) < 0) {
        ret = NGL_ERROR_MEMORY;
        goto fail;
    }

    struct ngl_scene *old_scene = s->scene; // note: the old scene is detached
    s->scene = NULL; // make sure the old scene is not unreferenced by set_scene()
    ret = ngli_ctx_set_scene(s, old_scene);
    if (ret < 0) {
        s->scene = old_scene; // restore detached scene on error
        goto fail;
    }
    ngl_scene_unrefp(&old_scene); // ngli_ctx_set_scene() incremented the ref counter of the scene so we can safely unref here

    return 0;

fail:
    ngli_ctx_reset(s, NGLI_ACTION_KEEP_SCENE);
    return ret;
}

int ngli_ctx_resize(struct ngl_ctx *s, uint32_t width, uint32_t height)
{
    int ret = ngpu_ctx_resize(s->gpu_ctx, width, height);
    if (ret < 0)
        return ret;

    s->viewport = compute_scene_viewport(s->scene, width, height);
    s->scissor = (struct ngpu_scissor){0, 0, width, height};

    return 0;
}

int ngli_ctx_get_viewport(struct ngl_ctx *s, int32_t *viewport)
{
    const int32_t vp_i32[] = {
        (int32_t)s->viewport.x,
        (int32_t)s->viewport.y,
        (int32_t)s->viewport.width,
        (int32_t)s->viewport.height
    };
    memcpy(viewport, vp_i32, sizeof(vp_i32));
    return 0;
}

int ngli_ctx_set_capture_buffer(struct ngl_ctx *s, void *capture_buffer)
{
    struct ngl_config *config = &s->config;

    int ret = ngpu_ctx_set_capture_buffer(s->gpu_ctx, capture_buffer);
    if (ret < 0) {
        ngli_ctx_reset(s, NGLI_ACTION_KEEP_SCENE);
        return ret;
    }

    config->capture_buffer = capture_buffer;

    return 0;
}

int ngli_ctx_prepare_draw(struct ngl_ctx *s, double t)
{
    const int64_t start_time = s->hud ? ngli_gettime_relative() : 0;

    uint32_t frame_index = ngpu_ctx_advance_frame(s->gpu_ctx);
    LOG(DEBUG, "start frame @ index=%u t=%f", frame_index, t);

    s->current_staging_buffer = s->update_staging_buffers[frame_index];
    ngpu_staging_buffer_reset(s->current_staging_buffer);

    int ret = ngpu_ctx_begin_update(s->gpu_ctx);
    if (ret < 0)
        return ret;

    struct ngl_scene *scene = s->scene;
    if (!scene) {
        return ngpu_ctx_end_update(s->gpu_ctx, NULL);
    }

    struct ngl_node *root = scene->params.root;
    LOG(DEBUG, "prepare scene %s @ t=%f", root->label, t);

    ret = ngli_node_honor_release_prefetch(root, t);
    if (ret < 0)
        return ret;

    ret = ngli_node_update(root, t);
    if (ret < 0)
        return ret;

    ret = ngpu_staging_buffer_flush(s->current_staging_buffer);
    if (ret < 0)
        return ret;

    ret = ngpu_ctx_end_update(s->gpu_ctx, NULL);
    if (ret < 0)
        return ret;

    s->cpu_update_time = s->hud ? ngli_gettime_relative() - start_time : 0;

    return 0;
}

int ngli_ctx_draw(struct ngl_ctx *s, double t, struct ngpu_fence *wait_fence, struct ngpu_fence **signal_fence)
{
    int ret = ngli_ctx_prepare_draw(s, t);
    if (ret < 0)
        return ret;

    ret = ngpu_ctx_begin_draw(s->gpu_ctx);
    if (ret < 0)
        return ret;

    const int64_t cpu_start_time = s->hud ? ngli_gettime_relative() : 0;

    s->current_rendertarget = ngpu_ctx_get_default_rendertarget(s->gpu_ctx);

    const uint32_t frame_index = ngpu_ctx_get_current_frame_index(s->gpu_ctx);
    s->current_staging_buffer = s->draw_staging_buffers[frame_index];
    ngpu_staging_buffer_reset(s->current_staging_buffer);

    ngli_darray_clear(&s->bounding_box_nodes);

    struct ngl_scene *scene = s->scene;
    if (scene) {
        LOG(DEBUG, "draw scene %s @ t=%f", scene->params.root->label, t);
        ngli_node_pre_draw(scene->params.root);
        ngli_node_draw(scene->params.root);
    }

    if (!ngpu_ctx_is_render_pass_active(s->gpu_ctx)) {
        ngpu_ctx_begin_render_pass(s->gpu_ctx, s->current_rendertarget);
    }

    if (s->hud) {
        s->cpu_draw_time = ngli_gettime_relative() - cpu_start_time;
        ngli_hud_draw(s->hud);
    }

    if (ngpu_ctx_is_render_pass_active(s->gpu_ctx)) {
        ngpu_ctx_end_render_pass(s->gpu_ctx);
    }

    if (s->hud) {
        ngpu_ctx_query_draw_time(s->gpu_ctx, &s->gpu_draw_time);
    }

    ret = ngpu_staging_buffer_flush(s->current_staging_buffer);
    if (ret < 0)
        return ret;

    return ngpu_ctx_end_draw(s->gpu_ctx, t, wait_fence, signal_fence);
}

enum probe_mode {
    PROBE_MODE_FULL,
    PROBE_MODE_NO_GRAPHICS,
};

static int backend_probe(struct ngl_backend *backend, const struct ngl_config *config, enum probe_mode mode)
{
    int ret = 0;

    struct ngpu_ctx *gpu_ctx = gpu_ctx_create_from_config(config);
    if (!gpu_ctx)
        return NGL_ERROR_MEMORY;

    if (mode == PROBE_MODE_FULL) {
        ret = ngpu_ctx_init(gpu_ctx);
        if (ret < 0)
            goto end;
    }

    ret = backend_init(backend, gpu_ctx);
    if (ret < 0)
        goto end;

end:
    ngpu_ctx_freep(&gpu_ctx);
    return ret;
}

static int backends_probe(const struct ngl_config *user_config, size_t *nb_backendsp, struct ngl_backend **backendsp, enum probe_mode mode)
{
    static const struct ngl_config default_config = {
        .width     = 1,
        .height    = 1,
        .offscreen = 1,
    };

    if (!user_config)
        user_config = &default_config;

    const enum ngl_platform_type platform = get_platform(user_config->platform);

    struct ngl_backend *backends = ngli_calloc(NGLI_ARRAY_NB(api_map), sizeof(*backends));
    if (!backends)
        return NGL_ERROR_MEMORY;
    size_t nb_backends = 0;

    for (size_t i = 0; i < NGLI_ARRAY_NB(api_map); i++) {
        if (!api_map[i])
            continue;
        const enum ngl_backend_type backend_id = (enum ngl_backend_type)i;
        if (user_config->backend != NGL_BACKEND_AUTO && user_config->backend != backend_id)
            continue;
        struct ngl_config config = *user_config;
        config.backend = backend_id;
        config.platform = platform;

        int ret = backend_probe(&backends[nb_backends], &config, mode);
        if (ret < 0)
            continue;

        nb_backends++;
    }

    if (!nb_backends)
        ngl_backends_freep(&backends);

    *backendsp = backends;
    *nb_backendsp = nb_backends;
    return 0;
}

int ngl_backends_probe(const struct ngl_config *user_config, size_t *nb_backendsp, struct ngl_backend **backendsp)
{
    return backends_probe(user_config, nb_backendsp, backendsp, PROBE_MODE_FULL);
}

int ngl_backends_get(const struct ngl_config *user_config, size_t *nb_backendsp, struct ngl_backend **backendsp)
{
    return backends_probe(user_config, nb_backendsp, backendsp, PROBE_MODE_NO_GRAPHICS);
}

void ngl_backends_freep(struct ngl_backend **backendsp)
{
    struct ngl_backend *backends = *backendsp;
    if (!backends)
        return;
    for (size_t i = 0; i < NGLI_ARRAY_NB(api_map); i++)
        backend_reset(&backends[i]);
    ngli_freep(backendsp);
}

struct ngl_ctx *ngl_create(void)
{
    struct ngl_ctx *s = ngli_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    int ret = ngli_queue_init(&s->background_queue, "ngl-bg-thread", 32, 1, s);
    if (ret < 0)
        goto fail;

    if (pthread_mutex_init(&s->frame_slots_lock, NULL) != 0)
        goto fail;

    static const struct ngli_mat4 id_matrix = {.m = NGLI_MAT4_IDENTITY};
    s->default_modelview_matrix = id_matrix;
    s->default_projection_matrix = id_matrix;

    if (ngli_darray_push(&s->modelview_matrix_stack, id_matrix) < 0 ||
        ngli_darray_push(&s->projection_matrix_stack, id_matrix) < 0 ||
        ngli_darray_push(&s->transform_2d_stack, id_matrix) < 0)
        goto fail;

    if (ngli_darray_push(&s->opacity_2d_stack, 1.f) < 0)
        goto fail;

    LOG(INFO, "context create in nope.gl v%d.%d.%d",
        NGL_VERSION_MAJOR, NGL_VERSION_MINOR, NGL_VERSION_MICRO);

    return s;

fail:
    ngl_freep(&s);
    return NULL;
}

int ngl_configure(struct ngl_ctx *s, const struct ngl_config *user_config)
{
    if (s->configured) {
        s->api_impl->reset(s, NGLI_ACTION_KEEP_SCENE);
        s->configured = 0;
    }

    if (!user_config) {
        LOG(ERROR, "context configuration cannot be NULL");
        return NGL_ERROR_INVALID_ARG;
    }

    if (user_config->backend == NGL_BACKEND_AUTO && user_config->backend_config) {
        LOG(ERROR, "backend specific configuration is not allowed "
                   "while automatic backend selection is used");
        return NGL_ERROR_INVALID_USAGE;
    }

    struct ngl_config config = *user_config;
    if (config.backend == NGL_BACKEND_AUTO)
        config.backend = DEFAULT_BACKEND;
    if (config.platform == NGL_PLATFORM_AUTO)
        config.platform = get_default_platform();

    if (config.backend < 0 ||
        config.backend >= NGLI_ARRAY_NB(api_map)) {
        LOG(ERROR, "unknown backend %u", config.backend);
        return NGL_ERROR_INVALID_ARG;
    }

    s->api_impl = api_map[config.backend];
    if (!s->api_impl) {
        LOG(ERROR, "backend \"%s\" not available with this build",
            ngpu_backend_get_string_id(ngl_backend_type_to_ngpu(config.backend)));
        return NGL_ERROR_UNSUPPORTED;
    }

    int ret = s->api_impl->configure(s, &config);
    if (ret < 0)
        return ret;

    ret = backend_init(&s->backend, s->gpu_ctx);
    if (ret < 0)
        return ret;

    s->configured = 1;
    return 0;
}

int ngl_get_backend(struct ngl_ctx *s, struct ngl_backend *backend)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured in order to get the information of the selected backend");
        return NGL_ERROR_INVALID_USAGE;
    }

    int ret = backend_copy(backend, &s->backend);
    if (ret < 0)
        return ret;

    return 0;
}

void ngl_reset_backend(struct ngl_backend *backend)
{
    backend_reset(backend);
}

int ngl_resize(struct ngl_ctx *s, uint32_t width, uint32_t height)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before resizing rendering buffers");
        return NGL_ERROR_INVALID_USAGE;
    }

    return s->api_impl->resize(s, width, height);
}

int ngl_get_viewport(struct ngl_ctx *s, int32_t *viewport)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured to get the viewport");
        return NGL_ERROR_INVALID_USAGE;
    }

    return s->api_impl->get_viewport(s, viewport);
}

int ngl_set_capture_buffer(struct ngl_ctx *s, void *capture_buffer)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before setting a capture buffer");
        return NGL_ERROR_INVALID_USAGE;
    }

    int ret = s->api_impl->set_capture_buffer(s, capture_buffer);
    if (ret < 0) {
        s->configured = 0;
        return ret;
    }
    return ret;
}

int ngl_set_scene(struct ngl_ctx *s, struct ngl_scene *scene)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before setting a scene");
        return NGL_ERROR_INVALID_USAGE;
    }

    return s->api_impl->set_scene(s, scene);
}

int ngli_prepare_draw(struct ngl_ctx *s, double t)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before updating");
        return NGL_ERROR_INVALID_USAGE;
    }

    return s->api_impl->prepare_draw(s, t);
}

int ngl_update(struct ngl_ctx *s, double t)
{
    return ngli_prepare_draw(s, t);
}

struct ngpu_texture *ngl_frame_get_texture(const struct ngl_frame *f)
{
    return f->texture;
}

struct ngpu_fence *ngl_frame_get_signal_fence(const struct ngl_frame *f)
{
    return f->signal_fence;
}

void ngl_frame_release(struct ngl_frame *f, struct ngpu_fence *fence)
{
    if (!f)
        return;

    struct ngl_ctx *s = f->ctx;
    struct ngli_frame_slot *slot = &s->frame_slots[f->index];

    pthread_mutex_lock(&s->frame_slots_lock);
    slot->frame = NULL;
    /*
     * Release previous fence.
     */
    if (slot->release_fence)
        ngpu_fence_freep(&slot->release_fence);
    /*
     * Stash (and take ownership) the consumer fence on the slot so the next
     * ngl_draw() that picks this slot can wait on it before writing.
     */
    slot->release_fence = fence;
    pthread_mutex_unlock(&s->frame_slots_lock);

    ngpu_texture_unrefp(&f->texture);
    ngpu_fence_freep(&f->signal_fence);
    ngli_freep(&f);
}

int ngl_draw(struct ngl_ctx *s, double t, struct ngl_draw_output *output)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before drawing");
        return NGL_ERROR_INVALID_USAGE;
    }

    /*
     * Check next frame. If it is still held by the user, refuse to draw and
     * return NGL_ERROR_BUSY.
     */
    const uint32_t next_frame_slot_index =
        (ngpu_ctx_get_current_frame_index(s->gpu_ctx) + 1) % s->nb_frame_slots;
    struct ngli_frame_slot *slot = &s->frame_slots[next_frame_slot_index];

    pthread_mutex_lock(&s->frame_slots_lock);
    if (slot->frame) {
        pthread_mutex_unlock(&s->frame_slots_lock);
        return NGL_ERROR_BUSY;
    }
    struct ngpu_fence *release_fence = slot->release_fence;
    slot->release_fence = NULL;
    pthread_mutex_unlock(&s->frame_slots_lock);

    struct ngl_frame *frame = NULL;
    if (output) {
        frame = ngli_calloc(1, sizeof(*frame));
        if (!frame) {
            ngpu_fence_freep(&release_fence);
            return NGL_ERROR_MEMORY;
        }
        frame->ctx = s;
        frame->index = next_frame_slot_index;
    }

    struct ngpu_fence **signal_fencep = frame ? &frame->signal_fence : NULL;
    int ret = s->api_impl->draw(s, t, release_fence, signal_fencep);

    ngpu_fence_freep(&release_fence);

    if (ret < 0) {
        if (frame) {
            ngpu_fence_freep(&frame->signal_fence);
            ngli_freep(&frame);
        }
        return ret;
    }

    if (frame) {
        struct ngpu_rendertarget *rt = ngpu_ctx_get_default_rendertarget(s->gpu_ctx);
        struct ngpu_texture *texture = rt ? ngpu_rendertarget_get_color_texture(rt, 0) : NULL;
        /*
         * Hold a ref on the texture so it survives a concurrent resize on
         * this ngl context — resize frees and re-creates the offscreen color
         * textures, and the consumer may still be sampling this one.
         */
        frame->texture = texture ? ngpu_texture_ref(texture) : NULL;
        pthread_mutex_lock(&s->frame_slots_lock);
        slot->frame = frame;
        pthread_mutex_unlock(&s->frame_slots_lock);
        output->frame = frame;
    }

    return 0;
}

NGL_API int ngl_get_nodes_at_point(struct ngl_ctx *s, const float *point, size_t *nb_nodesp, struct ngl_node ***nodesp)
{
    *nb_nodesp = 0;
    *nodesp = NULL;

    ngli_darray_clear(&s->intersecting_nodes);

    const NGLI_ALIGNED_VEC(pixel_point) = {point[0], point[1], 0.f, 1.f};

    for (size_t i = 0; i < s->bounding_box_nodes.count; i++) {
        struct ngl_node *bounding_box_node = s->bounding_box_nodes.data[i];
        /* Only test leaf draw nodes, not containers */
        if (bounding_box_node->cls->category != NGLI_NODE_CATEGORY_DRAW)
            continue;

        const struct ngli_node2d_info *node2d_info = bounding_box_node->priv_data;

        /* Broadphase: pixel-space AABB */
        if (!ngli_aabb_intersect_point(&node2d_info->screen_aabb, pixel_point))
            continue;

        /* Narrow phase: inverse-transform point into local space */
        struct ngli_mat4 inv;
        ngli_mat4_inverse(inv.m, node2d_info->transform_matrix.m);
        NGLI_ALIGNED_VEC(local_point);
        ngli_mat4_mul_vec4(local_point, inv.m, pixel_point);

        if (ngli_aabb_intersect_point(&node2d_info->aabb, local_point)) {
            if (ngli_darray_push(&s->intersecting_nodes, bounding_box_node) < 0)
                return NGL_ERROR_MEMORY;
        }
    }

    *nodesp = s->intersecting_nodes.data;
    *nb_nodesp = s->intersecting_nodes.count;

    return 0;
}

struct ngpu_ctx *ngl_get_gpu_ctx(struct ngl_ctx *s)
{
    if (!s->configured)
        return NULL;
    return s->gpu_ctx;
}

struct ngl_node *ngl_get_scene_root(struct ngl_ctx *s)
{
    if (!s->configured || !s->scene)
        return NULL;
    return s->scene->params.root;
}

int ngl_gl_wrap_framebuffer(struct ngl_ctx *s, uint32_t framebuffer)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before wrapping a new external OpenGL framebuffer");
        return NGL_ERROR_INVALID_USAGE;
    }

    if (!s->api_impl->gl_wrap_framebuffer) {
        LOG(ERROR, "wrapping external OpenGL framebuffer is not supported by context");
        return NGL_ERROR_UNSUPPORTED;
    }

    int ret = s->api_impl->gl_wrap_framebuffer(s, framebuffer);
    if (ret < 0) {
        s->configured = 0;
        return ret;
    }
    return 0;
 }

void ngl_freep(struct ngl_ctx **ss)
{
    struct ngl_ctx *s = *ss;

    if (!s)
        return;

    if (s->frame_slots) {
        for (uint32_t i = 0; i < s->nb_frame_slots; i++) {
            if (s->frame_slots[i].frame) {
                LOG(WARNING, "freeing context with an outstanding ngl_frame; releasing implicitly");
                break;
            }
        }
    }

    if (s->configured) {
        s->api_impl->reset(s, NGLI_ACTION_UNREF_SCENE);
        s->configured = 0;
    }

    ngli_queue_destroy(&s->background_queue);
    pthread_mutex_destroy(&s->frame_slots_lock);

    ngli_darray_reset(&s->modelview_matrix_stack);
    ngli_darray_reset(&s->projection_matrix_stack);
    ngli_darray_reset(&s->transform_2d_stack);
    ngli_darray_reset(&s->opacity_2d_stack);
    ngli_darray_reset(&s->activitycheck_nodes);
    ngli_darray_reset(&s->bounding_box_nodes);
    ngli_darray_reset(&s->intersecting_nodes);
    ngli_freep(ss);
}
