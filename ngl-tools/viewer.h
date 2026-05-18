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

#ifndef VIEWER_H
#define VIEWER_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL.h>
#include <nopegl/nopegl.h>
#include <ngpu/ngpu.h>

#include "viewer_blit.h"
#include "viewer_export.h"
#include "viewer_hooks.h"
#include "viewer_scene.h"

struct nk_ngpu_ctx;

#define VIEWER_PANEL_RATIO_MIN 0.3f
#define VIEWER_PANEL_RATIO_MAX 0.7f

struct viewer_ctx {
    int log_level;
    struct ngl_config ngl_cfg;

    /* Window — dimensions are in physical pixels (SDL_WINDOW_HIGH_PIXEL_DENSITY). */
    SDL_Window *window;
    int32_t win_width;
    int32_t win_height;
    /* East-west resize cursor shown when hovering the splitter handle. */
    SDL_Cursor *cursor_ew_resize;

    /*
     * Two-context architecture:
     * - ngl renders the scene offscreen on its worker thread
     * - GUI renders on the main thread, attached to the SDL window
     *
     * Cross-context sync, both directions:
     * - Producer -> consumer: each ngl_draw produces an ngl_frame whose
     *   signal_fence is signaled when the scene texture is ready. The
     *   GUI ngpu_ctx takes that fence as a wait fence before using the
     *   scene texture.
     * - Consumer -> producer: GUI ngpu_ctx also produces a fence
     *   that signals when the GUI blit operation completes, which we hand
     *   to ngl_frame_release as consumer_done. ngl waits on it before
     *   reusing the slot's texture, so the next scene draw can't race
     *   with the still-running blit operation.
     */
    struct ngl_ctx *ngl_ctx;
    struct ngpu_ctx *gpu_ctx;

    /*
     * Frame published by ngl but the main/UI thread hasn't picked up yet.
     */
    SDL_Mutex *frame_lock;
    struct ngl_frame *latest_frame;

    /*
     * `current_frame` is the frame held on the main/UI thread between producer
     * publishes. It is re-blit every UI frame until a new frame is
     * produced/picked up.
     *
     * `current_blit_done` is the most recent GUI-side completion fence; on
     * each UI frame submission we replace it with a new fence. It is
     * ultimately used when we release the `current_frame` back to ngl.
     */
    struct ngl_frame *current_frame;
    struct ngpu_fence *current_blit_done;

    /* Scene thread. */
    SDL_Thread *scene_thread;
    struct scene_cmd_queue cmd_q;

    /* Nuklear + blit contexts. */
    struct nk_ngpu_ctx *nk_ngpu_ctx;
    struct blit_ctx *blit_ctx;
    /*
     * Width of the controls panel as a fraction of win_width. Live-adjusted
     * by the splitter drag in viewer_ui; clamped to [VIEWER_PANEL_RATIO_MIN,
     * VIEWER_PANEL_RATIO_MAX] there too. The minimum doubles as the initial
     * default so the panel never shrinks below its original size.
     */
    float panel_ratio;
    float dpi_scale;

    /* Preview panel bounds (in logical coordinates, set by viewer_ui). */
    float preview_x, preview_y, preview_w, preview_h;

    /* Python scene management. */
    Sint64 script_mtime;
    char script_path[2048];
    char **scene_names;
    size_t nb_scenes;
    int selected_scene;

    /*
     * Main thread playback cache. Populated by the SDL scene-loaded event
     * handler. Never read or written from the scene thread.
     */
    int     scene_loaded;
    double  duration;
    int32_t framerate[2];

    int64_t clock_off;
    int paused;

    /*
     * Playback time in seconds. Shared between the scene and main threads.
     * Must be accessed through viewer_{get,set}_frame_time().
     */
    SDL_Mutex *frame_time_lock;
    double frame_time;

    /*
     * Custom SDL event type registered at startup. Carries struct
     * scene_load_result via user.data1 (consumer frees it).
     */
    Uint32 scene_loaded_event;

    /* Export. */
    struct export_ctx *exporter;
    int export_profile;
    int export_height;
    char export_path[1024];
    int avail_profiles[16];
    int nb_avail_profiles;

    /* Last error captured from viewer_python_*() calls. */
    char last_error[4096];

    /* Hooks. */
    struct hooks_ctx *hooks;
    const char *hooks_script_path;
    struct hooks_session *hooks_sessions;
    size_t nb_hooks_sessions;
    int *hooks_session_enabled; /* Per-session enable flag. */

    /* Nuklear-SDL input bridge state. Per-context (rather than file-scope
     * statics) so a second viewer instance wouldn't collide. */
    uint64_t nk_last_left_click_ts;
    int      nk_edit_was_active;

    /* Splitter drag state. Set while the user is actively dragging the
     * controls/preview divider; reset when the left mouse button releases. */
    int splitter_dragging;

    /* Reference to currently-selected node in the Scene Graph tree. */
    struct ngl_node *selected_node;
};

/*
 * Integer UI scale derived from the display DPI. Used for font bake and layout
 * so every row/margin lands on a pixel boundary.
 */
static inline float viewer_ui_scale(const struct viewer_ctx *s)
{
    const float dpi = s->dpi_scale > 0.0f ? s->dpi_scale : 1.0f;
    const float r = roundf(dpi);
    return r >= 1.0f ? r : 1.0f;
}

static inline double viewer_get_frame_time(struct viewer_ctx *s)
{
    SDL_LockMutex(s->frame_time_lock);
    const double t = s->frame_time;
    SDL_UnlockMutex(s->frame_time_lock);
    return t;
}

static inline void viewer_set_frame_time(struct viewer_ctx *s, double t)
{
    SDL_LockMutex(s->frame_time_lock);
    s->frame_time = t;
    SDL_UnlockMutex(s->frame_time_lock);
}

#endif
