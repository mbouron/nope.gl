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
#include "math_utils.h"
#include "utils/utils.h"

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

struct cie_xy {
    float x, y;
};

struct raw_primaries {
    struct cie_xy red, green, blue, white;
};

/* CIE standard illuminant series */
#define CIE_D65 {0.3127f, 0.3290f}
#define CIE_C   {0.3100f, 0.3160f}
#define CIE_E   {1.0f/3.0f, 1.0f/3.0f}
#define DCI     {0.3140f, 0.3510f}

/*
 * References:
 * https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.470-6-199811-S!!PDF-E.pdf
 * https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.601-7-201103-I!!PDF-E.pdf
 * https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-5-200204-I!!PDF-E.pdf
 * https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2020-0-201208-I!!PDF-E.pdf
 */
static const struct raw_primaries primaries_map[] = {
    /* Default to BT709 */
    [NMD_COL_PRI_RESERVED0] = {
        .red   = {0.640f, 0.330f},
        .green = {0.300f, 0.600f},
        .blue  = {0.150f, 0.060f},
        .white = CIE_D65,
    },
    [NMD_COL_PRI_BT709] = {
        .red   = {0.640f, 0.330f},
        .green = {0.300f, 0.600f},
        .blue  = {0.150f, 0.060f},
        .white = CIE_D65,
    },
    /* Default to BT709 */
    [NMD_COL_PRI_UNSPECIFIED] = {
        .red   = {0.640f, 0.330f},
        .green = {0.300f, 0.600f},
        .blue  = {0.150f, 0.060f},
        .white = CIE_D65,
    },
    /* Default to BT709 */
    [NMD_COL_PRI_RESERVED] = {
        .red   = {0.640f, 0.330f},
        .green = {0.300f, 0.600f},
        .blue  = {0.150f, 0.060f},
        .white = CIE_D65,
    },
    [NMD_COL_PRI_BT470M] = {
        .red   = {0.670f, 0.330f},
        .green = {0.210f, 0.710f},
        .blue  = {0.140f, 0.080f},
        .white = CIE_C,
    },
    [NMD_COL_PRI_BT470BG] = {
        .red   = {0.640f, 0.330f},
        .green = {0.290f, 0.600f},
        .blue  = {0.150f, 0.060f},
        .white = CIE_D65,
    },
    [NMD_COL_PRI_SMPTE170M] = {
        .red   = {0.630f, 0.340f},
        .green = {0.310f, 0.595f},
        .blue  = {0.155f, 0.070f},
        .white = CIE_D65,
    },
    [NMD_COL_PRI_SMPTE240M] = {
        .red   = {0.630f, 0.340f},
        .green = {0.310f, 0.595f},
        .blue  = {0.155f, 0.070f},
        .white = CIE_D65,
    },
    [NMD_COL_PRI_FILM] = {
        .red   = {0.681f, 0.319f},
        .green = {0.243f, 0.692f},
        .blue  = {0.145f, 0.049f},
        .white = CIE_C,
    },
    [NMD_COL_PRI_BT2020] = {
        .red   = {0.708f, 0.292f},
        .green = {0.170f, 0.797f},
        .blue  = {0.131f, 0.046f},
        .white = CIE_D65,
    },
    [NMD_COL_PRI_SMPTE428] = {
        .red   = {0.7347f, 0.2653f},
        .green = {0.2738f, 0.7174f},
        .blue  = {0.1666f, 0.0089f},
        .white = CIE_E,
    },
    [NMD_COL_PRI_SMPTE431] = {
        .red   = {0.680f, 0.320f},
        .green = {0.265f, 0.690f},
        .blue  = {0.150f, 0.060f},
        .white = DCI,
    },
    [NMD_COL_PRI_SMPTE432] = {
        .red   = {0.680f, 0.320f},
        .green = {0.265f, 0.690f},
        .blue  = {0.150f, 0.060f},
        .white = CIE_D65,
    },
    [NMD_COL_PRI_JEDEC_P22] = {
        .red   = {0.630f, 0.340f},
        .green = {0.295f, 0.605f},
        .blue  = {0.155f, 0.077f},
        .white = CIE_D65,
    },
};

static const struct raw_primaries *get_primaries(int primaries)
{
    ngli_assert(primaries >= 0 && primaries < NGLI_ARRAY_NB(primaries_map));
    return &primaries_map[primaries];
}

static float cie_X(struct cie_xy xy)
{
    return xy.x / xy.y;
}

static float cie_Z(struct cie_xy xy)
{
    return (1 - xy.x - xy.y) / xy.y;
}

/*
 * Compute the RGB → XYZ matrix as described here:
 * http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html
 */
static void get_rgb2xyz_matrix(float *dst, const struct raw_primaries *prim)
{
    float S[3], X[4], Z[4];

    X[0] = cie_X(prim->red);
    X[1] = cie_X(prim->green);
    X[2] = cie_X(prim->blue);
    X[3] = cie_X(prim->white);

    Z[0] = cie_Z(prim->red);
    Z[1] = cie_Z(prim->green);
    Z[2] = cie_Z(prim->blue);
    Z[3] = cie_Z(prim->white);

    NGLI_ALIGNED_MAT(tmp) = NGLI_MAT4_IDENTITY;

    // S = XYZ^-1 * W
    for (int i = 0; i < 3; i++) {
        tmp[0 + i] = X[i];
        tmp[4 + i] = 1;
        tmp[8 + i] = Z[i];
    }

    ngli_mat4_inverse(tmp, tmp);

    for (int i = 0; i < 3; i++)
        S[i] = tmp[4 * i + 0] * X[3] + tmp[4 * i + 1] * 1 + tmp[4 * i + 2] * Z[3];

    // M = [Sc * XYZc]
    for (int i = 0; i < 3; i++) {
        tmp[0 + i] = S[i] * X[i];
        tmp[4 + i] = S[i] * 1;
        tmp[8 + i] = S[i] * Z[i];
    }

    memcpy(dst, tmp, sizeof(tmp));
}

/*
 * Compute the XYZ → RGB matrix by inverting the corresponding RGB → XYZ matrix.
 */
static void get_xyz2rgb_matrix(float *dst, const struct raw_primaries *prim)
{
    get_rgb2xyz_matrix(dst, prim);
    ngli_mat4_inverse(dst, dst);
}

/*
 * Compute the RGB → RGB transformation matrix, converting from one set of
 * primaries to another, see:
 * http://www.brucelindbloom.com/index.html?Math.html
 */
static int get_mapping_color_matrix(float *dst, int src_primaries, int dst_primaries)
{
    static const NGLI_ALIGNED_MAT(identity) = NGLI_MAT4_IDENTITY;

    if (src_primaries == dst_primaries) {
        memcpy(dst, identity, sizeof(identity));
    }

    // XYZ → RGB matrix
    const struct raw_primaries *dst_raw_primaries = get_primaries(dst_primaries);
    NGLI_ALIGNED_MAT(xyz2rgb_matrix) = NGLI_MAT4_IDENTITY;
    get_xyz2rgb_matrix(xyz2rgb_matrix, dst_raw_primaries);

    // RGB → XYZ matrix
    const struct raw_primaries *src_raw_primaries = get_primaries(src_primaries);
    NGLI_ALIGNED_MAT(rgb2xyz_matrix) = NGLI_MAT4_IDENTITY;
    get_rgb2xyz_matrix(rgb2xyz_matrix, src_raw_primaries);

    ngli_mat4_mul(dst, xyz2rgb_matrix, rgb2xyz_matrix);

    return 0;
}

void ngli_colorconv_get_mapping_color_matrix(float *dst, const struct color_info *info, int dst_primaries)
{
    static const NGLI_ALIGNED_MAT(identity) = NGLI_MAT4_IDENTITY;

    // Only set the mapping color matrix if the transfer function is unspecified, BT709 or sRGB
    if (info->transfer != NMD_COL_TRC_UNSPECIFIED &&
        info->transfer != NMD_COL_TRC_BT709 &&
        info->transfer != NMD_COL_TRC_IEC61966_2_1) {
        memcpy(dst, identity, sizeof(identity));
        return;
    }

    get_mapping_color_matrix(dst, info->primaries, dst_primaries);
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
