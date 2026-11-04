/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023 Nope Forge
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

#include "config.h"

#include <math.h>

#if HAVE_TEXT_LIBRARIES
#include <hb.h>
#include <fribidi.h>
#endif

#include "internal.h"
#include "log.h"
#include "node_text.h"
#include "nopegl/nopegl.h"
#include "path.h"
#include "slug.h"
#include "utils/darray.h"
#include "utils/hmap.h"
#include "utils/memory.h"
#include "text.h"
#include "utils/utils.h"

#if HAVE_TEXT_LIBRARIES
NGLI_DECLARE_DARRAY_WITH_NAME(hb_font_darray, hb_font_t *);

struct text_external {
    struct hb_font_darray hb_fonts; // hb_font_t*
    double px_size;
    struct slug *slug;
    struct hmap *glyph_index; // persistent glyph cache across set_string calls
};

static int face_has_outlines(hb_face_t *face)
{
    const hb_tag_t tags[] = {
        HB_TAG('g', 'l', 'y', 'f'),
        HB_TAG('C', 'F', 'F', ' '),
        HB_TAG('C', 'F', 'F', '2'),
        HB_TAG('V', 'A', 'R', 'C'),
        HB_TAG('G', 'L', 'Y', 'F'),
    };
    for (size_t i = 0; i < NGLI_ARRAY_NB(tags); i++) {
        hb_blob_t *table = hb_face_reference_table(face, tags[i]);
        const unsigned length = hb_blob_get_length(table);
        hb_blob_destroy(table);
        if (length)
            return 1;
    }
    return 0;
}

static int load_font(struct text *text, const char *font_file, int32_t face_index)
{
    struct text_external *s = text->priv_data;

    /* This limitation simplifies the UID computation in GLYPH_UID_STRING() */
    if (s->hb_fonts.count == 0xff) {
        LOG(ERROR, "maximum number of fonts reached (256)");
        return NGL_ERROR_LIMIT_EXCEEDED;
    }

    hb_face_t *face = hb_face_create_from_file_or_fail(font_file, (unsigned)face_index);
    if (!face) {
        LOG(ERROR, "unable to load font %s face %d", font_file, face_index);
        return NGL_ERROR_EXTERNAL;
    }

    if (!hb_face_get_glyph_count(face) || !face_has_outlines(face)) {
        LOG(ERROR, "only scalable OpenType faces are supported");
        hb_face_destroy(face);
        return NGL_ERROR_UNSUPPORTED;
    }

    hb_font_t *hb_font = hb_font_create(face);
    hb_face_destroy(face);
    if (hb_font == hb_font_get_empty())
        return NGL_ERROR_MEMORY;

    /* Leave the scale at the default upem so shaping and outlines use design units. */
    const unsigned ppem = (unsigned)round(s->px_size);
    hb_font_set_ppem(hb_font, ppem, ppem);
    hb_font_set_ptem(hb_font, (float)NGLI_MAX(text->config.pt_size, 1));

    LOG(DEBUG, "loaded font %s face %d", font_file, face_index);
    LOG(DEBUG, "* num glyphs: %u", hb_face_get_glyph_count(hb_font_get_face(hb_font)));
    LOG(DEBUG, "* units_per_EM: %u", hb_face_get_upem(hb_font_get_face(hb_font)));

    if (ngli_darray_try_push(&s->hb_fonts, hb_font) < 0) {
        hb_font_destroy(hb_font);
        return NGL_ERROR_MEMORY;
    }

    return 0;
}

static void free_hb_font(void *user_arg, void *data)
{
    struct hb_font_t **fontp = data;
    hb_font_destroy(*fontp);
}

static void free_glyph(void *user_arg, void *data);

static int text_external_init(struct text *text)
{
    struct text_external *s = text->priv_data;

    ngli_darray_set_free_func(&s->hb_fonts, free_hb_font, NULL);

    const int32_t pt_size = NGLI_MAX(text->config.pt_size, 1);
    const int32_t dpi = text->config.dpi ? text->config.dpi : 72;
    s->px_size = (double)pt_size * dpi / 72.0;
    /* Keep the previous 16-bit ppem limit. */
    if (s->px_size <= 0.0 || s->px_size > 65535.0) {
        LOG(ERROR, "invalid font size %d points in %d DPI", pt_size, dpi);
        return NGL_ERROR_INVALID_ARG;
    }

    for (size_t i = 0; i < text->config.nb_font_faces; i++) {
        const struct ngl_node *face_node = text->config.font_faces[i];
        const struct fontface_opts *face_opts = face_node->opts;
        int ret = load_font(text, face_opts->path, face_opts->index);
        if (ret < 0)
            return ret;
    }

    s->slug = ngli_slug_create(text->ctx);
    if (!s->slug)
        return NGL_ERROR_MEMORY;
    int ret = ngli_slug_init(s->slug);
    if (ret < 0)
        return ret;

    s->glyph_index = ngli_hmap_try_create(NGLI_HMAP_TYPE_STR);
    if (!s->glyph_index)
        return NGL_ERROR_MEMORY;
    ngli_hmap_set_free_func(s->glyph_index, free_glyph, NULL);

    return 0;
}

static int text_external_prefetch(struct text *text)
{
    struct text_external *s = text->priv_data;

    int ret = ngli_slug_prefetch(s->slug);
    if (ret < 0)
        return ret;

    if (ngli_slug_get_glyph_count(s->slug)) {
        ret = ngli_slug_finalize(s->slug);
        if (ret < 0)
            return ret;
    }

    ngli_texture_resource_set(text->curve_texture, ngli_slug_get_curve_texture(s->slug));
    ngli_texture_resource_set(text->band_texture, ngli_slug_get_band_texture(s->slug));
    return 0;
}

static void text_external_release(struct text *text)
{
    struct text_external *s = text->priv_data;
    ngli_slug_release(s->slug);
    ngli_texture_resource_set(text->curve_texture, NULL);
    ngli_texture_resource_set(text->band_texture, NULL);
}

struct glyph {
    int32_t slug_index; // index in the slug glyph array
    hb_position_t width, height; // font design units
    hb_position_t bearing_x, bearing_y; // font design units
};

static void free_glyph(void *user_arg, void *data)
{
    struct glyph *glyph = data;
    ngli_freep(&glyph);
}

static struct glyph *create_glyph(void)
{
    struct glyph *glyph = ngli_try_calloc(1, sizeof(*glyph));
    return glyph;
}

static const char *hex = "0123456789abcdef";

/* Compute a unique glyph identifier string using the face and glyph IDs */
#define GLYPH_UID_STRING(fid, gid) {  \
    hex[(fid) >> 4U & 0xf],     \
    hex[(fid)       & 0xf],     \
    '-',                        \
    hex[(gid) >> 28U & 0xf],    \
    hex[(gid) >> 24U & 0xf],    \
    hex[(gid) >> 20U & 0xf],    \
    hex[(gid) >> 16U & 0xf],    \
    hex[(gid) >> 12U & 0xf],    \
    hex[(gid) >>  8U & 0xf],    \
    hex[(gid) >>  4U & 0xf],    \
    hex[(gid)        & 0xf],    \
    '\0'                        \
}

enum run_type {
    RUN_TYPE_WORD,
    RUN_TYPE_WORDSEP,
    RUN_TYPE_LINEBREAK,
};

struct text_run {
    enum run_type type;
    size_t face_id;
    hb_buffer_t *buffer;
    const hb_glyph_info_t *glyph_infos;
    const hb_glyph_position_t *glyph_positions;
};

NGLI_DECLARE_DARRAY_WITH_NAME(text_run_darray, struct text_run);

struct outline_ctx {
    struct path *path;
    hb_position_t x_min, y_min;
    int ret;
};

static void move_to_cb(hb_draw_funcs_t *funcs, void *user, hb_draw_state_t *state,
                       float x, float y, void *user_data)
{
    struct outline_ctx *s = user;
    if (s->ret < 0)
        return;
    const float to[3] = {x - (float)s->x_min, y - (float)s->y_min, 0.f};
    s->ret = ngli_path_move_to(s->path, to);
}

static void line_to_cb(hb_draw_funcs_t *funcs, void *user, hb_draw_state_t *state,
                       float x, float y, void *user_data)
{
    struct outline_ctx *s = user;
    if (s->ret < 0)
        return;
    const float to[3] = {x - (float)s->x_min, y - (float)s->y_min, 0.f};
    s->ret = ngli_path_line_to(s->path, to);
}

static void quadratic_to_cb(hb_draw_funcs_t *funcs, void *user, hb_draw_state_t *state,
                            float cx, float cy, float x, float y, void *user_data)
{
    struct outline_ctx *s = user;
    if (s->ret < 0)
        return;
    const float ctl[3] = {cx - (float)s->x_min, cy - (float)s->y_min, 0.f};
    const float to[3]  = {x  - (float)s->x_min, y  - (float)s->y_min, 0.f};
    s->ret = ngli_path_bezier2_to(s->path, ctl, to);
}

static void cubic_to_cb(hb_draw_funcs_t *funcs, void *user, hb_draw_state_t *state,
                        float cx1, float cy1, float cx2, float cy2, float x, float y, void *user_data)
{
    struct outline_ctx *s = user;
    if (s->ret < 0)
        return;
    const float ctl1[3] = {cx1 - (float)s->x_min, cy1 - (float)s->y_min, 0.f};
    const float ctl2[3] = {cx2 - (float)s->x_min, cy2 - (float)s->y_min, 0.f};
    const float to[3]   = {x   - (float)s->x_min, y   - (float)s->y_min, 0.f};
    s->ret = ngli_path_bezier3_to(s->path, ctl1, ctl2, to);
}

static void close_path_cb(hb_draw_funcs_t *funcs, void *user, hb_draw_state_t *state, void *user_data)
{
    struct outline_ctx *s = user;
    if (s->ret < 0 || (state->current_x == state->path_start_x && state->current_y == state->path_start_y))
        return;
    s->ret = ngli_path_close(s->path);
}

static int build_glyph_index(struct text *text, struct hmap *glyph_index, const struct text_run_darray *runs_array)
{
    int ret = 0;
    struct text_external *s = text->priv_data;

    struct path *path = ngli_path_create();
    if (!path)
        return NGL_ERROR_MEMORY;

    hb_draw_funcs_t *funcs = hb_draw_funcs_create();
    if (funcs == hb_draw_funcs_get_empty()) {
        ret = NGL_ERROR_MEMORY;
        goto end;
    }
    hb_draw_funcs_set_move_to_func(funcs, move_to_cb, NULL, NULL);
    hb_draw_funcs_set_line_to_func(funcs, line_to_cb, NULL, NULL);
    hb_draw_funcs_set_quadratic_to_func(funcs, quadratic_to_cb, NULL, NULL);
    hb_draw_funcs_set_cubic_to_func(funcs, cubic_to_cb, NULL, NULL);
    hb_draw_funcs_set_close_path_func(funcs, close_path_cb, NULL, NULL);
    hb_draw_funcs_make_immutable(funcs);

    for (size_t i = 0; i < runs_array->count; i++) {
        const struct text_run *run = &runs_array->data[i];
        if (run->face_id == SIZE_MAX)
            continue;

        hb_font_t *font = s->hb_fonts.data[run->face_id];
        const size_t nb_glyphs = hb_buffer_get_length(run->buffer);
        const hb_glyph_info_t *glyph_infos = run->glyph_infos;

        for (size_t j = 0; j < nb_glyphs; j++) {
            /*
             * We can't use hb_font_get_glyph_name() since the result is not
             * unique. With some font, it may return an empty string for all
             * the glyphs (see ttf-hanazono 20170904 for an example of this).
             */
            const hb_codepoint_t glyph_id = glyph_infos[j].codepoint;
            const char glyph_uid[] = GLYPH_UID_STRING(run->face_id, glyph_id);
            if (ngli_hmap_get_str(glyph_index, glyph_uid))
                continue;

            hb_glyph_extents_t extents;
            if (!hb_font_get_glyph_extents(font, glyph_id, &extents)) {
                LOG(ERROR, "unable to get extents for glyph id %u", glyph_id);
                ret = NGL_ERROR_EXTERNAL;
                goto end;
            }

            const hb_position_t shape_w = extents.width;
            const hb_position_t shape_h = -extents.height;

            // An empty space glyph doesn't need an outline
            if (shape_w <= 0 || shape_h <= 0)
                continue;

            ngli_path_clear(path);

            struct outline_ctx outline = {
                .path = path,
                .x_min = extents.x_bearing,
                .y_min = extents.y_bearing + extents.height,
            };
            const hb_bool_t drawn = hb_font_draw_glyph_or_fail(font, glyph_id, funcs, &outline);
            ret = outline.ret;
            if (ret < 0)
                goto end;
            if (!drawn) {
                LOG(ERROR, "unable to load outline for glyph id %u", glyph_id);
                ret = NGL_ERROR_EXTERNAL;
                goto end;
            }

            ret = ngli_path_finalize(path);
            if (ret < 0)
                goto end;

            const int slug_idx = ngli_slug_add_glyph(s->slug, path, 0, (float)shape_w, (float)shape_h);
            if (slug_idx < 0) {
                ret = slug_idx;
                goto end;
            }

            /* Save the glyph in the index */
            struct glyph *glyph = create_glyph();
            if (!glyph) {
                ret = NGL_ERROR_MEMORY;
                goto end;
            }

            glyph->slug_index = slug_idx;
            glyph->width     = shape_w;
            glyph->height    = shape_h;
            glyph->bearing_x = outline.x_min;
            glyph->bearing_y = outline.y_min;

            ret = ngli_hmap_try_set_str(glyph_index, glyph_uid, glyph);
            if (ret < 0) {
                free_glyph(NULL, glyph);
                goto end;
            }
        }
    }

end:
    hb_draw_funcs_destroy(funcs);
    ngli_path_freep(&path);
    return ret;
}

static void reset_runs(struct text_run_darray *runs_array)
{
    struct text_run *runs = runs_array->data;
    for (size_t i = 0; i < runs_array->count; i++)
        hb_buffer_destroy(runs[i].buffer);
    ngli_darray_reset(runs_array);
}

/* Make sure we can hold the whole Unicode codepoints */
NGLI_STATIC_ASSERT(sizeof(FriBidiChar) == 4, "fribidi chars are 32bit");

static int char_is_linebreak(FriBidiChar ch)
{
    /* Source: https://en.wikipedia.org/wiki/Whitespace_character */
    switch (ch) {
    case 0x000A: /* line feed */
    case 0x000B: /* line tabulation */
    case 0x000C: /* form feed */
    case 0x000D: /* carriage return */
    case 0x0085: /* next line */
    case 0x2028: /* line separator */
    case 0x2029: /* paragraph separator */
        return 1;
    }
    return 0;
}

static int char_is_whitespace(FriBidiChar ch)
{
    return fribidi_get_bidi_type(ch) == FRIBIDI_TYPE_WS;
}

static size_t find_line_end(const FriBidiChar *str, size_t len, size_t start)
{
    for (size_t i = start; i < len; i++)
        if (char_is_linebreak(str[i]))
            return i;
    return len;
}

static int append_run(struct text *text, struct text_run_darray *runs_array,
                      const FriBidiChar *str, size_t len, size_t face_id,
                      enum run_type type, size_t start, size_t end)
{
    hb_buffer_t *buffer = hb_buffer_create();
    if (!hb_buffer_allocation_successful(buffer))
        return NGL_ERROR_MEMORY;

    hb_buffer_add_codepoints(buffer, str, (int)len, (unsigned)start, (int)(end - start));

    if (text->config.writing_mode == NGLI_TEXT_WRITING_MODE_VERTICAL_LR ||
        text->config.writing_mode == NGLI_TEXT_WRITING_MODE_VERTICAL_RL) {
        hb_buffer_set_direction(buffer, HB_DIRECTION_TTB);
    } else if (text->config.writing_mode == NGLI_TEXT_WRITING_MODE_HORIZONTAL_TB) {
        /* FriBidi changes the codepoints order from right-to-left where appropriate */
        hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    } else {
        ngli_assert(0);
    }

    // TODO: expose to the user? The precise knowledge of the language and/or
    // script may affect the choice of symbols, rules and conventions used
    // during shaping.
    // hb_buffer_set_script(buffer, script);
    // hb_buffer_set_language(buffer, hb_language_from_string("en", -1));

    /* Guess direction, script and language (if not previously set) */
    hb_buffer_guess_segment_properties(buffer);

    const struct text_run run = {.type=type, .face_id=face_id, .buffer=buffer};
    if (ngli_darray_try_push(runs_array, run) < 0) {
        hb_buffer_reset(buffer);
        return NGL_ERROR_MEMORY;
    }

    return 0;
}

static size_t find_face_with_codepoint(const struct hb_font_darray *fonts, FriBidiChar ch)
{
    hb_codepoint_t glyph;
    for (size_t face_id = 0; face_id < fonts->count; face_id++)
        if (hb_font_get_nominal_glyph(fonts->data[face_id], ch, &glyph) && glyph)
            return face_id;
    return SIZE_MAX;
}

/*
 * Split the sequence of codepoints into multiple runs (or just one if there is
 * only one font face needed)
 */
static int split_into_runs(struct text *text, struct text_run_darray *runs_array,
                           const FriBidiChar *str, size_t len,
                           enum run_type type, size_t start, size_t end)
{
    struct text_external *s = text->priv_data;

    ngli_assert(start < end);

    size_t prev_face_id = find_face_with_codepoint(&s->hb_fonts, str[start]);
    size_t pos = start;

    for (;;) {
        size_t face_id;
        do {
            face_id = find_face_with_codepoint(&s->hb_fonts, str[pos]);
            if (face_id != prev_face_id)
                break;
            pos++;
        } while (pos < end);

        int ret = append_run(text, runs_array, str, len, prev_face_id, type, start, pos);
        if (ret < 0)
            return ret;

        if (pos == end)
            break;

        start = pos;
        prev_face_id = face_id;
    }

    return 0;
}

static int handle_words_and_wordseps(struct text *text, struct text_run_darray *runs_array, const FriBidiChar *str, size_t len)
{
    size_t pos = 0;

    for (;;) {
        /* Handle word separators (spaces) */
        size_t end = pos;
        while (end < len && char_is_whitespace(str[end]))
            end++;
        if (end > pos) {
            int ret = split_into_runs(text, runs_array, str, len, RUN_TYPE_WORDSEP, pos, end);
            if (ret < 0)
                return ret;
        }
        if (end == len)
            break;
        pos = end;

        /* Hande words */
        while (end < len && !char_is_whitespace(str[end]))
            end++;
        if (end > pos) {
            int ret = split_into_runs(text, runs_array, str, len, RUN_TYPE_WORD, pos, end);
            if (ret < 0)
                return ret;
        }
        if (end == len)
            break;
        pos = end;
    }

    return 0;
}

static int handle_line_breaks(struct text *text, struct text_run_darray *runs_array,
                              const FriBidiChar *str, size_t len, size_t *pos)
{
    size_t end = *pos;
    while (end < len && char_is_linebreak(str[end]))
        end++;
    if (end > *pos) {
        int ret = split_into_runs(text, runs_array, str, len, RUN_TYPE_LINEBREAK, *pos, end);
        if (ret < 0)
            return ret;
        *pos = end;
    }
    return 0;
}

#define STACK_LIST_SIZE 128

/*
 * We cannot use fribidi_log2vis() because it includes a clumsy shaping causing
 * the following bug: https://github.com/fribidi/fribidi/issues/200
 * This function is pretty much identical without arabic shaping and a few
 * simplifications due to various unused arguments.
 */
static int log2vis(const FriBidiChar *str, int len, FriBidiParType *pbase_dir, FriBidiChar *out_str)
{
    int ret = 0;
    FriBidiCharType bidi_types_stack[STACK_LIST_SIZE];
    FriBidiBracketType bracket_types_stack[STACK_LIST_SIZE];
    FriBidiLevel embedding_levels_stack[STACK_LIST_SIZE];

    const int use_local = len <= STACK_LIST_SIZE;

    FriBidiCharType *bidi_types       = use_local ? bidi_types_stack       : ngli_try_malloc((size_t)len * sizeof(*bidi_types));
    FriBidiBracketType *bracket_types = use_local ? bracket_types_stack    : ngli_try_malloc((size_t)len * sizeof(*bracket_types));
    FriBidiLevel *embedding_levels    = use_local ? embedding_levels_stack : ngli_try_malloc((size_t)len * sizeof(*embedding_levels));
    if (!bidi_types || !bracket_types || !embedding_levels) {
        ret = NGL_ERROR_MEMORY;
        goto end;
    }

    fribidi_get_bidi_types(str, len, bidi_types);
    fribidi_get_bracket_types(str, len, bidi_types, bracket_types);
    if (!fribidi_get_par_embedding_levels_ex(bidi_types, bracket_types, len, pbase_dir, embedding_levels)) {
        ret = NGL_ERROR_EXTERNAL;
        goto end;
    }

    memcpy(out_str, str, (size_t)len * sizeof(*str));
    if (!fribidi_reorder_line(FRIBIDI_FLAGS_DEFAULT, bidi_types, len, 0, *pbase_dir, embedding_levels, out_str, NULL)) {
        ret = NGL_ERROR_EXTERNAL;
        goto end;
    }

end:
    if (!use_local) {
        ngli_freep(&bidi_types);
        ngli_freep(&bracket_types);
        ngli_freep(&embedding_levels);
    }
    return ret;
}

/*
 * Split text into runs, where each run is essentially a harfbuzz buffer
 */
static int build_text_runs(struct text *text, const char *str_orig, struct text_run_darray *runs_array)
{
    int ret = 0;
    struct text_external *s = text->priv_data;

    const size_t full_len = strlen(str_orig);
    if (full_len > INT32_MAX)
        return NGL_ERROR_LIMIT_EXCEEDED;

    /* Convert the full string in UTF-8 to Unicode codepoints */
    FriBidiChar *codepoints = ngli_try_calloc(full_len, sizeof(*codepoints));
    if (!codepoints)
        return NGL_ERROR_MEMORY;
    FriBidiStrIndex unicode_len = fribidi_charset_to_unicode(FRIBIDI_CHAR_SET_UTF8, str_orig, (FriBidiStrIndex)full_len, codepoints);
    ngli_assert(unicode_len <= full_len);

    /* Split input into lines and re-order codepoints of each line for bidirectionnal */
    FriBidiParType pbase_dir = FRIBIDI_PAR_ON;
    size_t pos = 0;
    for (;;) {
        ret = handle_line_breaks(text, runs_array, codepoints, (size_t)unicode_len, &pos);
        if (ret < 0)
            goto end;
        if (pos == unicode_len)
            break;

        /*
         * FriBidi works with lines (or paragraphs) so we must break the input
         * into multiple chunks: this is our first level of segmentation.
         */
        size_t end = find_line_end(codepoints, (size_t)unicode_len, pos);
        if (end == pos) {
            ngli_assert(end == unicode_len);
            break;
        }
        ngli_assert(end > pos);
        const size_t len = end - pos;

        /* Transform codepoints array from logical to visual order */
        FriBidiChar *visual_str = ngli_try_calloc(len, sizeof(*visual_str));
        if (!visual_str) {
            ret = NGL_ERROR_MEMORY;
            goto end;
        }
        ret = log2vis(&codepoints[pos], (int)len, &pbase_dir, visual_str);
        if (ret < 0) {
            ngli_freep(&visual_str);
            goto end;
        }

        /*
         * Each paragraph needs to be split into words: after shaping, it
         * won't be possible to identify whether a character is a glyph, space,
         * linebreak, etc. This our 2nd level of segmentation.
         */
        ret = handle_words_and_wordseps(text, runs_array, visual_str, len);
        ngli_freep(&visual_str);
        if (ret < 0)
            goto end;

        pos += len;
    }

    /* Run shaping on all run buffers */
    for (size_t i = 0; i < runs_array->count; i++) {
        struct text_run *run = &runs_array->data[i];
        hb_buffer_t *buffer = run->buffer;
        const size_t face_id = run->face_id != SIZE_MAX ? run->face_id : 0;
        hb_shape(s->hb_fonts.data[face_id], buffer, NULL, 0);

        /*
         * Save these pointers because they take a mutable buffer and we want to
         * make sure the output is always the same.
         */
        run->glyph_infos = hb_buffer_get_glyph_infos(buffer, NULL);
        run->glyph_positions = hb_buffer_get_glyph_positions(buffer, NULL);
    }

end:
    ngli_freep(&codepoints);
    return ret;
}

static double get_font_scale(const struct text_external *s, hb_font_t *font)
{
    return s->px_size / hb_face_get_upem(hb_font_get_face(font));
}

static double get_line_advance(hb_font_t *font, double scale)
{
    hb_font_extents_t extents;
    hb_font_get_extents_for_direction(font, HB_DIRECTION_LTR, &extents);
    /* Preserve whole-pixel line spacing, also used for vertical text. */
    const double height = (double)extents.ascender - extents.descender + extents.line_gap;
    return round(height * scale);
}

static int register_chars(struct text *text, const char *str, struct ngli_char_info_internal_darray *chars_dst,
                          const struct text_run_darray *runs_array, const struct hmap *glyph_index)
{
    struct text_external *s = text->priv_data;

    const int32_t adv_sign = text->config.writing_mode == NGLI_TEXT_WRITING_MODE_VERTICAL_LR ? 1 : -1;

    ngli_assert(s->hb_fonts.count > 0);
    hb_font_t * const *fonts = s->hb_fonts.data;

    const double default_line_advance = get_line_advance(fonts[0], get_font_scale(s, fonts[0]));
    double x_cur = 0.0, y_cur = 0.0;
    double line_advance = default_line_advance;

    for (size_t i = 0; i < runs_array->count; i++) {
        const struct text_run *run = &runs_array->data[i];
        const size_t len = hb_buffer_get_length(run->buffer);
        const hb_direction_t direction = hb_buffer_get_direction(run->buffer);
        hb_font_t *font = fonts[run->face_id != SIZE_MAX ? run->face_id : 0];
        const double scale = get_font_scale(s, font);

        /* Update line advance in case there was a font change in the middle of the line */
        if (run->face_id != SIZE_MAX) {
            const double run_line_advance = get_line_advance(font, scale);
            line_advance = NGLI_MAX(line_advance, run_line_advance);
        }

        for (size_t j = 0; j < len; j++) {
            const hb_glyph_position_t *pos = &run->glyph_positions[j];
            struct char_info_internal chr = {0};

            if (run->type == RUN_TYPE_LINEBREAK) {
                chr.tags = NGLI_TEXT_CHAR_TAG_LINE_BREAK;
                if (HB_DIRECTION_IS_HORIZONTAL(direction)) {
                    x_cur = 0;
                    y_cur += adv_sign * line_advance;
                } else {
                    y_cur = 0;
                    x_cur += adv_sign * line_advance;
                }

                /* Reset line advance to its default value */
                line_advance = default_line_advance;

                /* We assume the linebreak is never displayable */
                if (ngli_darray_try_push(chars_dst, chr) < 0)
                    return NGL_ERROR_MEMORY;
                continue;

            } else if (run->type == RUN_TYPE_WORDSEP) {
                chr.tags = NGLI_TEXT_CHAR_TAG_WORD_SEPARATOR;
            }

            const hb_codepoint_t glyph_id = run->glyph_infos[j].codepoint;
            const char glyph_uid[] = GLYPH_UID_STRING(run->face_id, glyph_id);
            const struct glyph *glyph = ngli_hmap_get_str(glyph_index, glyph_uid);
            if (glyph) {
                chr.tags |= NGLI_TEXT_CHAR_TAG_GLYPH;
                chr.x = x_cur + scale * ((double)glyph->bearing_x + pos->x_offset);
                chr.y = y_cur + scale * ((double)glyph->bearing_y + pos->y_offset);
                chr.w = scale * glyph->width;
                chr.h = scale * glyph->height;
                chr.scale[0] = 1.f;
                chr.scale[1] = 1.f;
                chr.dist_scale = 1.f / (float)hb_face_get_upem(hb_font_get_face(font));
                ngli_slug_get_glyph_data(s->slug, glyph->slug_index, &chr.slug);
            }

            if (ngli_darray_try_push(chars_dst, chr) < 0)
                return NGL_ERROR_MEMORY;

            x_cur += scale * pos->x_advance;
            y_cur += scale * pos->y_advance;
        }
    }
    return 0;
}

static int text_external_set_string(struct text *text, const char *str, struct ngli_char_info_internal_darray *chars_dst)
{
    struct text_external *s = text->priv_data;

    struct text_run_darray runs_array = {0};

    int ret = build_text_runs(text, str, &runs_array);
    if (ret < 0)
        goto end;

    const size_t prev_glyph_count = ngli_slug_get_glyph_count(s->slug);
    ret = build_glyph_index(text, s->glyph_index, &runs_array);
    if (ret < 0)
        goto end;

    /* Only rebuild GPU textures when new glyphs were added */
    if (ngli_slug_get_glyph_count(s->slug) > prev_glyph_count) {
        ret = ngli_slug_finalize(s->slug);
        if (ret < 0)
            goto end;
        ngli_texture_resource_set(text->curve_texture, ngli_slug_get_curve_texture(s->slug));
        ngli_texture_resource_set(text->band_texture, ngli_slug_get_band_texture(s->slug));
    }

    ret = register_chars(text, str, chars_dst, &runs_array, s->glyph_index);
    if (ret < 0)
        goto end;

end:
    reset_runs(&runs_array);
    return ret;
}

static void text_external_reset(struct text *text)
{
    struct text_external *s = text->priv_data;

    ngli_hmap_freep(&s->glyph_index);
    ngli_darray_reset(&s->hb_fonts);
    ngli_slug_freep(&s->slug);
}

const struct text_cls ngli_text_external = {
    .priv_size       = sizeof(struct text_external),
    .init            = text_external_init,
    .prefetch        = text_external_prefetch,
    .set_string      = text_external_set_string,
    .release         = text_external_release,
    .reset           = text_external_reset,
    .flags           = NGLI_TEXT_FLAG_MUTABLE_ATLAS,
};

#else

static int text_external_dummy_set_string(struct text *s, const char *str, struct ngli_char_info_internal_darray *chars_dst)
{
    return NGL_ERROR_BUG;
}

static int text_external_dummy_init(struct text *s)
{
    return NGL_ERROR_BUG;
}

const struct text_cls ngli_text_external = {
    .init       = text_external_dummy_init,
    .set_string = text_external_dummy_set_string,
};

#endif
