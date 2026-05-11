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

#ifndef VIEWER_HOOKS_H
#define VIEWER_HOOKS_H

#include <stddef.h>
#include <stdint.h>

struct ngl_scene;

struct hooks_session {
    char *id;
    char *description;
};

struct hooks_ctx;

struct hooks_ctx *hooks_create(const char *script_path);

int hooks_get_sessions(struct hooks_ctx *s, struct hooks_session **sessionsp, size_t *nb_sessionsp);
void hooks_free_sessions(struct hooks_session *sessions, size_t nb_sessions);

int hooks_scene_change(struct hooks_ctx *s, const char *session_id,
                       struct ngl_scene *scene, uint32_t clear_color, int samples);

void hooks_freep(struct hooks_ctx **sp);

#endif /* VIEWER_HOOKS_H */
