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

#ifndef VIEWER_BLIT_H
#define VIEWER_BLIT_H

#include <ngpu/ngpu.h>

struct blit_ctx;

struct blit_ctx *viewer_blit_create(struct ngpu_ctx *gpu_ctx);

void viewer_blit_draw(struct blit_ctx *s, struct ngpu_texture *tex,
                      float x, float y, float w, float h);

void viewer_blit_freep(struct blit_ctx **sp);

#endif /* VIEWER_BLIT_H */
