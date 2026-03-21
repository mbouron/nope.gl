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

#include <math.h>
#include <string.h>

#include <ngpu/ngpu.h>

#include "internal.h"
#include "path.h"
#include "slug.h"
#include "utils/cubic_to_quad.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

#define BAND_TEX_WIDTH 4096
#define LOG_BAND_TEX_WIDTH 12
#define CUBIC_TO_QUAD_TOLERANCE 0.01f
#define MIN_TEX_HEIGHT 64

struct curve {
    float p1[2]; /* start point */
    float p2[2]; /* control point */
    float p3[2]; /* end point */
};

struct glyph_entry {
    struct slug_glyph_data data;
    struct darray curves; /* struct curve */
};

struct slug {
    struct ngl_ctx *ctx;
    struct darray glyphs; /* struct glyph_entry */

    /* Packed texture data (built during finalize) */
    struct darray curve_texels; /* float[4] per texel */
    struct darray band_texels;  /* float[4] per texel */
    int32_t curve_texture_height;
    int32_t band_texture_height;

    /* Pre-allocated GPU textures with capacity tracking (in rows) */
    struct ngpu_texture *curve_texture;
    struct ngpu_texture *band_texture;
    int32_t curve_texture_capacity;
    int32_t band_texture_capacity;
};

struct slug *ngli_slug_create(struct ngl_ctx *ctx)
{
    struct slug *s = ngli_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->ctx = ctx;
    return s;
}

static int ensure_texture_capacity(struct slug *s, struct ngpu_texture **tex_p, int32_t *capacity, int32_t needed_height)
{
    if (*capacity >= needed_height)
        return 0;

    int32_t new_height = *capacity > 0 ? *capacity : MIN_TEX_HEIGHT;
    while (new_height < needed_height)
        new_height *= 2;

    ngpu_texture_freep(tex_p);

    struct ngpu_ctx *gpu_ctx = s->ctx->gpu_ctx;
    *tex_p = ngpu_texture_create(gpu_ctx);
    if (!*tex_p)
        return NGL_ERROR_MEMORY;

    const struct ngpu_texture_params tex_params = {
        .type       = NGPU_TEXTURE_TYPE_2D,
        .width      = BAND_TEX_WIDTH,
        .height     = (uint32_t)new_height,
        .format     = NGPU_FORMAT_R32G32B32A32_SFLOAT,
        .min_filter = NGPU_FILTER_NEAREST,
        .mag_filter = NGPU_FILTER_NEAREST,
        .usage      = NGPU_TEXTURE_USAGE_TRANSFER_DST_BIT | NGPU_TEXTURE_USAGE_SAMPLED_BIT,
    };

    int ret = ngpu_texture_init(*tex_p, &tex_params);
    if (ret < 0)
        return ret;

    *capacity = new_height;
    return 0;
}

int ngli_slug_init(struct slug *s)
{
    ngli_darray_init(&s->glyphs, sizeof(struct glyph_entry), 0);
    ngli_darray_init(&s->curve_texels, 4 * sizeof(float), 0);
    ngli_darray_init(&s->band_texels, 4 * sizeof(float), 0);
    return 0;
}

int ngli_slug_prefetch(struct slug *s)
{
    int ret = ensure_texture_capacity(s, &s->curve_texture, &s->curve_texture_capacity, MIN_TEX_HEIGHT);
    if (ret < 0)
        return ret;

    ret = ensure_texture_capacity(s, &s->band_texture, &s->band_texture_capacity, MIN_TEX_HEIGHT);
    if (ret < 0)
        return ret;

    return 0;
}

void ngli_slug_release(struct slug *s)
{
    ngpu_texture_freep(&s->curve_texture);
    ngpu_texture_freep(&s->band_texture);
    s->curve_texture_capacity = 0;
    s->band_texture_capacity = 0;
}

/*
 * Convert path segments to quadratic bezier curves and collect them.
 */
static int collect_curves(struct slug *s, const struct path *path, uint32_t flags,
                          struct darray *curves_out, float bounds[4])
{
    const struct darray *segments = ngli_path_get_segments(path);
    const struct path_segment *segs = ngli_darray_data(segments);
    const size_t nb_segs = ngli_darray_count(segments);

    float last[2] = {0.f, 0.f};
    float subpath_start[2] = {0.f, 0.f};

    bounds[0] = bounds[1] = 1e30f;
    bounds[2] = bounds[3] = -1e30f;

    for (size_t i = 0; i < nb_segs; i++) {
        const struct path_segment *seg = &segs[i];

        if (seg->flags & NGLI_PATH_SEGMENT_FLAG_NEW_ORIGIN) {
            /* Close previous sub-path if auto-close is requested */
            if ((flags & NGLI_SLUG_FLAG_PATH_AUTO_CLOSE) && i > 0) {
                if (last[0] != subpath_start[0] || last[1] != subpath_start[1]) {
                    const struct curve closing = {
                        .p1 = {last[0], last[1]},
                        .p2 = {(last[0] + subpath_start[0]) * .5f, (last[1] + subpath_start[1]) * .5f},
                        .p3 = {subpath_start[0], subpath_start[1]},
                    };
                    if (!ngli_darray_push(curves_out, &closing))
                        return NGL_ERROR_MEMORY;
                }
            }
            subpath_start[0] = seg->bezier_x[0];
            subpath_start[1] = seg->bezier_y[0];
        }

        const float start[2] = {seg->bezier_x[0], seg->bezier_y[0]};

        if (seg->degree == 1) {
            /* Line: degenerate quadratic with midpoint as control */
            const float end[2] = {seg->bezier_x[1], seg->bezier_y[1]};
            const struct curve c = {
                .p1 = {start[0], start[1]},
                .p2 = {(start[0] + end[0]) * .5f, (start[1] + end[1]) * .5f},
                .p3 = {end[0], end[1]},
            };
            if (!ngli_darray_push(curves_out, &c))
                return NGL_ERROR_MEMORY;
            last[0] = end[0];
            last[1] = end[1];
        } else if (seg->degree == 2) {
            /* Quadratic bezier: use directly */
            const struct curve c = {
                .p1 = {seg->bezier_x[0], seg->bezier_y[0]},
                .p2 = {seg->bezier_x[1], seg->bezier_y[1]},
                .p3 = {seg->bezier_x[2], seg->bezier_y[2]},
            };
            if (!ngli_darray_push(curves_out, &c))
                return NGL_ERROR_MEMORY;
            last[0] = seg->bezier_x[2];
            last[1] = seg->bezier_y[2];
        } else if (seg->degree == 3) {
            /* Cubic bezier: convert to quadratics */
            const struct point cubic[4] = {
                {seg->bezier_x[0], seg->bezier_y[0]},
                {seg->bezier_x[1], seg->bezier_y[1]},
                {seg->bezier_x[2], seg->bezier_y[2]},
                {seg->bezier_x[3], seg->bezier_y[3]},
            };
            struct cubic_converter cvt;
            cubic_converter_to_quadratics(&cvt, cubic, CUBIC_TO_QUAD_TOLERANCE);
            const struct point *quads = cvt.quadratics;
            for (int j = 0; j < cvt.quadratic_count; j++) {
                const struct curve c = {
                    .p1 = {quads[2 * j + 0].x, quads[2 * j + 0].y},
                    .p2 = {quads[2 * j + 1].x, quads[2 * j + 1].y},
                    .p3 = {quads[2 * j + 2].x, quads[2 * j + 2].y},
                };
                if (!ngli_darray_push(curves_out, &c))
                    return NGL_ERROR_MEMORY;
            }
            last[0] = seg->bezier_x[3];
            last[1] = seg->bezier_y[3];
        }

        /* Update bounds */
        for (int d = 0; d <= seg->degree; d++) {
            bounds[0] = NGLI_MIN(bounds[0], seg->bezier_x[d]);
            bounds[1] = NGLI_MIN(bounds[1], seg->bezier_y[d]);
            bounds[2] = NGLI_MAX(bounds[2], seg->bezier_x[d]);
            bounds[3] = NGLI_MAX(bounds[3], seg->bezier_y[d]);
        }
    }

    /* Close last sub-path if auto-close */
    if ((flags & NGLI_SLUG_FLAG_PATH_AUTO_CLOSE) && nb_segs > 0) {
        if (last[0] != subpath_start[0] || last[1] != subpath_start[1]) {
            const struct curve closing = {
                .p1 = {last[0], last[1]},
                .p2 = {(last[0] + subpath_start[0]) * .5f, (last[1] + subpath_start[1]) * .5f},
                .p3 = {subpath_start[0], subpath_start[1]},
            };
            if (!ngli_darray_push(curves_out, &closing))
                return NGL_ERROR_MEMORY;
        }
    }

    return 0;
}

int ngli_slug_add_glyph(struct slug *s, const struct path *path, uint32_t flags,
                        float glyph_w, float glyph_h)
{
    struct glyph_entry entry = {0};
    ngli_darray_init(&entry.curves, sizeof(struct curve), 0);

    float bounds[4];
    int ret = collect_curves(s, path, flags, &entry.curves, bounds);
    if (ret < 0) {
        ngli_darray_reset(&entry.curves);
        return ret;
    }

    /* Use provided glyph dimensions for em bounds, falling back to path bounds */
    if (glyph_w > 0.f && glyph_h > 0.f) {
        entry.data.em_bounds[0] = 0.f;
        entry.data.em_bounds[1] = 0.f;
        entry.data.em_bounds[2] = glyph_w;
        entry.data.em_bounds[3] = glyph_h;
    } else {
        entry.data.em_bounds[0] = bounds[0];
        entry.data.em_bounds[1] = bounds[1];
        entry.data.em_bounds[2] = bounds[2];
        entry.data.em_bounds[3] = bounds[3];
    }

    if (!ngli_darray_push(&s->glyphs, &entry)) {
        ngli_darray_reset(&entry.curves);
        return NGL_ERROR_MEMORY;
    }

    /* Return glyph index; full data available after ngli_slug_finalize() via ngli_slug_get_glyph_data() */
    return (int)ngli_darray_count(&s->glyphs) - 1;
}

struct band_entry {
    int32_t curve_index;
    float sort_key; /* max-x for h-bands, max-y for v-bands */
};

static int cmp_band_entry_desc(const void *a, const void *b)
{
    const struct band_entry *ea = a;
    const struct band_entry *eb = b;
    if (ea->sort_key > eb->sort_key)
        return -1;
    if (ea->sort_key < eb->sort_key)
        return 1;
    return 0;
}

static int push_texel(struct darray *texels, float x, float y, float z, float w)
{
    const float v[4] = {x, y, z, w};
    if (!ngli_darray_push(texels, v))
        return NGL_ERROR_MEMORY;
    return 0;
}

static int finalize_internal(struct slug *s)
{
    struct glyph_entry *glyphs = ngli_darray_data(&s->glyphs);
    const size_t nb_glyphs = ngli_darray_count(&s->glyphs);

    /*
     * Track current write position in curve and band textures.
     * Positions are in texels; we use (x, y) coordinates in a BAND_TEX_WIDTH-wide texture.
     */
    int32_t curve_pos = 0; /* each curve = 2 texels */
    int32_t band_pos = 0;

    struct darray band_entries;
    ngli_darray_init(&band_entries, sizeof(struct band_entry), 0);

    for (size_t g = 0; g < nb_glyphs; g++) {
        struct glyph_entry *glyph = &glyphs[g];
        const struct curve *curves = ngli_darray_data(&glyph->curves);
        const size_t nb_curves = ngli_darray_count(&glyph->curves);

        /* Write all curves for this glyph to the curve texture */
        const int32_t glyph_curve_start = curve_pos;
        for (size_t i = 0; i < nb_curves; i++) {
            const struct curve *c = &curves[i];
            /* Texel 0: (p1.x, p1.y, p2.x, p2.y) */
            int ret = push_texel(&s->curve_texels, c->p1[0], c->p1[1], c->p2[0], c->p2[1]);
            if (ret < 0)
                goto end;
            /* Texel 1: (p3.x, p3.y, 0, 0) */
            ret = push_texel(&s->curve_texels, c->p3[0], c->p3[1], 0.f, 0.f);
            if (ret < 0)
                goto end;
            curve_pos += 2;
        }

        /* Compute band counts */
        const float bx0 = glyph->data.em_bounds[0];
        const float by0 = glyph->data.em_bounds[1];
        const float bx1 = glyph->data.em_bounds[2];
        const float by1 = glyph->data.em_bounds[3];
        const float em_w = bx1 - bx0;
        const float em_h = by1 - by0;

        int32_t nb_hbands, nb_vbands;
        if (nb_curves == 0) {
            nb_hbands = 1;
            nb_vbands = 1;
        } else {
            const int32_t n = (int32_t)ceilf(sqrtf((float)nb_curves));
            nb_hbands = NGLI_CLAMP(n, 1, 255);
            nb_vbands = NGLI_CLAMP(n, 1, 255);
        }

        /* Record glyph location in band texture */
        glyph->data.glyph_loc[0] = band_pos % BAND_TEX_WIDTH;
        glyph->data.glyph_loc[1] = band_pos / BAND_TEX_WIDTH;
        glyph->data.band_max[0] = nb_hbands - 1;
        glyph->data.band_max[1] = nb_vbands - 1;

        /* Band transform: maps em-space coord to band index */
        if (em_w > 0.f)
            glyph->data.band_transform[0] = (float)nb_vbands / em_w;
        else
            glyph->data.band_transform[0] = 0.f;
        if (em_h > 0.f)
            glyph->data.band_transform[1] = (float)nb_hbands / em_h;
        else
            glyph->data.band_transform[1] = 0.f;
        glyph->data.band_transform[2] = -bx0 * glyph->data.band_transform[0];
        glyph->data.band_transform[3] = -by0 * glyph->data.band_transform[1];

        /*
         * Build horizontal bands.
         * For h-bands, we divide the y-axis into nb_hbands slices.
         * A curve intersects band j if its y-extent overlaps band j's y-range.
         * Curves within each band are sorted by descending max-x.
         */

        /* Reserve space for band headers (nb_hbands + nb_vbands entries) */
        const int32_t header_start = band_pos;
        for (int32_t j = 0; j < nb_hbands + nb_vbands; j++) {
            int ret = push_texel(&s->band_texels, 0.f, 0.f, 0.f, 0.f);
            if (ret < 0)
                goto end;
            band_pos++;
        }

        /* Build h-bands */
        for (int32_t j = 0; j < nb_hbands; j++) {
            const float band_y0 = by0 + em_h * (float)j / (float)nb_hbands;
            const float band_y1 = by0 + em_h * (float)(j + 1) / (float)nb_hbands;

            ngli_darray_clear(&band_entries);
            for (size_t ci = 0; ci < nb_curves; ci++) {
                const struct curve *c = &curves[ci];
                const float min_y = NGLI_MIN(NGLI_MIN(c->p1[1], c->p2[1]), c->p3[1]);
                const float max_y = NGLI_MAX(NGLI_MAX(c->p1[1], c->p2[1]), c->p3[1]);
                if (max_y >= band_y0 && min_y <= band_y1) {
                    const float max_x = NGLI_MAX(NGLI_MAX(c->p1[0], c->p2[0]), c->p3[0]);
                    const struct band_entry e = {.curve_index = (int32_t)ci, .sort_key = max_x};
                    if (!ngli_darray_push(&band_entries, &e)) {
                        ngli_darray_reset(&band_entries);
                        return NGL_ERROR_MEMORY;
                    }
                }
            }

            /* Sort by descending max-x */
            struct band_entry *entries = ngli_darray_data(&band_entries);
            const size_t nb_entries = ngli_darray_count(&band_entries);
            if (nb_entries > 1)
                qsort(entries, nb_entries, sizeof(*entries), cmp_band_entry_desc);

            /* Write band header: (count, offset_to_curve_list, 0, 0) */
            const int32_t offset = band_pos - header_start;
            float *header = (float *)ngli_darray_data(&s->band_texels) + (size_t)(header_start + j) * 4;
            header[0] = (float)nb_entries;
            header[1] = (float)offset;

            /* Write curve locations for this band */
            for (size_t ei = 0; ei < nb_entries; ei++) {
                const int32_t ci = entries[ei].curve_index;
                const int32_t curve_texel = glyph_curve_start + ci * 2;
                const int32_t cx = curve_texel % BAND_TEX_WIDTH;
                const int32_t cy = curve_texel / BAND_TEX_WIDTH;
                int ret = push_texel(&s->band_texels, (float)cx, (float)cy, 0.f, 0.f);
                if (ret < 0)
                    goto end;
                band_pos++;
            }
        }

        /* Build v-bands */
        for (int32_t j = 0; j < nb_vbands; j++) {
            const float band_x0 = bx0 + em_w * (float)j / (float)nb_vbands;
            const float band_x1 = bx0 + em_w * (float)(j + 1) / (float)nb_vbands;

            ngli_darray_clear(&band_entries);
            for (size_t ci = 0; ci < nb_curves; ci++) {
                const struct curve *c = &curves[ci];
                const float min_x = NGLI_MIN(NGLI_MIN(c->p1[0], c->p2[0]), c->p3[0]);
                const float max_x = NGLI_MAX(NGLI_MAX(c->p1[0], c->p2[0]), c->p3[0]);
                if (max_x >= band_x0 && min_x <= band_x1) {
                    const float max_y = NGLI_MAX(NGLI_MAX(c->p1[1], c->p2[1]), c->p3[1]);
                    const struct band_entry e = {.curve_index = (int32_t)ci, .sort_key = max_y};
                    if (!ngli_darray_push(&band_entries, &e)) {
                        ngli_darray_reset(&band_entries);
                        return NGL_ERROR_MEMORY;
                    }
                }
            }

            struct band_entry *entries = ngli_darray_data(&band_entries);
            const size_t nb_entries = ngli_darray_count(&band_entries);
            if (nb_entries > 1)
                qsort(entries, nb_entries, sizeof(*entries), cmp_band_entry_desc);

            /* Write band header at offset nb_hbands + j from glyph start */
            const int32_t offset = band_pos - header_start;
            float *header = (float *)ngli_darray_data(&s->band_texels) + (size_t)(header_start + nb_hbands + j) * 4;
            header[0] = (float)nb_entries;
            header[1] = (float)offset;

            for (size_t ei = 0; ei < nb_entries; ei++) {
                const int32_t ci = entries[ei].curve_index;
                const int32_t curve_texel = glyph_curve_start + ci * 2;
                const int32_t cx = curve_texel % BAND_TEX_WIDTH;
                const int32_t cy = curve_texel / BAND_TEX_WIDTH;
                int ret = push_texel(&s->band_texels, (float)cx, (float)cy, 0.f, 0.f);
                if (ret < 0)
                    goto end;
                band_pos++;
            }
        }
    }

    /* Upload to GPU textures, growing capacity if needed */

    /* Curve texture */
    {
        const size_t nb_texels = ngli_darray_count(&s->curve_texels);
        s->curve_texture_height = nb_texels > 0 ? (int32_t)((nb_texels + BAND_TEX_WIDTH - 1) / BAND_TEX_WIDTH) : 1;

        int ret = ensure_texture_capacity(s, &s->curve_texture, &s->curve_texture_capacity, s->curve_texture_height);
        if (ret < 0) { ngli_darray_reset(&band_entries); return ret; }

        /* Pad to full allocated capacity */
        while (ngli_darray_count(&s->curve_texels) < (size_t)(s->curve_texture_capacity * BAND_TEX_WIDTH)) {
            ret = push_texel(&s->curve_texels, 0.f, 0.f, 0.f, 0.f);
            if (ret < 0)
                goto end;
        }

        ret = ngpu_texture_upload(s->curve_texture, (const uint8_t *)ngli_darray_data(&s->curve_texels), 0);
        if (ret < 0) {
            ngli_darray_reset(&band_entries);
            return ret;
        }
    }

    /* Band texture */
    {
        const size_t nb_texels = ngli_darray_count(&s->band_texels);
        s->band_texture_height = nb_texels > 0 ? (int32_t)((nb_texels + BAND_TEX_WIDTH - 1) / BAND_TEX_WIDTH) : 1;

        int ret = ensure_texture_capacity(s, &s->band_texture, &s->band_texture_capacity, s->band_texture_height);
        if (ret < 0) { ngli_darray_reset(&band_entries); return ret; }

        /* Pad to full allocated capacity */
        while (ngli_darray_count(&s->band_texels) < (size_t)(s->band_texture_capacity * BAND_TEX_WIDTH)) {
            ret = push_texel(&s->band_texels, 0.f, 0.f, 0.f, 0.f);
            if (ret < 0)
                goto end;
        }

        ret = ngpu_texture_upload(s->band_texture, (const uint8_t *)ngli_darray_data(&s->band_texels), 0);
        if (ret < 0) { ngli_darray_reset(&band_entries); return ret; }
    }

    ngli_darray_reset(&band_entries);
    return 0;

end:
    ngli_darray_reset(&band_entries);
    return NGL_ERROR_MEMORY;
}

int ngli_slug_finalize(struct slug *s)
{
    /* Clear texel data (textures are preserved and grown as needed) */
    ngli_darray_clear(&s->curve_texels);
    ngli_darray_clear(&s->band_texels);

    return finalize_internal(s);
}

size_t ngli_slug_get_glyph_count(const struct slug *s)
{
    return ngli_darray_count(&s->glyphs);
}

void ngli_slug_get_glyph_data(const struct slug *s, int32_t index, struct slug_glyph_data *out)
{
    const struct glyph_entry *glyphs = ngli_darray_data(&s->glyphs);
    *out = glyphs[index].data;
}

struct ngpu_texture *ngli_slug_get_curve_texture(const struct slug *s)
{
    return s->curve_texture;
}

struct ngpu_texture *ngli_slug_get_band_texture(const struct slug *s)
{
    return s->band_texture;
}

void ngli_slug_freep(struct slug **sp)
{
    struct slug *s = *sp;
    if (!s)
        return;

    struct glyph_entry *glyphs = ngli_darray_data(&s->glyphs);
    for (size_t i = 0; i < ngli_darray_count(&s->glyphs); i++)
        ngli_darray_reset(&glyphs[i].curves);
    ngli_darray_reset(&s->glyphs);
    ngli_darray_reset(&s->curve_texels);
    ngli_darray_reset(&s->band_texels);
    ngpu_texture_freep(&s->curve_texture);
    ngpu_texture_freep(&s->band_texture);
    ngli_freep(sp);
}
