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

#ifndef NUKLEAR_NGPU_H
#define NUKLEAR_NGPU_H

#include <ngpu/ngpu.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
/* Needed for nk_font_atlas_add_from_file (used to load the bundled TTF). */
#define NK_INCLUDE_STANDARD_IO

/*
 * Silence Nuklear's conversion warnings.
 */
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wconversion"
# pragma GCC diagnostic ignored "-Wfloat-conversion"
# pragma GCC diagnostic ignored "-Wsign-conversion"
#elif defined(_MSC_VER)
# pragma warning(push)
# pragma warning(disable: 5287)
#endif

#include "nuklear.h"

#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#elif defined(_MSC_VER)
# pragma warning(pop)
#endif

struct nk_ngpu_ctx;

struct nk_ngpu_ctx *nk_ngpu_create(struct ngpu_ctx *gpu_ctx);

/*
 * Initialize the Nuklear NGPU context.
 *
 * font_size is in physical pixels. For HiDPI displays, callers should pass
 * the logical font size multiplied by the display scale.
 */
int nk_ngpu_init(struct nk_ngpu_ctx *s, float font_size, const char *font_path);

/*
 * Get the underlying Nuklear context.
 */
struct nk_context *nk_ngpu_get_nk_ctx(struct nk_ngpu_ctx *s);

/*
 * Convert Nuklear draw commands to vertex/index buffers and upload to GPU.
 */
void nk_ngpu_prepare(struct nk_ngpu_ctx *s, enum nk_anti_aliasing aa,
                     uint32_t width, uint32_t height);

/*
 * Render Nuklear draw commands into the currently active render pass.
 */
void nk_ngpu_render(struct nk_ngpu_ctx *s, uint32_t width, uint32_t height);

void nk_ngpu_freep(struct nk_ngpu_ctx **sp);

#endif /* NUKLEAR_NGPU_H */
