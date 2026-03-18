/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2017 Eric Lengyel.
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
 *
 * Adapted from the Slug algorithm reference implementation by Eric Lengyel
 * (MIT license).
 */

#include path.glsl

#define kLogBandTextureWidth 12

/*
 * Unsigned distance from the origin to a quadratic bezier curve defined by
 * control points p0, p1, p2 (all relative to the query point, i.e. the query
 * point is at the origin). Based on Inigo Quilez's formulation.
 */
float udBezier(vec2 p0, vec2 p1, vec2 p2)
{
    vec2 a = p1 - p0;
    vec2 b = p0 - 2.0 * p1 + p2;

    float bb = dot(b, b);
    if (bb < 1e-10) {
        /* Degenerate case: nearly a line segment from p0 to p2 */
        vec2 e = p2 - p0;
        float ee = dot(e, e);
        if (ee < 1e-10)
            return length(p0);
        float t = clamp(-dot(p0, e) / ee, 0.0, 1.0);
        return length(p0 + e * t);
    }

    vec2 c = a * 2.0;
    vec2 d = p0;

    float kk = 1.0 / bb;
    float kx = kk * dot(a, b);
    float ky = kk * (2.0 * dot(a, a) + dot(d, b)) / 3.0;
    float kz = kk * dot(d, a);

    float p = ky - kx * kx;
    float q = kx * (2.0 * kx * kx - 3.0 * ky) + kz;
    float p3 = p * p * p;
    float h = q * q + 4.0 * p3;

    float res;
    if (h >= 0.0) {
        h = sqrt(h);
        vec2 x = (vec2(h, -h) - q) / 2.0;
        vec2 uv = sign(x) * pow(abs(x), vec2(1.0 / 3.0));
        float t = clamp(uv.x + uv.y - kx, 0.0, 1.0);
        vec2 dp = d + (c + b * t) * t;
        res = dot(dp, dp);
    } else {
        float z = sqrt(-p);
        float v = acos(clamp(q / (p * z * 2.0), -1.0, 1.0)) / 3.0;
        float m = cos(v);
        float n = sin(v) * 1.732050808;
        vec3 t = clamp(vec3(m + m, -n - m, n - m) * z - kx, 0.0, 1.0);
        vec2 dp1 = d + (c + b * t.x) * t.x;
        vec2 dp2 = d + (c + b * t.y) * t.y;
        res = min(dot(dp1, dp1), dot(dp2, dp2));
    }
    return sqrt(res);
}

uint CalcRootCode(float y1, float y2, float y3)
{
    uint i1 = floatBitsToUint(y1) >> 31U;
    uint i2 = floatBitsToUint(y2) >> 30U;
    uint i3 = floatBitsToUint(y3) >> 29U;

    uint shift = (i2 & 2U) | (i1 & ~2U);
    shift = (i3 & 4U) | (shift & ~4U);

    return ((0x2E74U >> shift) & 0x0101U);
}

vec2 SolveHorizPoly(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.y;
    float rb = 0.5 / b.y;

    float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
    float t1 = (b.y - d) * ra;
    float t2 = (b.y + d) * ra;

    if (abs(a.y) < 1.0 / 65536.0) { t1 = p12.y * rb; t2 = t1; }

    return vec2((a.x * t1 - b.x * 2.0) * t1 + p12.x, (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

vec2 SolveVertPoly(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.x;
    float rb = 0.5 / b.x;

    float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
    float t1 = (b.x - d) * ra;
    float t2 = (b.x + d) * ra;

    if (abs(a.x) < 1.0 / 65536.0) { t1 = p12.x * rb; t2 = t1; }

    return vec2((a.y * t1 - b.y * 2.0) * t1 + p12.y, (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

ivec2 CalcBandLoc(ivec2 glyphLoc, int offset)
{
    ivec2 bandLoc = ivec2(glyphLoc.x + offset, glyphLoc.y);
    bandLoc.y += bandLoc.x >> kLogBandTextureWidth;
    bandLoc.x &= (1 << kLogBandTextureWidth) - 1;
    return bandLoc;
}

float CalcCoverage(float xcov, float ycov, float xwgt, float ywgt)
{
    float coverage = max(abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0), min(abs(xcov), abs(ycov)));
    return clamp(coverage, 0.0, 1.0);
}

/*
 * Compute minimum unsigned distance to all curves in one band.
 * bandHeaderLoc points to the band header in the band texture.
 */
float minDistFromBand(ivec2 glyphLoc, ivec2 bandHeaderLoc, vec2 renderCoord)
{
    vec4 bandData = texelFetch(band_tex, bandHeaderLoc, 0);
    int count = int(bandData.x);
    ivec2 bandLoc = CalcBandLoc(glyphLoc, int(bandData.y));
    float md = 1e10;
    for (int i = 0; i < count; i++) {
        vec4 clocData = texelFetch(band_tex, ivec2(bandLoc.x + i, bandLoc.y), 0);
        ivec2 curveLoc = ivec2(clocData.xy);
        vec4 p12 = texelFetch(curve_tex, curveLoc, 0) - vec4(renderCoord, renderCoord);
        vec2 p3 = texelFetch(curve_tex, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;
        md = min(md, udBezier(p12.xy, p12.zw, p3));
    }
    return md;
}

vec2 SlugRender(vec2 renderCoord, vec4 bandTransform, ivec4 glyphData, bool needDist)
{
    vec2 emsPerPixel = fwidth(renderCoord);
    vec2 pixelsPerEm = 1.0 / emsPerPixel;

    ivec2 bandMax = glyphData.zw;
    bandMax.y &= 0x00FF;

    ivec2 bandIndex = clamp(ivec2(renderCoord * bandTransform.xy + bandTransform.zw), ivec2(0, 0), bandMax);
    ivec2 glyphLoc = glyphData.xy;

    float xcov = 0.0;
    float xwgt = 0.0;

    /* Horizontal band — coverage */
    vec4 hbandData = texelFetch(band_tex, ivec2(glyphLoc.x + bandIndex.y, glyphLoc.y), 0);
    int hcount = int(hbandData.x);
    ivec2 hbandLoc = CalcBandLoc(glyphLoc, int(hbandData.y));

    for (int curveIndex = 0; curveIndex < hcount; curveIndex++)
    {
        vec4 clocData = texelFetch(band_tex, ivec2(hbandLoc.x + curveIndex, hbandLoc.y), 0);
        ivec2 curveLoc = ivec2(clocData.xy);

        vec4 p12 = texelFetch(curve_tex, curveLoc, 0) - vec4(renderCoord, renderCoord);
        vec2 p3 = texelFetch(curve_tex, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;

        if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5) break;

        uint code = CalcRootCode(p12.y, p12.w, p3.y);
        if (code != 0U)
        {
            vec2 r = SolveHorizPoly(p12, p3) * pixelsPerEm.x;

            if ((code & 1U) != 0U)
            {
                xcov += clamp(r.x + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }

            if (code > 1U)
            {
                xcov -= clamp(r.y + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    float ycov = 0.0;
    float ywgt = 0.0;

    /* Vertical band — coverage */
    vec4 vbandData = texelFetch(band_tex, ivec2(glyphLoc.x + bandMax.y + 1 + bandIndex.x, glyphLoc.y), 0);
    int vcount = int(vbandData.x);
    ivec2 vbandLoc = CalcBandLoc(glyphLoc, int(vbandData.y));

    for (int curveIndex = 0; curveIndex < vcount; curveIndex++)
    {
        vec4 clocData = texelFetch(band_tex, ivec2(vbandLoc.x + curveIndex, vbandLoc.y), 0);
        ivec2 curveLoc = ivec2(clocData.xy);

        vec4 p12 = texelFetch(curve_tex, curveLoc, 0) - vec4(renderCoord, renderCoord);
        vec2 p3 = texelFetch(curve_tex, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;

        if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5) break;

        uint code = CalcRootCode(p12.x, p12.z, p3.x);
        if (code != 0U)
        {
            vec2 r = SolveVertPoly(p12, p3) * pixelsPerEm.y;

            if ((code & 1U) != 0U)
            {
                ycov -= clamp(r.x + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }

            if (code > 1U)
            {
                ycov += clamp(r.y + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    float coverage = CalcCoverage(xcov, ycov, xwgt, ywgt);

    float minDist = 1e10;
    if (needDist) {
        /*
         * Compute the minimum unsigned distance to all curves in the glyph by
         * iterating every h-band and v-band. Band counts are typically small
         * (2-10) so this is affordable. This is only executed when effects
         * (outline, glow, blur) are active.
         */
        for (int i = 0; i <= bandMax.y; i++) {
            ivec2 hdr = ivec2(glyphLoc.x + i, glyphLoc.y);
            minDist = min(minDist, minDistFromBand(glyphLoc, hdr, renderCoord));
        }
        int vBandBase = bandMax.y + 1;
        for (int i = 0; i <= bandMax.x; i++) {
            ivec2 hdr = ivec2(glyphLoc.x + vBandBase + i, glyphLoc.y);
            minDist = min(minDist, minDistFromBand(glyphLoc, hdr, renderCoord));
        }
    }

    float sign = coverage > 0.5 ? 1.0 : -1.0;
    return vec2(coverage, sign * minDist);
}

void main()
{
    bool needDist = outline.a != 0.0 || glow.a != 0.0 || blur != 0.0;
    vec2 result = SlugRender(texcoord, banding, glyph, needDist);
    float coverage = result.x;

    if (!needDist)
        /* Premultiplied alpha output to match the blending mode (src=ONE, dst=1-SRC_A) */
        ngl_out_color = vec4(color.rgb * color.a, color.a) * coverage;
    else
        ngl_out_color = get_path_color(result.y * dist_scale, color, outline, glow, blur, outline_pos);
}
