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
    if (any(lessThan(uv, clip_min)) || any(greaterThan(uv, clip_max)))
        discard;

    /* Rounded-rectangle SDF in pixel space */
    vec2 half_size = rect_size * 0.5;
    vec2 pos       = (uv - 0.5) * rect_size;
    float r        = corner_radius;
    float d        = ngli_sdf_rounded_box(pos, half_size, r);

    float aa = fwidth(d) * 0.5;

    /*
     * Outline inner/outer edges:
     *   inside (0):  outline occupies [-outline_width, 0]
     *   center (1):  outline occupies [-outline_width/2, outline_width/2]
     *   outside (2): outline occupies [0, outline_width]
     */
    float inner_edge = (outline_mode == 2) ?  0.0 : (outline_mode == 1) ? -outline_width * 0.5 : -outline_width;
    float outer_edge = (outline_mode == 0) ?  0.0 : (outline_mode == 1) ?  outline_width * 0.5 :  outline_width;

    /*
     * Inner fill boundary: same corner_radius as the outer shape so both
     * edges of the stroke ring share the same curvature.
     * inner_width == 0 for outside mode so d_fill collapses to d (no change).
     */
    float inner_width = max(-inner_edge, 0.0);
    float d_fill      = ngli_sdf_rounded_box(pos, half_size - inner_width, r);
    float fill_alpha  = 1.0 - smoothstep(-aa, aa, d_fill);

    /*
     * For the outer boundary, compute a fresh SDF for a rounded rect expanded by
     * outer_edge but with the same corner_radius.  This prevents the outer stroke
     * edge from acquiring extra corner rounding (radius = corner_radius + outer_edge)
     * that the plain "d == outer_edge" threshold would produce.
     * When outer_edge == 0 this collapses to d_outer == d, so the INSIDE mode is
     * unaffected.
     */
    float d_outer     = ngli_sdf_rounded_box(pos, half_size + outer_edge, r);
    float shape_alpha = 1.0 - smoothstep(-aa, aa, d_outer);
    float ol_mask     = clamp(shape_alpha - fill_alpha, 0.0, 1.0);

    /* Dash pattern (dash_length == 0 means solid) */
    if (dash_length > 0.0) {
        float W = half_size.x;
        float H = half_size.y;

        float t;
        if (dash_cap == 0) {
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
            float r_perim = max(r, outline_width * 0.5);
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
                else                                   t = L5 + f * arc_len;
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

        float on_len  = dash_length * dash_ratio;
        float daa     = min(length(vec2(dFdx(t), dFdy(t))) * 0.5, 2.0);

        float phase     = mod(t + dash_offset, dash_length);
        float d_to_end  = phase - on_len;
        float d_to_start = -(dash_length - phase);
        float gap_sdf   = (phase < on_len) ? d_to_end : min(d_to_end, -d_to_start);

        float dash_on;
        if (dash_cap == 0) {
            /* Butt cap: hard edge at the dash boundary */
            dash_on = 1.0 - smoothstep(-daa, daa, gap_sdf);
        } else if (dash_cap == 1) {
            /*
             * Round cap: capsule SDF in (perimeter, perpendicular) space.
             * Each dash is a capsule from (0,0) to (on_len,0) with radius
             * half_w.  The wrap-around neighbour is also checked.
             */
            float stroke_center = (inner_edge + outer_edge) * 0.5;
            float d_perp = d - stroke_center;
            float half_w = outline_width * 0.5;

            float t_clamped = clamp(phase, 0.0, on_len);
            float d_capsule = length(vec2(phase - t_clamped, d_perp)) - half_w;
            float d_wrap    = length(vec2(dash_length - phase, d_perp)) - half_w;

            float cap_sdf = min(d_capsule, d_wrap);
            float cap_aa  = fwidth(cap_sdf) * 0.5;
            dash_on = 1.0 - smoothstep(-cap_aa, cap_aa, cap_sdf);
        } else {
            /* Square cap: extend dash by half the stroke width on each end */
            float half_w = outline_width * 0.5;
            dash_on = 1.0 - smoothstep(-daa, daa, gap_sdf - half_w);
        }
        ol_mask *= dash_on;
    }

    vec4 stroke_col   = ngl_stroke_color(uv);
    float ol_alpha    = ol_mask * stroke_col.a;

    /*
     * Content transform: zoom and translate the fill content (gradient, texture…)
     * independently of the shape.  content_zoom > 1 zooms in; content_translate
     * pans in UV space.
     */
    vec2 content_uv       = (uv       - 0.5) / content_zoom + 0.5 + content_translate;
    vec2 content_tex_coord = (tex_coord - 0.5) / content_zoom + 0.5 + content_translate;

    /*
     * Only sample the texture for fragments inside the original rect (uv in
     * [0,1]).  Margin pixels introduced by geometry dilation lie outside this
     * range and must not bleed texture colour into the outline AA transition.
     */
    vec4 tex_color = vec4(0.0);
    if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
        /* Texture wrap */
        if (wrap == 1 && (any(lessThan(content_tex_coord, vec2(0.0))) || any(greaterThan(content_tex_coord, vec2(1.0)))))
            tex_color = vec4(0.0);
        else
            tex_color = ngl_color(content_uv, content_tex_coord);
    }

    /*
     * Premultiplied-alpha compositing: stroke over fill.
     * Blending uses ONE, ONE_MINUS_SRC_ALPHA so the output rgb must be
     * premultiplied (rgb *= alpha).  Using straight-alpha mixing would leave
     * non-zero rgb where alpha==0, causing fill colour to bleed into
     * transparent corner pixels (outside the rounded shape).
     */
    float fill_a = tex_color.a * fill_alpha;
    ngl_out_color.rgb = stroke_col.rgb * ol_alpha + tex_color.rgb * fill_a * (1.0 - ol_alpha);
    ngl_out_color.a   = ol_alpha + fill_a * (1.0 - ol_alpha);

    /* Global opacity */
    ngl_out_color *= opacity;
}
