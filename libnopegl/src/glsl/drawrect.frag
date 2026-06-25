/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2015 Inigo Quilez
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

/*
 * Signed distance to an axis-aligned ellipse centred at the origin with
 * semi-axes (ab.x, ab.y).
 *
 * Reference: https://iquilezles.org/articles/ellipsedist/
 */
float ngli_sdf_ellipse(vec2 p, vec2 ab)
{
    p = abs(p);
    bool s = dot(p / ab, p / ab) > 1.0;
    float w = s
        ? atan(p.y * ab.x, p.x * ab.y)
        : ((ab.x * (p.x - ab.x) < ab.y * (p.y - ab.y)) ? 1.5707963 : 0.0);
    for (int i = 0; i < 5; i++) {
        vec2 cs = vec2(cos(w), sin(w));
        vec2 u  = ab * vec2( cs.x, cs.y);
        vec2 v  = ab * vec2(-cs.y, cs.x);
        w = w + dot(p - u, v) / (dot(p - u, u) + dot(v, v));
    }
    return length(p - ab * vec2(cos(w), sin(w))) * (s ? 1.0 : -1.0);
}

/*
 * Signed distance to a rounded rectangle with per-axis corner radii.
 */
float ngli_sdf_rounded_box(vec2 pos, vec2 half_size, vec2 radius)
{
    if (radius.x == radius.y) {
        vec2 q = abs(pos) - half_size + radius.x;
        return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius.x;
    }
    vec2 d = abs(pos) - half_size;
    vec2 q = d + radius;
    if (radius.x > 0.0 && radius.y > 0.0 && q.x > 0.0 && q.y > 0.0)
        return ngli_sdf_ellipse(q, radius);
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

/*
 * 2D point on one quarter-corner of the dash centerline, at arc length `seg`
 * into a corner of total arc length `arc`.  `c` is the corner-arc centre, `r`
 * its semi-axes, `q` the quadrant sign of the corner (e.g. (-1,-1) for
 * top-left), and `flip` selects the sweep direction so the result matches the
 * dash arc-length coordinate t.  The angular parameter is the polar angle of
 * the offset (the same atan() used to build t), so this inverts t back to a
 * 2D location.
 */
vec2 ngli_perim_corner(float seg, float arc, vec2 c, vec2 r, vec2 q, bool flip)
{
    float g     = seg / arc;
    float alpha = (flip ? (1.0 - g) : g) * 1.5707963;
    vec2 dir    = vec2(cos(alpha), sin(alpha));
    r           = max(r, vec2(1e-6));        /* guard the divisions for degenerate radii */
    float rho   = 1.0 / sqrt(dot(dir / r, dir / r));
    vec2 qp     = rho * dir;                 /* offset from centre, positive quadrant */
    return c + q * qp;
}

/*
 * Inverse of the dash arc-length coordinate: the 2D point on the rounded-rect
 * centerline at arc length s (with the same edge/corner layout used to build
 * t).  Used to place geometrically correct round dash caps, which would
 * otherwise distort where the perimeter curves around a corner.
 */
vec2 ngli_perim_point(float s, float Wr, float Hr, vec2 r, float arc)
{
    float W  = Wr + r.x;
    float H  = Hr + r.y;
    float L1 = 2.0 * Wr;
    float L2 = L1 + arc;
    float L3 = L2 + 2.0 * Hr;
    float L4 = L3 + arc;
    float L5 = L4 + 2.0 * Wr;
    float L6 = L5 + arc;
    float L7 = L6 + 2.0 * Hr;
    s = mod(s, L7 + arc);
    if (s < L1) return vec2(Wr - s, -H);
    if (s < L2) return ngli_perim_corner(s - L1, arc, vec2(-Wr, -Hr), r, vec2(-1.0, -1.0), true);
    if (s < L3) return vec2(-W, -Hr + (s - L2));
    if (s < L4) return ngli_perim_corner(s - L3, arc, vec2(-Wr,  Hr), r, vec2(-1.0,  1.0), false);
    if (s < L5) return vec2(-Wr + (s - L4), H);
    if (s < L6) return ngli_perim_corner(s - L5, arc, vec2( Wr,  Hr), r, vec2( 1.0,  1.0), true);
    if (s < L7) return vec2(W, Hr - (s - L6));
    return ngli_perim_corner(s - L7, arc, vec2( Wr, -Hr), r, vec2( 1.0, -1.0), false);
}

void main()
{
    /*
     * Rounded clip rectangles: each clip is a rounded box evaluated in its own
     * local space: map the canvas-pixel position into that space (inverse of
     * the clip's transform), evaluate the rounded-box SDF and anti-alias the
     * edge over one pixel. The clip region is the intersection of all clips,
     * so we take the minimum coverage.
     */
    float clip_cov = 1.0;
    for (int i = 0; i < ngli_nb_clips; i++) {
        vec4 inv = ngli_clip_inv[i];
        vec4 rc  = ngli_clip_rect[i];
        vec2 rad = ngli_clip_radius[i].xy;
        vec2 dlt = ngli_clip_pos - rc.xy;
        vec2 loc = vec2(dot(inv.xy, dlt), dot(inv.zw, dlt));
        float cd  = ngli_sdf_rounded_box(loc, rc.zw, rad);
        float caa = max(fwidth(cd) * 0.5, 1e-6);
        clip_cov = min(clip_cov, 1.0 - smoothstep(-caa, caa, cd));
    }
    if (clip_cov <= 0.0)
        discard;

    /* Rounded-rectangle SDF in pixel space */
    vec2 half_size = ngli_rect_size * 0.5;
    vec2 pos       = (ngli_uv - 0.5) * ngli_rect_size;
    vec2 r         = ngli_corner_radius;
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
        float stroke_center = (inner_edge + outer_edge) * 0.5;

        /*
         * Perimeter coordinate: arc length along the dash path, replacing the
         * former AABB nearest-edge heuristic whose iso-t lines kinked along the
         * corner diagonal and sliced any dash landing there.
         *
         * The path depends on the cap:
         *   - Round caps run along the stroke centerline itself (the boundary
         *     offset by stroke_center).  A convex corner then rounds by that
         *     offset, so e.g. an outside stroke turns a sharp corner with a
         *     smooth join instead of a square block, and the cap end points
         *     derived from this path stay exactly on the stroke.
         *   - Butt/square caps have no end disk, so the boundary is merely
         *     inflated to at least half the stroke width: enough corner arc for
         *     a smooth, seam-free t even at a sharp corner.
         */
        bool  round_cap = ngli_dash_cap == 1;
        vec2  half_d  = round_cap ? half_size + vec2(stroke_center) : half_size;
        vec2  r_perim = round_cap ? max(r + vec2(stroke_center), vec2(0.0))
                                  : max(r, vec2(ngli_outline_width * 0.5));
        float W = half_d.x;
        float H = half_d.y;
        float Wr = max(W - r_perim.x, 0.0);
        float Hr = max(H - r_perim.y, 0.0);
        vec2 qp  = abs(pos) - vec2(Wr, Hr);

        float half_pi = 1.5707963;
        /*
         * Quarter ellipse arc length (Ramanujan I approximation).
         */
        float a = r_perim.x;
        float b = r_perim.y;
        float arc_len = 0.7853981 * (3.0 * (a + b) - sqrt((3.0 * a + b) * (a + 3.0 * b)));

        float L1 = 2.0 * Wr;
        float L2 = L1 + arc_len;
        float L3 = L2 + 2.0 * Hr;
        float L4 = L3 + arc_len;
        float L5 = L4 + 2.0 * Wr;
        float L6 = L5 + arc_len;
        float L7 = L6 + 2.0 * Hr;

        float t;
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

        /*
         * Even tiling: snap the dash period so an exact whole number of periods
         * fits the perimeter.  Without this, the requested ngli_dash_length
         * rarely divides the perimeter evenly, leaving a partial dash at the
         * t == 0 wrap point that slides around (and pops in and out) whenever
         * the length changes.  Dividing the perimeter into round(perim/length)
         * equal periods makes the pattern close on itself seamlessly; the
         * effective period only departs from the request by up to half a dash.
         */
        float perim  = L7 + arc_len;
        float n      = max(floor(perim / ngli_dash_length + 0.5), 1.0);
        float period = perim / n;

        float on_len = period * ngli_dash_ratio;

        float phase      = mod(t + ngli_dash_offset, period);
        float d_to_end   = phase - on_len;
        float d_to_start = -(period - phase);
        float gap_sdf    = (phase < on_len) ? d_to_end : min(d_to_end, -d_to_start);

        /*
         * daa is the dash-boundary anti-alias width in arc-length units; it
         * needs the derivative of t, so it is only taken inside the butt/square
         * branches (uniform control flow — ngli_dash_cap is a uniform).  The
         * round cap anti-aliases in screen space with aa instead.
         */
        float dash_on;
        if (ngli_dash_cap == 0) {
            /* Butt cap: hard edge at the dash boundary */
            float daa = min(length(vec2(dFdx(t), dFdy(t))) * 0.5, 2.0);
            dash_on = 1.0 - smoothstep(-daa, daa, gap_sdf);
        } else if (ngli_dash_cap == 1) {
            /*
             * Round cap: a dash is the stroke of the centerline segment between
             * s_start and s_end — i.e. every point within half_w of that segment
             * (a capsule following the curved path).  The distance to it is
             * evaluated in true 2D: inside the span it is the perpendicular
             * stroke distance |d_perp|; beyond it, the distance to the segment's
             * endpoint, which ngli_perim_point gives directly (the path is the
             * centerline).  Measuring real 2D distance — rather than an unrolled
             * (arc-length, perpendicular) capsule — keeps the caps perfectly
             * circular where the perimeter curves around a corner; using
             * |d_perp| for the body (not the kinky arc-length derivative) keeps
             * it seam-free across straight/curved junctions.  The two pieces meet
             * continuously: at phase == on_len the fragment projects onto the
             * endpoint, so both distances equal |d_perp|.
             */
            float d_perp  = d - stroke_center;
            float half_w  = ngli_outline_width * 0.5;

            if (abs(d_perp) > half_w + aa) {
                /*
                 * Off the stroke band: every centerline point is at least
                 * |d_perp| away, so cap_sdf > aa and no cap can reach here.
                 * Skip the ngli_perim_point evaluations — the bulk of the quad
                 * (interior and exterior of the ring) lands in this case.
                 */
                dash_on = 0.0;
            } else {
                float dist;
                if (phase <= on_len) {
                    dist = abs(d_perp);
                } else {
                    float s_start = t - phase;
                    vec2 ce = ngli_perim_point(s_start + on_len, Wr, Hr, r_perim, arc_len);
                    vec2 cs = ngli_perim_point(s_start + period, Wr, Hr, r_perim, arc_len);
                    dist = min(length(pos - ce), length(pos - cs));
                }

                /*
                 * cap_sdf is a unit-gradient distance, so anti-alias it with the
                 * stable screen-space pixel size (aa) rather than fwidth(cap_sdf):
                 * the latter spikes along the distance field's medial axis (a
                 * sharp corner's diagonal, or the bisector between the two end
                 * disks) and would etch a faint seam there.
                 */
                float cap_sdf = dist - half_w;
                dash_on = 1.0 - smoothstep(-aa, aa, cap_sdf);
            }
        } else {
            /* Square cap: extend dash by half the stroke width on each end */
            float half_w = ngli_outline_width * 0.5;
            float daa    = min(length(vec2(dFdx(t), dFdy(t))) * 0.5, 2.0);
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
     * range and must not bleed color into the outline AA transition.
     */
    vec4 tex_color = vec4(0.0);
    if (all(greaterThanEqual(ngli_uv, vec2(0.0))) && all(lessThanEqual(ngli_uv, vec2(1.0)))) {
        tex_color = ngli_color(content_uv, content_tex_coord);
    }

    /*
     * Premultiplied-alpha compositing: stroke over fill.
     * Blending uses ONE, ONE_MINUS_SRC_ALPHA so the output rgb must be
     * premultiplied (rgb *= alpha).  Using straight-alpha mixing would leave
     * non-zero rgb where alpha==0, causing fill color to bleed into
     * transparent corner pixels (outside the rounded shape).
     */
    float fill_rgb_scale = (ngli_fill_premult != 0 ? tex_color.a : 1.0)
                         * fill_alpha * ngli_fill_opacity;
    float fill_a = tex_color.a * fill_alpha * ngli_fill_opacity;
    ngl_out_color.rgb = stroke_col.rgb * ol_alpha + tex_color.rgb * fill_rgb_scale * (1.0 - ol_alpha);
    ngl_out_color.a   = ol_alpha + fill_a * (1.0 - ol_alpha);

    /* Global opacity and anti-aliased cascaded clip coverage */
    ngl_out_color *= ngli_opacity * clip_cov;
}
