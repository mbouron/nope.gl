/*
 * Copyright 2023-2025 Clément Bœsch <u pkh.me>
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

#include helper_misc_utils.glsl

const float eps = 1e-4;

bool isfinite(highp float x) { return (floatBitsToUint(x) & 0x7f800000u) != 0x7f800000u; }

#define LARGE_FLOAT 1e38

struct Roots {
    uint count;
    float values[5];
};

float bisect5(float a, float b, float c, float d, float e, float f, vec2 t, vec2 v) {
    float x = (t.x+t.y) * .5;
    float s = v.x < v.y ? 1. : -1.; // sign flip
    for (int i = 0; i < 32; i++) {
        // Evaluate polynomial (y) and its derivative (q) using Horner's method in one pass
        float y = a*x + b, q = a*x + y;
              y = y*x + c; q = q*x + y;
              y = y*x + d; q = q*x + y;
              y = y*x + e; q = q*x + y;
              y = y*x + f;

        t = s*y < 0. ? vec2(x, t.y) : vec2(t.x, x);
        float next = x - y/q; // Newton iteration
        next = next >= t.x && next <= t.y ? next : (t.x+t.y) * .5;
        if (abs(next - x) < eps)
            return next;
        x = next;
    }
    return x;
}

float poly5(float a, float b, float c, float d, float e, float f, float t) {
     return ((((a * t + b) * t + c) * t + d) * t + e) * t + f;
}

// Generic quintic: f(x)=ax⁵+bx⁴+cx³+dx²+ex+f
Roots p5roots(Roots r4, float a, float b, float c, float d, float e, float f) {
    vec2 p = vec2(0, poly5(a,b,c,d,e,f, 0.));
    Roots r; r.count = 0u;
    for (uint i = 0u; i <= r4.count; i++) {
        float x = i == r4.count ? 1. : r4.values[i],
              y = poly5(a,b,c,d,e,f, x);
        if (p.y * y > 0.) continue;
        float v = bisect5(a,b,c,d,e,f,vec2(p.x,x),vec2(p.y,y));
        r.values[r.count++] = v;
        p = vec2(x, y);
    }
    return r;
}

// Quadratic: f(x)=ax²+bx+c
Roots p2roots(float a, float b, float c) {
    Roots r; r.count = 0u;
    float d = b*b - 4.*a*c;
    if (d < 0.)
        return r;
    if (d == 0.) {
        float rz = -.5 * b / a;
        if (isfinite(rz)) r.values[r.count++] = rz;
        return r;
    }
    float h = sqrt(d);
    float q = -.5 * (b + (b > 0. ? h : -h));
    vec2 v = vec2(q/a, c/q);
    if (v.x > v.y) v.xy = v.yx; // keep them ordered
    if (isfinite(v.x) && v.x >= 0. && v.x <= 1.) r.values[r.count++] = v.x;
    if (isfinite(v.y) && v.y >= 0. && v.y <= 1.) r.values[r.count++] = v.y;
    return r;
}

Roots root_find5(float a, float b, float c, float d, float e, float f) {
    Roots r = p2roots(   10.*a, 4.*b,   c);            // degree 2
    r = p5roots(r,0.,0., 10.*a, 6.*b,3.*c,   d);       // degree 3
    r = p5roots(r,   0.,  5.*a, 4.*b,3.*c, d+d, e);    // degree 4
    r = p5roots(r,           a,    b,   c,   d, e, f); // degree 5
    return r;
}

/* TODO: can we reduce the number of polynomial to evaluate for a given pixel? */
vec4 get_color(vec2 p)
{
    float dist = LARGE_FLOAT;
    int winding_number = 0;
    float area = 0.f;

    int base = 0;
    for (int j = 0; j < beziergroup_count; j++) {

        /*
         * Process a group of polynomials, or sub-shape
         */
        int bezier_count = abs(bezier_counts[j]);
        bool closed = bezier_counts[j] < 0;

        int shape_winding_number = 0;
        float shape_min_dist = LARGE_FLOAT;

        float shape_area = 0.f;

        for (int i = 0; i < bezier_count; i++) {
            vec4 bezier_x = bezier_x_buf[base + i];
            vec4 bezier_y = bezier_y_buf[base + i];
            vec2 p0 = vec2(bezier_x.x, bezier_y.x); // start point
            vec2 p1 = vec2(bezier_x.y, bezier_y.y); // control point 1
            vec2 p2 = vec2(bezier_x.z, bezier_y.z); // control point 2
            vec2 p3 = vec2(bezier_x.w, bezier_y.w); // end point

            shape_area += (p1.x - p0.x) * (p1.y + p0.y);
            shape_area += (p2.x - p1.x) * (p2.y + p1.y);
            shape_area += (p3.x - p2.x) * (p3.y + p2.y);

            /* Bezier cubic points to polynomial coefficients */
            vec2 a = -p0 + 3.0*(p1 - p2) + p3;
            vec2 b = 3.0 * (p0 - 2.0*p1 + p2);
            vec2 c = 3.0 * (p1 - p0);
            vec2 d = p0;

            /* Get smallest distance to current point */
            if (shape_min_dist > 0.0) {
                /*
                 * Calculate coefficients for the derivative D'(t) (degree 5) of D(t)
                 * where D(t) is the distance squared
                 * See https://stackoverflow.com/questions/2742610/closest-point-on-a-cubic-bezier-curve/57315396#57315396
                 * The coefficient are also divided by 2 to simplify the
                 * expression since D'(t)=0 is equivalent to D'(t)/2=0.
                 */
                vec2 dmp = d - p;
                float da = 3.0 * dot(a, a);
                float db = 5.0 * dot(a, b);
                float dc = 4.0 * dot(a, c) + 2.0 * dot(b, b);
                float dd = 3.0 * (dot(a, dmp) + dot(b, c));
                float de = 2.0 * dot(b, dmp) + dot(c, c);
                float df = dot(c, dmp);

                Roots roots_dt = root_find5(da, db, dc, dd, de, df);
                for (uint r = 0u; r < roots_dt.count; r++) {
                    float t = roots_dt.values[r];
                    if (t < 0.0 || t > 1.0) /* ignore out of bounds roots */
                        continue;

                    vec2 pr = ((a * t + b) * t + c) * t + d;
                    vec2 dp = p - pr;
                    shape_min_dist = min(shape_min_dist, dot(dp, dp));
                }

                /* Also include points at t=0 and t=1 */
                vec2 dp0 = p - p0;
                vec2 dp3 = p - p3;
                float mdp = min(dot(dp0, dp0), dot(dp3, dp3));
                shape_min_dist = min(shape_min_dist, mdp);
            }

            /* Winding number */
            if (closed) {
                int signs = int(p0.y < p.y)
                          | int(p1.y < p.y) << 1
                          | int(p2.y < p.y) << 2
                          | int(p3.y < p.y) << 3;
                int inc = (0x2AAA >> signs & 1) == 0 ? 1 : -1;
                Roots r5 = root_find5(0., 0., a.y, b.y, c.y, d.y - p.y);
                vec3 t = vec3(r5.values[0], r5.values[1], r5.values[2]);
                vec3 px = ((a.x*t + b.x)*t + c.x)*t + d.x;
                if (r5.count > 0u && px.x > p.x) shape_winding_number += inc;
                if (r5.count > 1u && px.y > p.x) shape_winding_number -= inc;
                if (r5.count > 2u && px.z > p.x) shape_winding_number += inc;
            }
        }

        bool orientation_flip = sign(area) != sign(shape_area);
        bool cur_in = winding_number != 0;
        bool shape_in = shape_winding_number != 0;
        bool sign_xchg = (int(cur_in) ^ int(shape_in)) != 0;

        winding_number += shape_winding_number;
        bool new_in = winding_number != 0;

        if (((cur_in && shape_in) || (area != 0.0 && !orientation_flip && sign_xchg)) && new_in) {
            shape_min_dist = (shape_in ? 1.0 : -1.0) * sqrt(shape_min_dist);
            dist = max(dist, shape_min_dist); // union
        } else {
            shape_min_dist = sqrt(shape_min_dist);
            dist = (new_in ? 1.0 : -1.0) * min(abs(dist), shape_min_dist);
        }

        area += shape_area;
        base += bezier_count;
    }

    /* Negative means outside, positive means inside */
    return vec4(vec3(dist), 1.0);
}

void main()
{
    vec2 pos = mix(coords.xy, coords.zw, uv);
    ngl_out_color = get_color(pos);
}
