/*
 * Copyright 2025-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <string.h>

#include <SDL3/SDL.h>

#include "viewer_blit.h"
#include "nuklear_ngpu.h"
#include "viewer.h"
#include "viewer_compositor.h"

int viewer_compositor_init(struct viewer_ctx *s,
                           struct ngpu_ctx *shared_gpu_ctx,
                           uintptr_t wsi_window)
{
    struct ngpu_ctx_params gpu_ctx_params;
    ngpu_ctx_params_init_from_shared_ctx(&gpu_ctx_params, shared_gpu_ctx);
    gpu_ctx_params.window         = wsi_window;
    gpu_ctx_params.swap_interval  = 1;
    gpu_ctx_params.width          = (uint32_t)s->win_width;
    gpu_ctx_params.height         = (uint32_t)s->win_height;
    memcpy(gpu_ctx_params.clear_color, s->ngl_cfg.clear_color,
           sizeof(gpu_ctx_params.clear_color));

    s->gpu_ctx = ngpu_ctx_create(&gpu_ctx_params);
    if (!s->gpu_ctx) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create GUI context");
        return -1;
    }
    int ret = ngpu_ctx_init(s->gpu_ctx);
    if (ret < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize GUI context");
        return ret;
    }

    s->nk_ngpu_ctx = nk_ngpu_create(s->gpu_ctx);
    if (!s->nk_ngpu_ctx)
        return -1;

    const float ui_scale = viewer_ui_scale(s);

    const char *font_path = NULL;
#ifdef NGL_TOOLS_DATADIR
    static const char bundled_font[] = NGL_TOOLS_DATADIR "/fonts/Inter-Regular.ttf";
    SDL_PathInfo info;
    if (SDL_GetPathInfo(bundled_font, &info))
        font_path = bundled_font;
#endif

    ret = nk_ngpu_init(s->nk_ngpu_ctx, 18.0f * ui_scale, font_path);
    if (ret < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize Nuklear backend");
        return ret;
    }

    s->blit_ctx = viewer_blit_create(s->gpu_ctx);
    if (!s->blit_ctx) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize blit");
        return -1;
    }
    return 0;
}

void viewer_compositor_close(struct viewer_ctx *s)
{
    viewer_blit_freep(&s->blit_ctx);
    nk_ngpu_freep(&s->nk_ngpu_ctx);
    ngpu_ctx_freep(&s->gpu_ctx);
}

void viewer_compositor_render(struct viewer_ctx *s)
{
    nk_ngpu_prepare(s->nk_ngpu_ctx, NK_ANTI_ALIASING_ON,
                    (uint32_t)s->win_width, (uint32_t)s->win_height);

    SDL_LockMutex(s->frame_lock);
    struct ngl_frame *new_frame = s->latest_frame;
    s->latest_frame = NULL;
    SDL_UnlockMutex(s->frame_lock);

    if (new_frame) {
        if (s->current_frame) {
            ngl_frame_release(s->current_frame, s->current_blit_done);
            s->current_blit_done = NULL;
        }
        s->current_frame = new_frame;
    }

    struct ngpu_fence *scene_fence = s->current_frame ? ngl_frame_get_signal_fence(s->current_frame) : NULL;
    struct ngpu_texture *scene_tex = s->current_frame ? ngl_frame_get_texture(s->current_frame)      : NULL;

    ngpu_ctx_begin_draw(s->gpu_ctx);
    struct ngpu_rendertarget *rt = ngpu_ctx_get_default_rendertarget(s->gpu_ctx);
    ngpu_ctx_begin_render_pass(s->gpu_ctx, rt);

    nk_ngpu_render(s->nk_ngpu_ctx, (uint32_t)s->win_width, (uint32_t)s->win_height);

    if (s->scene_loaded && scene_tex && s->preview_w > 0.0f && s->preview_h > 0.0f) {
        /* Vulkan's viewport Y is top-down; GL's is bottom-up. */
        const int vk = ngpu_ctx_get_backend_type(s->gpu_ctx) == NGPU_BACKEND_VULKAN;
        const float vp_y = vk ? s->preview_y
                              : ((float)s->win_height - s->preview_y - s->preview_h);
        viewer_blit_draw(s->blit_ctx, scene_tex, s->preview_x, vp_y, s->preview_w, s->preview_h);
    }

    ngpu_ctx_end_render_pass(s->gpu_ctx);

    struct ngpu_fence *blit_done = NULL;
    ngpu_ctx_end_draw(s->gpu_ctx, viewer_get_frame_time(s), scene_fence,
                      s->current_frame ? &blit_done : NULL);

    /* Keep the latest blit_done fence: queue ordering guarantees the new
     * one supersedes the previous one. */
    if (blit_done) {
        if (s->current_blit_done)
            ngpu_fence_freep(&s->current_blit_done);
        s->current_blit_done = blit_done;
    }
}
