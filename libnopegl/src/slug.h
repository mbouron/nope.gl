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

#ifndef SLUG_H
#define SLUG_H

#include <stddef.h>
#include <stdint.h>

struct ngl_ctx;
struct ngpu_texture;
struct path;
struct slug;

#define NGLI_SLUG_FLAG_PATH_AUTO_CLOSE (1 << 0)

struct slug_glyph_data {
    float em_bounds[4];        /* em-space bounding box: x0, y0, x1, y1 */
    float band_transform[4];   /* scale_x, scale_y, offset_x, offset_y */
    int32_t glyph_loc[2];     /* position in band texture */
    int32_t band_max[2];      /* max h-band index, max v-band index */
};

struct slug *ngli_slug_create(struct ngl_ctx *ctx);
int ngli_slug_init(struct slug *s);
int ngli_slug_prefetch(struct slug *s);
void ngli_slug_release(struct slug *s);
/* Returns glyph index (>=0) on success, or a negative error code.
 * Full glyph data is available after ngli_slug_finalize() via ngli_slug_get_glyph_data(). */
int ngli_slug_add_glyph(struct slug *s, const struct path *path, uint32_t flags,
                        float glyph_w, float glyph_h);
int ngli_slug_finalize(struct slug *s);
size_t ngli_slug_get_glyph_count(const struct slug *s);
void ngli_slug_get_glyph_data(const struct slug *s, int32_t index, struct slug_glyph_data *out);
struct ngpu_texture *ngli_slug_get_curve_texture(const struct slug *s);
struct ngpu_texture *ngli_slug_get_band_texture(const struct slug *s);
void ngli_slug_freep(struct slug **sp);

#endif
