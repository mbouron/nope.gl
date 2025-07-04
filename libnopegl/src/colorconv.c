/*
 * Copyright 2019-2022 GoPro Inc.
 * Copyright 2019-2022 Clément Bœsch <u pkh.me>
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

#include "log.h"
#include "colorconv.h"

enum {
    COLORMATRIX_UNDEFINED,
    COLORMATRIX_BT601,
    COLORMATRIX_BT709,
    COLORMATRIX_BT2020,
    COLORMATRIX_NB
};

#define DEFAULT_COLORMATRIX COLORMATRIX_BT709

static const char * nopemd_col_spc_str[] = {
    [NMD_COL_SPC_RGB]                = "rgb",
    [NMD_COL_SPC_BT709]              = "bt709",
    [NMD_COL_SPC_UNSPECIFIED]        = "unspecified",
    [NMD_COL_SPC_RESERVED]           = "reserved",
    [NMD_COL_SPC_FCC]                = "fcc",
    [NMD_COL_SPC_BT470BG]            = "bt470bg",
    [NMD_COL_SPC_SMPTE170M]          = "smpte170m",
    [NMD_COL_SPC_SMPTE240M]          = "smpte240m",
    [NMD_COL_SPC_YCGCO]              = "ycgco",
    [NMD_COL_SPC_BT2020_NCL]         = "bt2020_ncl",
    [NMD_COL_SPC_BT2020_CL]          = "bt2020_cl",
    [NMD_COL_SPC_SMPTE2085]          = "smpte2085",
    [NMD_COL_SPC_CHROMA_DERIVED_NCL] = "chroma_derived_ncl",
    [NMD_COL_SPC_CHROMA_DERIVED_CL]  = "chroma_derived_cl",
    [NMD_COL_SPC_ICTCP]              = "ictcp",
};

static const int color_space_map[] = {
    [NMD_COL_SPC_BT470BG]    = COLORMATRIX_BT601,
    [NMD_COL_SPC_SMPTE170M]  = COLORMATRIX_BT601,
    [NMD_COL_SPC_BT709]      = COLORMATRIX_BT709,
    [NMD_COL_SPC_BT2020_NCL] = COLORMATRIX_BT2020,
    [NMD_COL_SPC_BT2020_CL]  = COLORMATRIX_BT2020,
};

NGLI_STATIC_ASSERT(COLORMATRIX_UNDEFINED == 0, "undefined colormatrix is 0");

static const char *get_col_spc_str(int color_space)
{
    if (color_space < 0 || color_space >= NGLI_ARRAY_NB(nopemd_col_spc_str))
        return NULL;
    return nopemd_col_spc_str[color_space];
}

static int unsupported_colormatrix(int color_space)
{
    const char *colormatrix_str = get_col_spc_str(color_space);
    if (!colormatrix_str)
        LOG(WARNING, "unsupported colormatrix %d, fallback on default", color_space);
    else
        LOG(WARNING, "unsupported colormatrix %s, fallback on default", colormatrix_str);
    return DEFAULT_COLORMATRIX;
}

static int get_colormatrix_from_nopemd(int color_space)
{
    if (color_space == NMD_COL_SPC_UNSPECIFIED) {
        LOG(DEBUG, "media colormatrix unspecified, fallback on default matrix");
        return DEFAULT_COLORMATRIX;
    }

    if (color_space < 0 || color_space >= NGLI_ARRAY_NB(color_space_map))
        return unsupported_colormatrix(color_space);

    const int colormatrix = color_space_map[color_space];
    if (colormatrix == COLORMATRIX_UNDEFINED)
        return unsupported_colormatrix(color_space);

    return colormatrix;
}

static const struct k_constants {
    float r, g, b;
} k_constants_infos[] = {
    [COLORMATRIX_BT601]  = {.r = 0.2990f, .g = 0.5870f, .b = 0.1140f},
    [COLORMATRIX_BT709]  = {.r = 0.2126f, .g = 0.7152f, .b = 0.0722f},
    [COLORMATRIX_BT2020] = {.r = 0.2627f, .g = 0.6780f, .b = 0.0593f},
};

NGLI_STATIC_ASSERT(NGLI_ARRAY_NB(k_constants_infos) == COLORMATRIX_NB, "colormatrix size");

static const struct range_info {
    float y, uv, y_off;
} range_infos[] = {
    {255, 255, 0},
    {219, 224, 16},
};

int ngli_colorconv_get_ycbcr_to_rgb_color_matrix(float *dst, const struct color_info *info, float scale)
{
    const int colormatrix = get_colormatrix_from_nopemd(info->space);
    const int video_range = info->range != NMD_COL_RNG_FULL;
    const struct range_info range = range_infos[video_range];
    const struct k_constants k = k_constants_infos[colormatrix];

    const float y_factor = 255 / range.y;
    const float r_scale  = 2 * (1.f - k.r) / range.uv;
    const float b_scale  = 2 * (1.f - k.b) / range.uv;
    const float g_scale  = 2 / (k.g * range.uv);
    const float y_off    = -range.y_off / range.y;

    /* Y factor */
    dst[ 0 /* R */] = y_factor * scale;
    dst[ 1 /* G */] = y_factor * scale;
    dst[ 2 /* B */] = y_factor * scale;
    dst[ 3 /* A */] = 0;

    /* Cb factor */
    dst[ 4 /* R */] = 0;
    dst[ 5 /* G */] = -255 * g_scale * scale * k.b * (1 - k.b);
    dst[ 6 /* B */] =  255 * b_scale * scale;
    dst[ 7 /* A */] = 0;

    /* Cr factor */
    dst[ 8 /* R */] =  255 * r_scale * scale;
    dst[ 9 /* G */] = -255 * g_scale * scale * k.r * (1 - k.r);
    dst[10 /* B */] = 0;
    dst[11 /* A */] = 0;

    /* Offset */
    dst[12 /* R */] = y_off - 128 * r_scale;
    dst[13 /* G */] = y_off + 128 * g_scale * (k.b * (1 - k.b) + k.r * (1 - k.r));
    dst[14 /* B */] = y_off - 128 * b_scale;
    dst[15 /* A */] = 1;

    return 0;
}

const struct param_choices ngli_colorconv_colorspace_choices = {
    .name = "colorspace",
    .consts = {
        {"srgb",  NGLI_COLORCONV_SPACE_SRGB,  .desc=NGLI_DOCSTRING("sRGB (standard RGB)")},
        {"hsl",   NGLI_COLORCONV_SPACE_HSL,   .desc=NGLI_DOCSTRING("Hue/Saturation/Lightness (polar form of sRGB)")},
        {"hsv",   NGLI_COLORCONV_SPACE_HSV,   .desc=NGLI_DOCSTRING("Hue/Saturation/Value (polar form of sRGB)")},
        {NULL}
    }
};

static inline float linear2srgb(float x)
{
    return x < 0.0031308f ? x * 12.92f : powf(1.055f * x, 1.f/2.4f) - 0.055f;
}

static inline float srgb2linear(float x)
{
    return x < 0.04045f ? x / 12.92f : powf((x + .055f) / 1.055f, 2.4f);
}

void ngli_colorconv_srgb2linear(float *dst, const float *srgb)
{
    const float rgb[3] = {srgb2linear(srgb[0]), srgb2linear(srgb[1]), srgb2linear(srgb[2])};
    memcpy(dst, rgb, sizeof(rgb));
}

void ngli_colorconv_linear2srgb(float *dst, const float *rgb)
{
    const float srgb[3] = {linear2srgb(rgb[0]), linear2srgb(rgb[1]), linear2srgb(rgb[2])};
    memcpy(dst, srgb, sizeof(srgb));
}

static inline float sat(float x)
{
    return NGLI_CLAMP(x, 0.f, 1.f);
}

void ngli_colorconv_hsl2srgb(float *dst, const float *hsl)
{
    const float h = hsl[0], s = hsl[1], l = hsl[2];
    const float h6 = h * 6.f;
    const float c = (1.f - fabsf(2.f * l - 1.f)) * s;
    dst[0] = (sat(fabsf(h6 - 3.f) - 1.f) - .5f) * c + l;
    dst[1] = (sat(2.f - fabsf(h6 - 2.f)) - .5f) * c + l;
    dst[2] = (sat(2.f - fabsf(h6 - 4.f)) - .5f) * c + l;
}

/*
 * HSL is a polar form of an RGB coordinate, but it does not specify whether it
 * is on linear RGB or gamma encoded sRGB. While it should be linear RGB, in
 * practice it is always sRGB, so that's what we use here.
 */
void ngli_colorconv_hsl2linear(float *dst, const float *hsl)
{
    float srgb[3];
    ngli_colorconv_hsl2srgb(srgb, hsl);
    ngli_colorconv_srgb2linear(dst, srgb);
}

void ngli_colorconv_hsv2srgb(float *dst, const float *hsv)
{
    const float h = hsv[0], s = hsv[1], v = hsv[2];
    const float h6 = h * 6.f;
    const float c = v * s;
    dst[0] = (sat(fabsf(h6 - 3.f) - 1.f) - 1.f) * c + v;
    dst[1] = (sat(2.f - fabsf(h6 - 2.f)) - 1.f) * c + v;
    dst[2] = (sat(2.f - fabsf(h6 - 4.f)) - 1.f) * c + v;
}

/* See HSL-to-linear comment */
void ngli_colorconv_hsv2linear(float *dst, const float *hsv)
{
    float srgb[3];
    ngli_colorconv_hsv2srgb(srgb, hsv);
    ngli_colorconv_srgb2linear(dst, srgb);
}
