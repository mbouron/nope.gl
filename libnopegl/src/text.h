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

#ifndef TEXT_H
#define TEXT_H

#include <nopegpu/nopegpu.h>

#include "box.h"
#include "nopegl/nopegl.h"
#include "resource.h"
#include "slug.h"
#include "utils/darray.h"

enum writing_mode {
    NGLI_TEXT_WRITING_MODE_HORIZONTAL_TB,
    NGLI_TEXT_WRITING_MODE_VERTICAL_RL,
    NGLI_TEXT_WRITING_MODE_VERTICAL_LR,
    NGLI_TEXT_WRITING_MODE_MAX_ENUM = 0x7FFFFFFF
};

enum text_valign {
    NGLI_TEXT_VALIGN_CENTER,
    NGLI_TEXT_VALIGN_TOP,
    NGLI_TEXT_VALIGN_BOTTOM,
    NGLI_TEXT_VALIGN_MAX_ENUM = 0x7FFFFFFF
};

enum text_halign {
    NGLI_TEXT_HALIGN_CENTER,
    NGLI_TEXT_HALIGN_RIGHT,
    NGLI_TEXT_HALIGN_LEFT,
    NGLI_TEXT_HALIGN_MAX_ENUM = 0x7FFFFFFF
};

enum text_scale_mode {
    NGLI_TEXT_SCALE_MODE_AUTO,
    NGLI_TEXT_SCALE_MODE_FIXED,
    NGLI_TEXT_SCALE_MODE_MAX_ENUM = 0x7FFFFFFF
};

enum {
    NGLI_TEXT_CHAR_TAG_GLYPH          = 1U << 0,
    NGLI_TEXT_CHAR_TAG_WORD_SEPARATOR = 1U << 1,
    NGLI_TEXT_CHAR_TAG_LINE_BREAK     = 1U << 2,
};

/* Exposed by text drivers  */
struct char_info_internal {
    double x, y, w, h; // pixel canvas coordinates, kept in double precision until GPU geometry conversion
    struct ngli_aabb atlas_coords; // atlas coordinates (distmap path)
    float scale[2]; // geometry scaling factors
    float dist_scale; // outline coordinates to em units for effects
    uint32_t tags; // combination of NGLI_TEXT_CHAR_TAG_*
    struct slug_glyph_data slug; // slug rendering data
};

/* Exposed by the text API */
struct char_info {
    struct ngli_aabb geom;  // vertices
    struct ngli_aabb atlas_coords; // texture position (used by distmap path)
    float real_dim[2];      // real dimension (without distance field padding)
    float dist_scale; // outline coordinates to em units for effects
    struct slug_glyph_data slug; // slug rendering data
};

/* User-requested defaults for all the characters */
struct text_effects_defaults {
    float color[3];
    float opacity;
};

struct text_config {
    struct ngl_node **font_faces;
    size_t nb_font_faces;
    int32_t pt_size;
    int32_t dpi;
    int32_t padding;
    enum text_scale_mode scale_mode;
    float font_scale;
    enum text_valign valign;
    enum text_halign halign;
    enum writing_mode writing_mode;
    struct ngli_box box;
    struct ngl_node **effect_nodes;
    size_t nb_effect_nodes;
    struct text_effects_defaults defaults;
};

struct text;

#define NGLI_TEXT_FLAG_MUTABLE_ATLAS (1 << 0) // whether set_string() can change the atlas texture or not

NGLI_DECLARE_DARRAY_WITH_NAME(ngli_char_info_darray, struct char_info);
NGLI_DECLARE_DARRAY_WITH_NAME(ngli_char_info_internal_darray, struct char_info_internal);

/* structure reserved for internal implementations */
struct text_cls {
    int (*init)(struct text *text);
    int (*prefetch)(struct text *text);
    int (*set_string)(struct text *text, const char *str, struct ngli_char_info_internal_darray *chars_dst);
    void (*release)(struct text *text);
    void (*reset)(struct text *text);
    size_t priv_size;
    uint32_t flags; // combination of NGLI_TEXT_FLAG_*
};

struct text_char_data {
    float vertices[4];
    float texcoord_bounds[4]; // em-space bounding box
    float band_transform[4];  // band scale and offset
    int32_t glyph_data[4];    // glyph_loc, band_max
    float dist_scale;         // outline coordinates to em units
    float transform[4 * 4];
    float color[4];   // color and opacity
    float outline[4]; // color and width
    float glow[4];    // color and amount
    float blur;
    float outline_pos;
};

struct effect_segmentation {
    size_t *positions;     // character index (in chars darray) to position in "target unit" (char, word, ...)
    size_t total_segments; // total number of segment: all values in positions are between [0,total_segments-1]
};

struct text {
    struct ngl_ctx *ctx;
    struct text_config config;

    /* public */
    double width;
    double height;
    struct ngli_char_info_darray chars;
    struct ngpu_texture *atlas_texture;
    struct texture_resource *curve_texture;
    struct texture_resource *band_texture;
    struct text_char_data *chars_data;

    /* effects specific */
    struct effect_segmentation *effects;
    struct text_char_data *chars_data_default; // used to reset chars_data before every update
    size_t chars_data_size;    // size of chars_data_default and chars_data

    struct ngli_char_info_internal_darray chars_internal;

    const struct text_cls *cls;
    void *priv_data;
};

struct text *ngli_text_create(struct ngl_ctx *ctx);
int ngli_text_init(struct text *s, const struct text_config *cfg);
int ngli_text_prefetch(struct text *s);
void ngli_text_release(struct text *s);

/* The specified new user defaults will be honored at the next ngli_text_set_{string,time}() call */
void ngli_text_update_effects_defaults(struct text *s, const struct text_effects_defaults *defaults);

void ngli_text_refresh_geometry_data(struct text *s);

int ngli_text_set_string(struct text *s, const char *str);

int ngli_text_set_time(struct text *s, double t);

void ngli_text_freep(struct text **sp);

#endif
