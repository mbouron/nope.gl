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

float poly5(float a, float b, float c, float d, float e, float f, float t) {
     return ((((a * t + b) * t + c) * t + d) * t + e) * t + f;
}

// Newton bisection
//
// a,b,c,d,e,f: 5th degree polynomial parameters
// t: x-axis boundaries
// v: respectively f(t.x) and f(t.y)
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

// Quintic: solve ax⁵+bx⁴+cx³+dx²+ex+f=0
int cy_find5(out float r[5], float r4[5], int n, float a, float b, float c, float d, float e, float f) {
    int count = 0;
    vec2 p = vec2(0, poly5(a,b,c,d,e,f, 0.));
    for (int i = 0; i <= n; i++) {
        float x = i == n ? 1. : r4[i],
              y = poly5(a,b,c,d,e,f, x);
        if (p.y * y > 0.)
            continue;
        float v = bisect5(a,b,c,d,e,f,vec2(p.x,x),vec2(p.y,y));
        r[count++] = v;
        p = vec2(x, y);
    }
    return count;
}

// Quadratic: solve ax²+bx+c=0 (clamped to [0,1] and ordered)
int root_find2(out float r[5], float a, float b, float c) {
    int count = 0;
    float d = b*b - 4.*a*c;
    if (d < 0.)
        return count;
    if (d == 0.) {
        float s = -.5 * b / a;
        if (isfinite(s))
            r[count++] = s;
        return count;
    }
    float h = sqrt(d);
    float q = -.5 * (b + (b > 0. ? h : -h));
    vec2 v = vec2(q/a, c/q);
    if (v.x > v.y) v.xy = v.yx; // keep them ordered
    if (isfinite(v.x) && v.x >= 0. && v.x <= 1.) r[count++] = v.x;
    if (isfinite(v.y) && v.y >= 0. && v.y <= 1.) r[count++] = v.y;
    return count;
}

int root_find3(out float r[5], float a, float b, float c, float d) {
    float r2[5];
    int n = root_find2(r2, 3.*a, b+b, c);
    return cy_find5(r, r2, n, 0., 0., a, b, c, d);
}

int root_find5(out float r[5], float a, float b, float c, float d, float e, float f) {
    float r2[5], r3[5], r4[5];
    int n = root_find2(r2,          10.*a, 4.*b,    c);            // degree 2
    n = cy_find5(r3, r2, n, 0. ,0., 10.*a, 6.*b, 3.*c,   d);       // degree 3
    n = cy_find5(r4, r3, n,     0.,  5.*a, 4.*b, 3.*c, d+d, e);    // degree 4
    n = cy_find5(r,  r4, n,             a,    b,    c,   d, e, f); // degree 5
    return n;
}

float bezier_sq(vec2 p, vec2 p0, vec2 p1, vec2 p2, vec2 p3) {
    // Start by testing the distance to the boundary points at t=0 (p0) and t=1 (p3)
    vec2 dp0 = p0 - p,
         dp3 = p3 - p;
    float dist = min(dot(dp0, dp0), dot(dp3, dp3));

    // Bezier cubic points to polynomial coefficients
    vec2 a = -p0 + 3.0*(p1 - p2) + p3,
         b = 3.0 * (p0 - 2.0*p1 + p2),
         c = 3.0 * (p1 - p0),
         d = p0;

    // Solve D'(t)=0 where D(t) is the distance squared
    vec2 dmp = d - p;
    float da = 3.0 * dot(a, a),
          db = 5.0 * dot(a, b),
          dc = 4.0 * dot(a, c) + 2.0 * dot(b, b),
          dd = 3.0 * (dot(a, dmp) + dot(b, c)),
          de = 2.0 * dot(b, dmp) + dot(c, c),
          df = dot(c, dmp);

    float r[5];
    int count = root_find5(r, da, db, dc, dd, de, df);
    for (int i = 0; i < count; i++) {
        float t = r[i];
        vec2 dp = ((a * t + b) * t + c) * t + dmp;
        dist = min(dist, dot(dp, dp));
    }

    return dist;
}

int bezier_winding(vec2 p, vec2 p0, vec2 p1, vec2 p2, vec2 p3) {
    int w = 0;
    int signs = int(p0.y < p.y)
              | int(p1.y < p.y) << 1
              | int(p2.y < p.y) << 2
              | int(p3.y < p.y) << 3;
    if (signs == 0 || signs == 15)
        return 0;
    int inc = (0x2AAA >> signs & 1) == 0 ? 1 : -1;
    vec2 a = -p0 + 3.*(p1 - p2) + p3,
         b = 3. * (p0 - 2.*p1 + p2),
         c = 3. * (p1 - p0),
         d = p0 - p;
    float r[5];
    int count = root_find3(r, a.y, b.y, c.y, d.y);
    vec3 t = vec3(r[0], r[1], r[2]);
    vec3 v = ((a.x*t + b.x)*t + c.x)*t + d.x;
    if (count > 0 && v.x >= 0.) w += inc;
    if (count > 1 && v.y >= 0.) w -= inc;
    if (count > 2 && v.z >= 0.) w += inc;
    return w;
}

struct Bezier {
    vec2 p0; // start point
    vec2 p1; // control point 1
    vec2 p2; // control point 2
    vec2 p3; // end point
};

Bezier get_bezier(int i) {
    vec4 bezier_x = bezier_x_buf[i];
    vec4 bezier_y = bezier_y_buf[i];
    vec2 p0 = vec2(bezier_x.x, bezier_y.x);
    vec2 p1 = vec2(bezier_x.y, bezier_y.y);
    vec2 p2 = vec2(bezier_x.z, bezier_y.z);
    vec2 p3 = vec2(bezier_x.w, bezier_y.w);
    return Bezier(p0, p1, p2, p3);
}

vec4 get_color(vec2 p) {
    int w = 0;
    int base = 0;
    float dist = 1e38;

    for (int j = 0; j < beziergroup_count; j++) {
        int count = abs(bezier_counts[j]);
        bool closed = bezier_counts[j] < 0;

        // Find a good initial guess
        int best = 0;
        float boxd = 1e38;
        for (int i = 0; i < count; i++) {
            Bezier b = get_bezier(base + i);
            vec2 p0=b.p0, p1=b.p1, p2=b.p2, p3=b.p3;
            vec2 q0 = min(p0, min(p1, min(p2, p3)));
            vec2 q1 = max(p0, max(p1, max(p2, p3)));
            vec2 v = (q0+q1)*.5 - p;
            float h = dot(v,v);
            if (h < boxd)
                best=i, boxd=h;
        }

        // Initial guess
        Bezier bb = get_bezier(base + best);
        dist = min(dist, bezier_sq(p, bb.p0, bb.p1, bb.p2, bb.p3));

        for (int i = 0; i < count; i++) {
            if (i == best) // We already computed this one
                continue;

            Bezier b = get_bezier(base + i);
            vec2 p0=b.p0, p1=b.p1, p2=b.p2, p3=b.p3;

            // Distance to box (0 if inside), squared
            vec2 q0 = min(p0, min(p1, min(p2, p3)));
            vec2 q1 = max(p0, max(p1, max(p2, p3)));
            vec2 v = max(abs(q0+q1-p-p)-q1+q0, 0.)*.5;
            float h = dot(v,v);

            // We can't get a shorter distance than h if we were to compute the
            // distance to that curve
            if (h > dist)
                continue;

            float d = bezier_sq(p, p0, p1, p2, p3);
            dist = min(dist, d);
        }

        // Get the sign of the distance
        if (closed) {
            for (int i = 0; i < count; i++) {
                Bezier b = get_bezier(base + i);
                w += bezier_winding(p, b.p0, b.p1, b.p2, b.p3);
            }
        }

        base += count;
    }

    /* Negative means outside, positive means inside */
    dist = (w != 0 ? 1. : -1.) * sqrt(dist);

    return vec4(vec3(dist), 1.0);
}

void main()
{
    vec2 pos = mix(coords.xy, coords.zw, uv);
    ngl_out_color = get_color(pos);
}
