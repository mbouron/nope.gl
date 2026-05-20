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

float ngli_sdf_rounded_box(vec2 pos, vec2 half_size, float radius)
{
    vec2 q = abs(pos) - half_size + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

void main()
{
    /* Clip rect (in UV space; disabled when bounds are ±1e9) */
    if (any(lessThan(ngli_uv, ngli_clip_min)) || any(greaterThan(ngli_uv, ngli_clip_max)))
        discard;

    /* Rounded-rectangle SDF in pixel space */
    vec2 half_size = ngli_rect_size * 0.5;
    vec2 pos       = (ngli_uv - 0.5) * ngli_rect_size;
    float r        = ngli_corner_radius;
    float d        = ngli_sdf_rounded_box(pos, half_size, r);

    /*
     * Use the screen-space position derivatives instead of fwidth(d) for the
     * anti-aliasing width.  The box SDF uses max(q.x, q.y) in its interior,
     * which has a derivative discontinuity at sharp corners (where q.x ==
     * q.y).  Within the GPU 2×2 fragment quad, max() can select different axes
     * in neighbouring fragments, making fwidth(d) depend on which row/column
     * the hardware samples — producing different results across GPUs.  pos
     * always has smooth, hardware-independent derivatives so min(fwidth) gives
     * a stable per-pixel transition width.
     */
    float aa = min(fwidth(pos.x), fwidth(pos.y)) * 0.5;

    /*
     * Outline inner/outer edges:
     *   inside (0):  outline occupies [-ngli_outline_width, 0]
     *   center (1):  outline occupies [-ngli_outline_width/2, ngli_outline_width/2]
     *   outside (2): outline occupies [0, ngli_outline_width]
     */
    float inner_edge = (ngli_outline_mode == 2) ?  0.0 : (ngli_outline_mode == 1) ? -ngli_outline_width * 0.5 : -ngli_outline_width;
    float outer_edge = (ngli_outline_mode == 0) ?  0.0 : (ngli_outline_mode == 1) ?  ngli_outline_width * 0.5 :  ngli_outline_width;

    /* Fill boundary: always the original rect (no stroke shrinkage) */
    float fill_alpha  = 1.0 - smoothstep(-aa, aa, d);

    /*
     * Stroke ring: inner edge shrunk by inner_width, outer edge expanded by
     * outer_edge, both with the same ngli_corner_radius so curvature is
     * consistent across the two edges.
     */
    float inner_width  = max(-inner_edge, 0.0);
    float d_inner      = ngli_sdf_rounded_box(pos, half_size - inner_width, r);
    float inner_alpha  = 1.0 - smoothstep(-aa, aa, d_inner);
    float d_outer      = ngli_sdf_rounded_box(pos, half_size + outer_edge, r);
    float outer_alpha  = 1.0 - smoothstep(-aa, aa, d_outer);
    float ol_mask      = clamp(outer_alpha - inner_alpha, 0.0, 1.0);

    /* Dash pattern (ngli_dash_length == 0 means solid) */
    if (ngli_dash_length > 0.0) {
        float W = half_size.x;
        float H = half_size.y;

        float t;
        if (ngli_dash_cap == 0) {
            /*
             * Butt cap: AABB perimeter coordinate.
             * The nearest-edge heuristic is sufficient for 1D dash masks
             * and preserves a total perimeter of exactly 4W+4H.
             */
            float dx_edge = W - abs(pos.x);
            float dy_edge = H - abs(pos.y);
            if (dx_edge < dy_edge) {
                t = pos.x > 0.0
                    ? (4.0*W + 2.0*H) + (H - pos.y)
                    : 2.0*W + (H + pos.y);
            } else {
                t = pos.y < 0.0
                    ? W - pos.x
                    : (2.0*W + 2.0*H) + (W + pos.x);
            }
        } else {
            /*
             * Round / square cap: arc-length perimeter along a rounded-rect
             * boundary.  Uses an effective corner radius (at least half the
             * stroke width) so the arc zone always spans enough pixels for a
             * smooth t transition through corners, eliminating diagonal seam
             * artifacts from the AABB nearest-edge heuristic.
             */
            float r_perim = max(r, ngli_outline_width * 0.5);
            float Wr = W - r_perim;
            float Hr = H - r_perim;
            vec2 qp  = abs(pos) - vec2(Wr, Hr);

            float half_pi = 1.5707963;
            float arc_len = r_perim * half_pi;

            float L1 = 2.0 * Wr;
            float L2 = L1 + arc_len;
            float L3 = L2 + 2.0 * Hr;
            float L4 = L3 + arc_len;
            float L5 = L4 + 2.0 * Wr;
            float L6 = L5 + arc_len;
            float L7 = L6 + 2.0 * Hr;

            if (qp.x > 0.0 && qp.y > 0.0) {
                float alpha = atan(qp.y, qp.x);
                float f = alpha / half_pi;
                bool same_sign = (pos.x > 0.0) == (pos.y > 0.0);
                if (same_sign) f = 1.0 - f;

                if      (pos.x > 0.0 && pos.y < 0.0) t = L7 + f * arc_len;
                else if (pos.x < 0.0 && pos.y < 0.0) t = L1 + f * arc_len;
                else if (pos.x < 0.0 && pos.y > 0.0) t = L3 + f * arc_len;
                else                                 t = L5 + f * arc_len;
            } else if (qp.x > qp.y) {
                t = pos.x > 0.0
                    ? L6 + (Hr - pos.y)
                    : L2 + (pos.y + Hr);
            } else {
                t = pos.y < 0.0
                    ? Wr - pos.x
                    : L4 + (pos.x + Wr);
            }
        }

        float on_len = ngli_dash_length * ngli_dash_ratio;
        float daa    = min(length(vec2(dFdx(t), dFdy(t))) * 0.5, 2.0);

        float phase      = mod(t + ngli_dash_offset, ngli_dash_length);
        float d_to_end   = phase - on_len;
        float d_to_start = -(ngli_dash_length - phase);
        float gap_sdf    = (phase < on_len) ? d_to_end : min(d_to_end, -d_to_start);

        float dash_on;
        if (ngli_dash_cap == 0) {
            /* Butt cap: hard edge at the dash boundary */
            dash_on = 1.0 - smoothstep(-daa, daa, gap_sdf);
        } else if (ngli_dash_cap == 1) {
            /*
             * Round cap: capsule SDF in (perimeter, perpendicular) space.
             * Each dash is a capsule from (0,0) to (on_len,0) with radius
             * half_w.  The wrap-around neighbour is also checked.
             */
            float stroke_center = (inner_edge + outer_edge) * 0.5;
            float d_perp = d - stroke_center;
            float half_w = ngli_outline_width * 0.5;

            float t_clamped = clamp(phase, 0.0, on_len);
            float d_capsule = length(vec2(phase - t_clamped, d_perp)) - half_w;
            float d_wrap    = length(vec2(ngli_dash_length - phase, d_perp)) - half_w;

            float cap_sdf = min(d_capsule, d_wrap);
            float cap_aa  = fwidth(cap_sdf) * 0.5;
            dash_on = 1.0 - smoothstep(-cap_aa, cap_aa, cap_sdf);
        } else {
            /* Square cap: extend dash by half the stroke width on each end */
            float half_w = ngli_outline_width * 0.5;
            dash_on = 1.0 - smoothstep(-daa, daa, gap_sdf - half_w);
        }
        ol_mask *= dash_on;
    }

    vec4 stroke_col = ngli_stroke_color(ngli_uv);
    float ol_alpha  = ol_mask * stroke_col.a * ngli_stroke_opacity;

    /*
     * Content transform: orientation, zoom and translate applied to the fill
     * content (gradient, texture…) independently of the shape.
     *
     * ngli_content_orientation is vec2(cos(angle), sin(angle)) for discrete
     * 90° rotations applied around UV center (0.5, 0.5).
     * ngli_content_zoom > 1 zooms in; ngli_content_translate pans in UV space.
     */
    float co = ngli_content_orientation.x;
    float so = ngli_content_orientation.y;
    mat2 rot = mat2(co, so, -so, co);
    vec2 content_uv        = rot * ((ngli_uv        - 0.5) / ngli_content_zoom + ngli_content_translate) + 0.5;
    vec2 content_tex_coord = rot * ((ngli_tex_coord - 0.5) / ngli_content_zoom + ngli_content_translate) + 0.5;

    /*
     * Only sample the fill for fragments inside the original rect (ngli_uv in
     * [0,1]).  Margin pixels introduced by geometry dilation lie outside this
     * range and must not bleed colour into the outline AA transition.
     */
    vec4 tex_color = vec4(0.0);
    if (all(greaterThanEqual(ngli_uv, vec2(0.0))) && all(lessThanEqual(ngli_uv, vec2(1.0)))) {
        tex_color = ngli_color(content_uv, content_tex_coord);
    }

    /*
     * Premultiplied-alpha compositing: stroke over fill.
     * Blending uses ONE, ONE_MINUS_SRC_ALPHA so the output rgb must be
     * premultiplied (rgb *= alpha).  Using straight-alpha mixing would leave
     * non-zero rgb where alpha==0, causing fill colour to bleed into
     * transparent corner pixels (outside the rounded shape).
     */
    float fill_rgb_scale = (ngli_fill_premult != 0 ? tex_color.a : 1.0)
                         * fill_alpha * ngli_fill_opacity;
    float fill_a = tex_color.a * fill_alpha * ngli_fill_opacity;
    ngl_out_color.rgb = stroke_col.rgb * ol_alpha + tex_color.rgb * fill_rgb_scale * (1.0 - ol_alpha);
    ngl_out_color.a   = ol_alpha + fill_a * (1.0 - ol_alpha);

    /* Global opacity */
    ngl_out_color *= ngli_opacity;
}
