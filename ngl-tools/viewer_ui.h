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

#ifndef VIEWER_UI_H
#define VIEWER_UI_H

#include <SDL3/SDL.h>

struct viewer_ctx;

/* Render the Nuklear control + preview panels for one frame. */
void viewer_ui(struct viewer_ctx *s);

/* Translate one SDL event into Nuklear input. SDL event coords are logical,
 * Nuklear is fed physical — uses s->dpi_scale to convert. */
void nk_sdl_input(struct viewer_ctx *s, SDL_Event *evt);

/* Advance the playback clock based on wall time, looping at duration. */
void viewer_update_time(struct viewer_ctx *s);

/* Rebuild the hooks session list. Preserves per-session disabled flags
 * across the refresh by matching session IDs. No-op if s->hooks is NULL. */
void viewer_hooks_refresh(struct viewer_ctx *s);

/* Release all hooks-related state: the session array, the per-session
 * enable flags, and the hooks context itself (GIL-guarded since
 * hooks_freep DECREFs Python objects). Safe on partial state. */
void viewer_hooks_close(struct viewer_ctx *s);

/* Synchronously request a scene snapshot from the rendering thread,
 * serialize it, and push it to every enabled hooks session. No-op if no
 * hooks are configured or no session is enabled. Blocks the caller until
 * the rendering thread has produced the duplicate (at most one ngl_draw
 * iteration). */
void viewer_hooks_send_scene(struct viewer_ctx *s);

#endif
