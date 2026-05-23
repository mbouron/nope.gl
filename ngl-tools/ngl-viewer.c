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

#include <Python.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "nuklear_ngpu.h"
#include "opts.h"
#include "viewer.h"
#include "viewer_compositor.h"
#include "viewer_config.h"
#include "viewer_python.h"
#include "viewer_ui.h"
#include "wsi.h"

#define OFFSET(x) offsetof(struct viewer_ctx, x)
static const struct opt options[] = {
    {"-l", "--loglevel",      OPT_TYPE_LOGLEVEL, .offset=OFFSET(log_level)},
    {"-b", "--backend",       OPT_TYPE_BACKEND,  .offset=OFFSET(ngl_cfg.backend)},
    {"-s", "--size",          OPT_TYPE_RATIONAL, .offset=OFFSET(ngl_cfg.width)},
    {"-k", "--hooks-script",  OPT_TYPE_STR,      .offset=OFFSET(hooks_script_path)},
};

/* Map libnopegl's level enum to SDL's priority enum so -l drives both. */
static SDL_LogPriority sdl_priority_from_ngl_level(int level)
{
    switch (level) {
    case NGL_LOG_VERBOSE: return SDL_LOG_PRIORITY_VERBOSE;
    case NGL_LOG_DEBUG:   return SDL_LOG_PRIORITY_DEBUG;
    case NGL_LOG_INFO:    return SDL_LOG_PRIORITY_INFO;
    case NGL_LOG_WARNING: return SDL_LOG_PRIORITY_WARN;
    case NGL_LOG_ERROR:   return SDL_LOG_PRIORITY_ERROR;
    default:              return SDL_LOG_PRIORITY_INFO;
    }
}

int main(int argc, char *argv[])
{
    struct viewer_ctx s = {
        .log_level             = NGL_LOG_INFO,
        .ngl_cfg.offscreen     = 1,
        .ngl_cfg.width         = 1280,
        .ngl_cfg.height        = 800,
        .ngl_cfg.swap_interval = -1,
        .ngl_cfg.clear_color   = {0.15f, 0.15f, 0.15f, 1.0f},
        .panel_ratio           = VIEWER_PANEL_RATIO_MIN,
        .framerate             = {60, 1},
        .selected_scene        = -1,
        .clock_off_ns          = -1,
        .export_height         = 720,
    };

    int ret = opts_parse(argc, argc, argv, options, ARRAY_NB(options), &s);
    if (ret < 0 || ret == OPT_HELP) {
        opts_print_usage(argv[0], options, ARRAY_NB(options), NULL);
        return ret == OPT_HELP ? 0 : EXIT_FAILURE;
    }

    ngl_log_set_min_level(s.log_level);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, sdl_priority_from_ngl_level(s.log_level));

    /* Filter export profiles. */
    for (int i = 0; i < nb_export_profiles && s.nb_avail_profiles < (int)ARRAY_NB(s.avail_profiles); i++) {
        if (export_is_profile_available(i))
            s.avail_profiles[s.nb_avail_profiles++] = i;
    }
    s.export_profile = s.nb_avail_profiles > 0 ? s.avail_profiles[0] : -1;

    ret = viewer_python_init();
    if (ret < 0)
        return EXIT_FAILURE;

    struct config_data cfg;
    config_load(&cfg);
    if (!s.script_path[0] && cfg.script_path[0])
        snprintf(s.script_path, sizeof(s.script_path), "%s", cfg.script_path);
    if (cfg.panel_ratio > 0.0f)
        s.panel_ratio = NK_CLAMP(VIEWER_PANEL_RATIO_MIN, cfg.panel_ratio, VIEWER_PANEL_RATIO_MAX);

    if (s.script_path[0])
        viewer_python_list_scenes(s.script_path, &s.scene_names, &s.nb_scenes,
                                  s.last_error, sizeof(s.last_error));

    if (cfg.scene_name[0]) {
        for (size_t i = 0; i < s.nb_scenes; i++) {
            if (!strcmp(s.scene_names[i], cfg.scene_name)) {
                s.selected_scene = (int)i;
                break;
            }
        }
    }
    /* No remembered scene (or it no longer exists in the script) — default to
     * the first one so the viewer comes up showing something. */
    if (s.selected_scene < 0 && s.nb_scenes > 0)
        s.selected_scene = 0;

    /* Load hooks script if specified. */
    if (s.hooks_script_path) {
        s.hooks = hooks_create(s.hooks_script_path);
        if (!s.hooks)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "failed to load hooks script '%s'", s.hooks_script_path);
    }

    /* Release the GIL so the scene thread can acquire it for Python calls.
     * Main thread must re-acquire before any Python call. */
    PyThreadState *py_tstate = PyEval_SaveThread();

    if (wsi_init() < 0) {
        PyEval_RestoreThread(py_tstate);
        viewer_python_uninit();
        return EXIT_FAILURE;
    }

    s.window = wsi_get_window("ngl-viewer", s.ngl_cfg.width, s.ngl_cfg.height,
                              WSI_WINDOW_FLAG_HIGH_PIXEL_DENSITY);
    if (!s.window) {
        SDL_Quit();
        PyEval_RestoreThread(py_tstate);
        viewer_python_uninit();
        return EXIT_FAILURE;
    }

    SDL_SetWindowMinimumSize(s.window, 640, 480);

    /* Resize cursor for the splitter; failure is non-fatal (we just keep the
     * default cursor). Freed in cleanup with SDL_DestroyCursor. */
    s.cursor_ew_resize = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);

    s.scene_loaded_event = SDL_RegisterEvents(1);
    if (s.scene_loaded_event == (Uint32)-1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to register custom SDL event");
        SDL_Quit();
        PyEval_RestoreThread(py_tstate);
        viewer_python_uninit();
        return EXIT_FAILURE;
    }

    SDL_GetWindowSizeInPixels(s.window, &s.win_width, &s.win_height);

    s.dpi_scale = SDL_GetWindowDisplayScale(s.window);
    if (s.dpi_scale <= 0.0f)
        s.dpi_scale = 1.0f;

    /*
     * On Wayland, the pixel size isn't known until the compositor sends its
     * first xdg_toplevel configure, which arrives after we return here. Until
     * the event arrives (SDL_EVENT_{WINDOW_RESIZED, WINDOW_PIXEL_SIZE_CHANGED}),
     * fall back to the requested logical size scaled by the display DPI.
     */
    if (s.win_width <= 1 || s.win_height <= 1) {
        s.win_width  = (int32_t)((float)s.ngl_cfg.width  * s.dpi_scale);
        s.win_height = (int32_t)((float)s.ngl_cfg.height * s.dpi_scale);
    }

    /* Seed the scene-side render size from the window size minus the
     * controls panel. The actual size tracks the preview panel bounds
     * once Nuklear publishes them; this is just a sensible initial value. */
    const uint32_t init_scene_w = (uint32_t)((float)s.win_width * (1.0f - s.panel_ratio));
    const uint32_t init_scene_h = (uint32_t)s.win_height;

    /* Get window system info for the GUI context. */
    struct ngl_config wsi_cfg = {
        .backend = s.ngl_cfg.backend
    };
    ret = wsi_set_ngl_config(&wsi_cfg, s.window);
    if (ret < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get window system info");
        goto end;
    }

    /* Inherit platform/display from WSI. Backend stays whatever the user
     * (or default) selected — wsi_set_ngl_config doesn't fill it in. */
    s.ngl_cfg.platform = wsi_cfg.platform;
    s.ngl_cfg.display  = wsi_cfg.display;

    ret = viewer_scene_init(&s, init_scene_w, init_scene_h);
    if (ret < 0) {
        ret = EXIT_FAILURE;
        goto end;
    }

    if (s.selected_scene >= 0)
        viewer_request_load(&s);

    struct ngpu_ctx *scene_gpu_ctx = ngl_get_gpu_ctx(s.ngl_ctx);
    if (!scene_gpu_ctx) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get scene GPU context");
        ret = EXIT_FAILURE;
        goto end;
    }

    ret = viewer_compositor_init(&s, scene_gpu_ctx, wsi_cfg.window);
    if (ret < 0) {
        ret = EXIT_FAILURE;
        goto end;
    }

    struct nk_context *nk = nk_ngpu_get_nk_ctx(s.nk_ngpu_ctx);

    bool run = true;
    Uint64 last_mtime_check = SDL_GetTicks();
    Uint64 last_hooks_refresh = SDL_GetTicks() - 3000; /* Fire on first iter. */
    uint32_t last_resize_w = 0, last_resize_h = 0;
    double last_render_t = -1.0;
    while (run) {
        /* Nuklear's event handling. */
        nk_input_begin(nk);
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            /* Load result from the scene thread. */
            if (event.type == s.scene_loaded_event) {
                struct scene_load_result *r = event.user.data1;
                if (r) {
                    if (r->success) {
                        /* Preserve play/pause state and clamp current time to
                         * the new scene duration so the viewer resumes where
                         * it was rather than snapping back to 0. */
                        const int    prev_paused = s.scene_loaded ? s.paused : 0;
                        const double prev_time   = s.scene_loaded ? viewer_get_frame_time(&s) : 0.0;
                        const double resume_time = prev_time < r->duration ? prev_time : r->duration;

                        s.scene_loaded = 1;
                        s.duration     = r->duration;
                        s.framerate[0] = r->framerate[0];
                        s.framerate[1] = r->framerate[1];
                        s.paused       = prev_paused;
                        last_render_t  = -1.0;
                        viewer_set_frame_time(&s, resume_time);
                        /* Re-anchor the clock so playback resumes from
                         * resume_time rather than rewinding to 0. */
                        s.clock_off_ns = prev_paused ? -1
                            : (int64_t)viewer_now_ns(&s) - (int64_t)(resume_time * 1.0e9);
                        s.last_error[0] = '\0';
                        /* Selection holds a ref to a node from the previous
                         * scene; the new scene has an entirely fresh tree. */
                        if (s.selected_node)
                            ngl_node_unrefp(&s.selected_node);

                        /* Push the freshly-loaded scene to any enabled hooks
                         * sessions. Uses the same snapshot mechanism as export
                         * — see viewer_hooks_send_scene. */
                        viewer_hooks_send_scene(&s);
                    } else {
                        snprintf(s.last_error, sizeof(s.last_error), "%s",
                                 r->error ? r->error : "(load failed)");
                    }
                    scene_load_result_freep(&r);
                }
                continue;
            }

            switch (event.type) {
            case SDL_EVENT_QUIT:
                run = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                run = false;
                break;
            case SDL_EVENT_DROP_FILE:
                viewer_load_script(&s, event.drop.data);
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
                /* Update window dimensions. */
                SDL_GetWindowSizeInPixels(s.window, &s.win_width, &s.win_height);
                /* Resize swapchain if needed. */
                uint32_t cur_w = 0, cur_h = 0;
                ngpu_ctx_get_default_rendertarget_size(s.gpu_ctx, &cur_w, &cur_h);
                if (cur_w != (uint32_t)s.win_width || cur_h != (uint32_t)s.win_height)
                    ngpu_ctx_resize(s.gpu_ctx, (uint32_t)s.win_width, (uint32_t)s.win_height);
                break;
            }
            case SDL_EVENT_KEY_DOWN:
                /* ESC/Q: exit. */
                if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
                    run = false;
                    continue;
                }
                /* SPACE: pause. */
                if (event.key.key == SDLK_SPACE) {
                    s.paused ^= 1;
                    if (!s.paused)
                        s.clock_off_ns = (int64_t)viewer_now_ns(&s) - (int64_t)(viewer_get_frame_time(&s) * 1.0e9);
                    continue;
                }
                /* LEFT/RIGHT: ±1 second seek. */
                if ((event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT) &&
                    s.scene_loaded) {
                    const double seek_step = 1.0;
                    double t = viewer_get_frame_time(&s);
                    t += (event.key.key == SDLK_LEFT) ? -seek_step : seek_step;
                    if (t < 0.0)
                        t = 0.0;
                    else if (t > s.duration)
                        t = s.duration;
                    viewer_set_frame_time(&s, t);
                    s.clock_off_ns = (int64_t)viewer_now_ns(&s) - (int64_t)(t * 1.0e9);
                    continue;
                }
                /* O/P: ±1 frame step. */
                if ((event.key.key == SDLK_O || event.key.key == SDLK_P) &&
                    s.scene_loaded && s.framerate[0]) {
                    s.paused = 1;
                    const double step = (double)s.framerate[1] / (double)s.framerate[0];
                    double t = viewer_get_frame_time(&s);
                    t += (event.key.key == SDLK_O) ? -step : step;
                    if (t < 0.0)
                        t = 0.0;
                    else if (t > s.duration)
                        t = s.duration;
                    viewer_set_frame_time(&s, t);
                    continue;
                }
                SDL_FALLTHROUGH;
            default:
                nk_sdl_input(&s, &event);
                break;
            }
        }
        nk_input_end(nk);

        viewer_update_time(&s);
        viewer_ui(&s);

        /* Watch the loaded script's mtime every 500ms and reload on change. */
        const Uint64 now = SDL_GetTicks();
        if (s.selected_scene >= 0 && s.script_path[0] && now - last_mtime_check > 500) {
            last_mtime_check = now;
            const Sint64 mtime = viewer_scene_get_file_mtime(s.script_path);
            if (mtime && mtime != s.script_mtime)
                viewer_request_load(&s);
        }

        /* Auto-refresh the hooks session list every 2s. */
        if (s.hooks && now - last_hooks_refresh > 2000) {
            last_hooks_refresh = now;
            viewer_hooks_refresh(&s);
        }

        /* Update scene resolution to match preview panel. Only repost when
         * the size actually changed — otherwise we'd take the queue lock
         * every frame just to coalesce a duplicate. */
        if (s.preview_w > 1 && s.preview_h > 1) {
            const uint32_t new_w = (uint32_t)s.preview_w;
            const uint32_t new_h = (uint32_t)s.preview_h;
            if (new_w >= 2 && new_h >= 2 && (new_w != last_resize_w || new_h != last_resize_h)) {
                last_resize_w = new_w;
                last_resize_h = new_h;
                scene_cmd_post(&s.cmd_q, (struct scene_cmd){
                    .type   = SCENE_CMD_RESIZE,
                    .resize = {.width = new_w, .height = new_h},
                });
            }
        }

        /* Drive the scene from the UI: post one render request per UI
         * iteration with the predicted-vsync-aligned target time. Posted after
         * RESIZE so the resize is processed first if both are queued. */
        if (s.scene_loaded) {
            const double t = viewer_get_frame_time(&s);
            if (t != last_render_t) {
                scene_cmd_post(&s.cmd_q, (struct scene_cmd){
                    .type   = SCENE_CMD_RENDER,
                    .render = {.target_time = t},
                });
                last_render_t = t;
            }
        }

        viewer_compositor_render(&s);
    }

end:
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.script_path, sizeof(cfg.script_path), "%s", s.script_path);
    if (s.selected_scene >= 0 && (size_t)s.selected_scene < s.nb_scenes)
        snprintf(cfg.scene_name, sizeof(cfg.scene_name), "%s", s.scene_names[s.selected_scene]);
    cfg.panel_ratio = s.panel_ratio;
    config_save(&cfg);

    /* Stop the export worker. */
    export_freep(&s.exporter);

    /* Release the selected-node ref before tearing the scene down */
    if (s.selected_node)
        ngl_node_unrefp(&s.selected_node);

    /* Tear the scene renderer down first as it may hold fences from the UI compositor. */
    viewer_scene_close(&s);
    viewer_hooks_close(&s);
    viewer_python_free_scenes(s.scene_names, s.nb_scenes);
    viewer_compositor_close(&s);

    if (s.cursor_ew_resize)
        SDL_DestroyCursor(s.cursor_ew_resize);
    if (s.window)
        SDL_DestroyWindow(s.window);
    SDL_Quit();
    PyEval_RestoreThread(py_tstate);
    viewer_python_uninit();

    return ret;
}
