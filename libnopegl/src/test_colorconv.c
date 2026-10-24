/*
 * Copyright 2019-2022 GoPro Inc.
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

#include <stdio.h>
#include <math.h>

#include "colorconv.h"
#include "image.h"
#include "math_utils.h"
#include "utils/utils.h"

static const struct {
    int val;
    const char *name;
} ranges[] = {
    {NMD_COL_RNG_LIMITED, "limited range"},
    {NMD_COL_RNG_FULL,    "full range"},
};

static const struct {
    int val;
    const char *name;
} spaces[] = {
    {NMD_COL_SPC_BT470BG,    "bt601"},
    {NMD_COL_SPC_BT709,      "bt709"},
    {NMD_COL_SPC_BT2020_NCL, "bt2020"},
};

static const float expected_colormatrices[2][3][4 * 4]= {
    [0] = {
        [0] = {
                       85.f/73.f,                85.f/73.f,            85.f/73.f, 0.f,
                             0.f,     -1287801.f/3287200.f,      22593.f/11200.f, 0.f,
                 35751.f/22400.f,   -10689549.f/13148800.f,                  0.f, 0.f,
              -167519.f/191625.f,   59804057.f/112483875.f,   -208034.f/191625.f, 1.f,
        },
        [1] = {
                       85.f/73.f,                85.f/73.f,            85.f/73.f, 0.f,
                             0.f,  -28469543.f/133504000.f,    236589.f/112000.f, 0.f,
               200787.f/112000.f,  -71145527.f/133504000.f,                  0.f, 0.f,
              -932203.f/958125.f,   34431883.f/114208500.f,  -1085941.f/958125.f, 1.f,
        },
        [2] = {
                       85.f/73.f,                85.f/73.f,            85.f/73.f, 0.f,
                             0.f,  -94831967.f/506240000.f,    479757.f/224000.f, 0.f,
               376023.f/224000.f, -329270807.f/506240000.f,                  0.f, 0.f,
            -1754687.f/1916250.f,  250791201.f/721787500.f, -2200133.f/1916250.f, 1.f,
        },
    },
    [1] = {
        [0] = {
                             1.f,                      1.f,                  1.f, 0.f,
                             0.f,         -25251.f/73375.f,          443.f/250.f, 0.f,
                     701.f/500.f,       -209599.f/293500.f,                  0.f, 0.f,
                -22432.f/31875.f,     9939296.f/18710625.f,     -28352.f/31875.f, 1.f,
        },
        [1] = {
                             1.f,                      1.f,                  1.f, 0.f,
                             0.f,     -1674679.f/8940000.f,        4639.f/2500.f, 0.f,
                   3937.f/2500.f,     -4185031.f/8940000.f,                  0.f, 0.f,
              -125984.f/159375.f,     4687768.f/14248125.f,   -148448.f/159375.f, 1.f,
        },
        [2] = {
                             1.f,                      1.f,                  1.f, 0.f,
                             0.f,    -5578351.f/33900000.f,        9407.f/5000.f, 0.f,
                   7373.f/5000.f,   -19368871.f/33900000.f,                  0.f, 0.f,
              -117968.f/159375.f,   99788888.f/270140625.f,   -150512.f/159375.f, 1.f,
        },
    },
};

static int compare_matrices(const float *a, const float *b)
{
    int fail = 0;
    float diff[4 * 4];
    for (size_t i = 0; i < NGLI_ARRAY_NB(diff); i++) {
        diff[i] = fabsf(a[i] - b[i]);
        fail += diff[i] > 1e-6;
    }
    printf("diff:\n" NGLI_FMT_MAT4 "\n\n", NGLI_ARG_MAT4(diff));
    return fail ? -fail : 0;
}

static int check_reference_levels(const struct color_info *cinfo, int depth)
{
    const float max = (float)((1 << depth) - 1);
    const int limited = cinfo->range != NMD_COL_RNG_FULL;
    const float shift = (float)(1 << (depth - 8));
    const float black = limited ? 16.f * shift : 0.f;
    const float white = limited ? 235.f * shift : max;
    const float neutral = limited ? 128.f * shift : (float)(1 << (depth - 1));
    const float gray = (black + white) / 2.f;
    const float expected[][2] = {{black, 0.f}, {white, 1.f}, {gray, .5f}};

    const struct ngli_mat4 mat = ngli_colorconv_get_ycbcr_to_rgb_color_matrix(cinfo, depth, 1.f);
    int fail = 0;
    for (size_t i = 0; i < NGLI_ARRAY_NB(expected); i++) {
        NGLI_ALIGNED_VEC(ycbcr) = {expected[i][0] / max, neutral / max, neutral / max, 1.f};
        NGLI_ALIGNED_VEC(rgb);
        ngli_mat4_mul_vec4(rgb, mat.m, ycbcr);
        for (int c = 0; c < 3; c++) {
            if (fabsf(rgb[c] - expected[i][1]) > 1e-5f) {
                printf(">>>> %d-bit code value %g: %g instead of %g <<<<\n", depth, expected[i][0], rgb[c], expected[i][1]);
                fail++;
            }
        }
    }
    return fail;
}

static const struct {
    int src;
    const char *name;
    float matrix[3][3];
} to_bt2020[] = {
    {NMD_COL_PRI_BT709, "bt709", {
        {0.627404f, 0.329283f, 0.043313f},
        {0.069097f, 0.919540f, 0.011362f},
        {0.016391f, 0.088013f, 0.895595f},
    }},
    {NMD_COL_PRI_SMPTE432, "display-p3", {
        { 0.753833f, 0.198597f, 0.047570f},
        { 0.045744f, 0.941777f, 0.012479f},
        {-0.001210f, 0.017601f, 0.983609f},
    }},
    {NMD_COL_PRI_BT2020, "bt2020", {
        {1.f, 0.f, 0.f},
        {0.f, 1.f, 0.f},
        {0.f, 0.f, 1.f},
    }},
};

static int check_primaries_matrices(void)
{
    int fail = 0;
    for (size_t i = 0; i < NGLI_ARRAY_NB(to_bt2020); i++) {
        const struct ngli_mat4 mat = ngli_colorconv_get_primaries_color_matrix(to_bt2020[i].src, NMD_COL_PRI_BT2020);
        printf("%s to bt2020:\n" NGLI_FMT_MAT4 "\n\n", to_bt2020[i].name, NGLI_ARG_MAT4(mat.m));
        for (int c = 0; c < 3; c++) {
            NGLI_ALIGNED_VEC(src) = {0};
            NGLI_ALIGNED_VEC(dst);
            src[c] = 1.f;
            ngli_mat4_mul_vec4(dst, mat.m, src);
            for (int r = 0; r < 3; r++) {
                const float v = dst[r];
                if (fabsf(v - to_bt2020[i].matrix[r][c]) > 1e-4f) {
                    printf(">>>> %s to bt2020 [%d][%d]: %g instead of %g <<<<\n",
                           to_bt2020[i].name, r, c, v, to_bt2020[i].matrix[r][c]);
                    fail++;
                }
            }
        }
    }
    return fail;
}

int main(void)
{
    int fail = 0;
    struct color_info cinfo = NGLI_COLOR_INFO_DEFAULTS;

    for (size_t r = 0; r < NGLI_ARRAY_NB(ranges); r++) {
        cinfo.range = ranges[r].val;
        for (size_t s = 0; s < NGLI_ARRAY_NB(spaces); s++) {
            cinfo.space = spaces[s].val;
            const struct ngli_mat4 mat = ngli_colorconv_get_ycbcr_to_rgb_color_matrix(&cinfo, 8, 1.f);
            printf("%s %s:\n" NGLI_FMT_MAT4 "\n\n", spaces[s].name, ranges[r].name, NGLI_ARG_MAT4(mat.m));
            if (compare_matrices(mat.m, expected_colormatrices[r][s]) < 0) {
                printf(">>>> DIFF IS TOO HIGH <<<<\n\n");
                fail++;
            }
            static const int depths[] = {8, 10, 16};
            for (size_t d = 0; d < NGLI_ARRAY_NB(depths); d++)
                fail += check_reference_levels(&cinfo, depths[d]);
        }
    }
    fail += check_primaries_matrices();
    return fail;
}
