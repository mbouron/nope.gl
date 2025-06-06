/*
 * Copyright 2016-2022 GoPro Inc.
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

#include <string.h>
#include <math.h>

#include "math_utils.h"
#include "utils/utils.h"

static const float zero_vec[4];

#define DECLARE_BASE_VEC_FUNCS(n)                                       \
void ngli_vec##n##_add(float *dst, const float *v1, const float *v2)    \
{                                                                       \
    const float r[] = NGLI_VEC##n##_ADD(v1, v2);                        \
    memcpy(dst, r, sizeof(r));                                          \
}                                                                       \
                                                                        \
void ngli_vec##n##_sub(float *dst, const float *v1, const float *v2)    \
{                                                                       \
    const float r[] = NGLI_VEC##n##_SUB(v1, v2);                        \
    memcpy(dst, r, sizeof(r));                                          \
}                                                                       \
                                                                        \
void ngli_vec##n##_neg(float *dst, const float *v)                      \
{                                                                       \
    const float r[] = NGLI_VEC##n##_NEG(v);                             \
    memcpy(dst, r, sizeof(r));                                          \
}                                                                       \
                                                                        \
void ngli_vec##n##_scale(float *dst, const float *v, float s)           \
{                                                                       \
    const float r[] = NGLI_VEC##n##_SCALE(v, s);                        \
    memcpy(dst, r, sizeof(r));                                          \
}                                                                       \
                                                                        \
void ngli_vec##n##_norm(float *dst, const float *v)                     \
{                                                                       \
    if (ngli_vec##n##_is_zero(v)) {                                     \
        memset(dst, 0, n * sizeof(*v));                                 \
        return;                                                         \
    }                                                                   \
    const float l = 1.0f / ngli_vec##n##_length(v);                     \
    const float r[] = NGLI_VEC##n##_SCALE(v, l);                        \
    memcpy(dst, r, sizeof(r));                                          \
}                                                                       \
                                                                        \
int ngli_vec##n##_is_zero(const float *v)                               \
{                                                                       \
    return !memcmp(v, zero_vec, n * sizeof(*v));                        \
}                                                                       \
                                                                        \
void ngli_vec##n##_abs(float *dst, const float *v)                      \
{                                                                       \
    const float r[] = NGLI_VEC##n##_ABS(v);                             \
    memcpy(dst, r, sizeof(r));                                          \
}                                                                       \
                                                                        \
float ngli_vec##n##_dot(const float *v1, const float *v2)               \
{                                                                       \
    return NGLI_VEC##n##_DOT(v1, v2);                                   \
}                                                                       \
                                                                        \
float ngli_vec##n##_length(const float *v)                              \
{                                                                       \
    return NGLI_VEC##n##_LENGTH(v);                                     \
}                                                                       \
                                                                        \
void ngli_vec##n##_mul(float *dst, const float *v1, const float *v2)    \
{                                                                       \
    const float r[] = NGLI_VEC##n##_MUL(v1, v2);                        \
    memcpy(dst, r, sizeof(r));                                          \
}                                                                       \

DECLARE_BASE_VEC_FUNCS(2)
DECLARE_BASE_VEC_FUNCS(3)
DECLARE_BASE_VEC_FUNCS(4)

void ngli_vec2_init(float *dst, float x, float y)
{
    const float r[] = {x, y};
    memcpy(dst, r, sizeof(r));
}

void ngli_vec3_cross(float *dst, const float *v1, const float *v2)
{
    float v[3];

    v[0] = v1[1]*v2[2] - v1[2]*v2[1];
    v[1] = v1[2]*v2[0] - v1[0]*v2[2];
    v[2] = v1[0]*v2[1] - v1[1]*v2[0];

    memcpy(dst, v, sizeof(v));
}

void ngli_vec3_normalvec(float *dst, const float *a, const float *b, const float *c)
{
    float d[3] = NGLI_VEC3_SUB(b, a);
    float e[3] = NGLI_VEC3_SUB(c, a);

    ngli_vec3_cross(dst, d, e);
    ngli_vec3_norm(dst, dst);
}

void ngli_vec3_init(float *dst, float x, float y, float z)
{
    const float r[] = {x, y, z};
    memcpy(dst, r, sizeof(r));
}

void ngli_vec4_lerp(float *dst, const float *v1, const float *v2, float c)
{
    const NGLI_ALIGNED_VEC(a) = {NGLI_ARG_VEC4(v1)};
    const NGLI_ALIGNED_VEC(b) = {NGLI_ARG_VEC4(v2)};
    dst[0] = NGLI_MIX_F32(a[0], b[0], c);
    dst[1] = NGLI_MIX_F32(a[1], b[1], c);
    dst[2] = NGLI_MIX_F32(a[2], b[2], c);
    dst[3] = NGLI_MIX_F32(a[3], b[3], c);
}

void ngli_vec4_perspective_div(float *dst, const float *v)
{
    const NGLI_ALIGNED_VEC(r) = NGLI_VEC4_SCALE(v, 1.f / v[3]);
    memcpy(dst, r, sizeof(r));
}

void ngli_vec4_init(float *dst, float x, float y, float z, float w)
{
    const NGLI_ALIGNED_VEC(r) = {x, y, z, w};
    memcpy(dst, r, sizeof(r));
}

void ngli_mat3_from_mat4(float *dst, const float *m)
{
    memcpy(dst,     m,     3 * sizeof(*m));
    memcpy(dst + 3, m + 4, 3 * sizeof(*m));
    memcpy(dst + 6, m + 8, 3 * sizeof(*m));
}

void ngli_mat3_mul_scalar(float *dst, const float *m, float s)
{
    const float tmp[3*3] = {
        m[0] * s, m[1] * s, m[2] * s,
        m[3] * s, m[4] * s, m[5] * s,
        m[6] * s, m[7] * s, m[8] * s,
    };
    memcpy(dst, tmp, sizeof(tmp));
}

void ngli_mat3_transpose(float *dst, const float *m)
{
    const float tmp[3*3] = {
        m[0], m[3], m[6],
        m[1], m[4], m[7],
        m[2], m[5], m[8],
    };
    memcpy(dst, tmp, sizeof(tmp));
}

float ngli_mat3_determinant(const float *m)
{
    return m[0]*m[4]*m[8] + m[1]*m[5]*m[6] + m[2]*m[3]*m[7]
         - m[2]*m[4]*m[6] - m[1]*m[3]*m[8] - m[0]*m[5]*m[7];
}

void ngli_mat3_adjugate(float *dst, const float *m)
{
    const float tmp[3*3] = {
        m[4]*m[8] - m[5]*m[7],
        m[2]*m[7] - m[1]*m[8],
        m[1]*m[5] - m[2]*m[4],
        m[5]*m[6] - m[3]*m[8],
        m[0]*m[8] - m[2]*m[6],
        m[2]*m[3] - m[0]*m[5],
        m[3]*m[7] - m[4]*m[6],
        m[1]*m[6] - m[0]*m[7],
        m[0]*m[4] - m[1]*m[3],
    };
    memcpy(dst, tmp, sizeof(tmp));
}

void ngli_mat3_inverse(float *dst, const float *m)
{
    float a[3*3];
    float det = ngli_mat3_determinant(m);

    if (det == 0.f) {
        memcpy(dst, m, 3 * 3 * sizeof(*m));
        return;
    }

    ngli_mat3_adjugate(a, m);
    ngli_mat3_mul_scalar(dst, a, 1.f / det);
}

float ngli_mat4_determinant(const float *m)
{
    const float x12 = m[ 8] * m[13] - m[ 9] * m[12];
    const float x13 = m[ 8] * m[14] - m[10] * m[12];
    const float x14 = m[ 8] * m[15] - m[11] * m[12];
    const float x15 = m[ 9] * m[14] - m[10] * m[13];
    const float x16 = m[ 9] * m[15] - m[11] * m[13];
    const float x17 = m[10] * m[15] - m[11] * m[14];

    const float det_p0 = m[5] * x17 - m[6] * x16 + m[7] * x15;
    const float det_p1 = m[4] * x17 - m[6] * x14 + m[7] * x13;
    const float det_p2 = m[4] * x16 - m[5] * x14 + m[7] * x12;
    const float det_p3 = m[4] * x15 - m[5] * x13 + m[6] * x12;

    return m[0] * det_p0 - m[1] * det_p1 + m[2] * det_p2 - m[3] * det_p3;
}

void ngli_mat4_inverse(float *dst, const float *m)
{
    const float x00 = m[ 4] * m[ 9] - m[ 5] * m[ 8];
    const float x01 = m[ 4] * m[13] - m[ 5] * m[12];
    const float x02 = m[ 4] * m[10] - m[ 6] * m[ 8];
    const float x03 = m[ 4] * m[11] - m[ 7] * m[ 8];
    const float x04 = m[ 4] * m[14] - m[ 6] * m[12];
    const float x05 = m[ 4] * m[15] - m[ 7] * m[12];
    const float x06 = m[ 5] * m[10] - m[ 6] * m[ 9];
    const float x07 = m[ 5] * m[11] - m[ 7] * m[ 9];
    const float x08 = m[ 6] * m[11] - m[ 7] * m[10];
    const float x09 = m[ 5] * m[14] - m[ 6] * m[13];
    const float x10 = m[ 5] * m[15] - m[ 7] * m[13];
    const float x11 = m[ 6] * m[15] - m[ 7] * m[14];
    const float x12 = m[ 8] * m[13] - m[ 9] * m[12];
    const float x13 = m[ 8] * m[14] - m[10] * m[12];
    const float x14 = m[ 8] * m[15] - m[11] * m[12];
    const float x15 = m[ 9] * m[14] - m[10] * m[13];
    const float x16 = m[ 9] * m[15] - m[11] * m[13];
    const float x17 = m[10] * m[15] - m[11] * m[14];

    const float det_p0 = m[5] * x17 - m[6] * x16 + m[7] * x15;
    const float det_p1 = m[4] * x17 - m[6] * x14 + m[7] * x13;
    const float det_p2 = m[4] * x16 - m[5] * x14 + m[7] * x12;
    const float det_p3 = m[4] * x15 - m[5] * x13 + m[6] * x12;

    float det = m[0] * det_p0 - m[1] * det_p1 + m[2] * det_p2 - m[3] * det_p3;
    if (det == 0.f) {
        memcpy(dst, m, 4 * 4 * sizeof(*dst));
        return;
    }
    det = 1.f / det;

    const NGLI_ALIGNED_MAT(tmp) = {
         det * det_p0,
        -det * (m[1] * x17 - m[2] * x16 + m[3] * x15),
         det * (m[1] * x11 - m[2] * x10 + m[3] * x09),
        -det * (m[1] * x08 - m[2] * x07 + m[3] * x06),
        -det * det_p1,
         det * (m[0] * x17 - m[2] * x14 + m[3] * x13),
        -det * (m[0] * x11 - m[2] * x05 + m[3] * x04),
         det * (m[0] * x08 - m[2] * x03 + m[3] * x02),
         det * det_p2,
        -det * (m[0] * x16 - m[1] * x14 + m[3] * x12),
         det * (m[0] * x10 - m[1] * x05 + m[3] * x01),
        -det * (m[0] * x07 - m[1] * x03 + m[3] * x00),
        -det * det_p3,
         det * (m[0] * x15 - m[1] * x13 + m[2] * x12),
        -det * (m[0] * x09 - m[1] * x04 + m[2] * x01),
         det * (m[0] * x06 - m[1] * x02 + m[2] * x00),
    };

    memcpy(dst, tmp, sizeof(tmp));
}

void ngli_mat4_mul_c(float *dst, const float *m1, const float *m2)
{
    NGLI_ALIGNED_MAT(m);

    m[ 0] = m1[0]*m2[ 0] + m1[4]*m2[ 1] + m1[ 8]*m2[ 2] + m1[12]*m2[ 3];
    m[ 1] = m1[1]*m2[ 0] + m1[5]*m2[ 1] + m1[ 9]*m2[ 2] + m1[13]*m2[ 3];
    m[ 2] = m1[2]*m2[ 0] + m1[6]*m2[ 1] + m1[10]*m2[ 2] + m1[14]*m2[ 3];
    m[ 3] = m1[3]*m2[ 0] + m1[7]*m2[ 1] + m1[11]*m2[ 2] + m1[15]*m2[ 3];

    m[ 4] = m1[0]*m2[ 4] + m1[4]*m2[ 5] + m1[ 8]*m2[ 6] + m1[12]*m2[ 7];
    m[ 5] = m1[1]*m2[ 4] + m1[5]*m2[ 5] + m1[ 9]*m2[ 6] + m1[13]*m2[ 7];
    m[ 6] = m1[2]*m2[ 4] + m1[6]*m2[ 5] + m1[10]*m2[ 6] + m1[14]*m2[ 7];
    m[ 7] = m1[3]*m2[ 4] + m1[7]*m2[ 5] + m1[11]*m2[ 6] + m1[15]*m2[ 7];

    m[ 8] = m1[0]*m2[ 8] + m1[4]*m2[ 9] + m1[ 8]*m2[10] + m1[12]*m2[11];
    m[ 9] = m1[1]*m2[ 8] + m1[5]*m2[ 9] + m1[ 9]*m2[10] + m1[13]*m2[11];
    m[10] = m1[2]*m2[ 8] + m1[6]*m2[ 9] + m1[10]*m2[10] + m1[14]*m2[11];
    m[11] = m1[3]*m2[ 8] + m1[7]*m2[ 9] + m1[11]*m2[10] + m1[15]*m2[11];

    m[12] = m1[0]*m2[12] + m1[4]*m2[13] + m1[ 8]*m2[14] + m1[12]*m2[15];
    m[13] = m1[1]*m2[12] + m1[5]*m2[13] + m1[ 9]*m2[14] + m1[13]*m2[15];
    m[14] = m1[2]*m2[12] + m1[6]*m2[13] + m1[10]*m2[14] + m1[14]*m2[15];
    m[15] = m1[3]*m2[12] + m1[7]*m2[13] + m1[11]*m2[14] + m1[15]*m2[15];

    memcpy(dst, m, sizeof(m));
}

void ngli_mat4_mul_vec4_c(float *dst, const float *m, const float *v)
{
    NGLI_ALIGNED_VEC(tmp);

    tmp[0] = m[ 0]*v[0] + m[ 4]*v[1] + m[ 8]*v[2] + m[12]*v[3];
    tmp[1] = m[ 1]*v[0] + m[ 5]*v[1] + m[ 9]*v[2] + m[13]*v[3];
    tmp[2] = m[ 2]*v[0] + m[ 6]*v[1] + m[10]*v[2] + m[14]*v[3];
    tmp[3] = m[ 3]*v[0] + m[ 7]*v[1] + m[11]*v[2] + m[15]*v[3];

    memcpy(dst, tmp, sizeof(tmp));
}

void ngli_mat4_look_at(float * restrict dst, float *eye, float *center, float *up)
{
    float f[3] = NGLI_VEC3_SUB(center, eye);
    float s[3];
    float u[3];

    ngli_vec3_norm(f, f);

    ngli_vec3_cross(s, f, up);
    ngli_vec3_norm(s, s);

    ngli_vec3_cross(u, s, f);

    dst[ 0] =  s[0];
    dst[ 1] =  u[0];
    dst[ 2] = -f[0];
    dst[ 3] =  0.f;

    dst[ 4] =  s[1];
    dst[ 5] =  u[1];
    dst[ 6] = -f[1];
    dst[ 7] =  0.f;

    dst[ 8] =  s[2];
    dst[ 9] =  u[2];
    dst[10] = -f[2];
    dst[11] =  0.f;

    dst[12] = -ngli_vec3_dot(s, eye);
    dst[13] = -ngli_vec3_dot(u, eye);
    dst[14] =  ngli_vec3_dot(f, eye);
    dst[15] =  1.f;
}

void ngli_mat4_identity(float *dst)
{
    static const NGLI_ALIGNED_MAT(id) = NGLI_MAT4_IDENTITY;
    memcpy(dst, id, sizeof(id));
}

void ngli_mat4_orthographic(float * restrict dst, float left, float right,
                            float bottom, float top, float near, float far)
{
    const float dx = right - left;
    const float dy = top - bottom;
    const float dz = far - near;

    ngli_mat4_identity(dst);

    if (dx == 0 || dy == 0 || dz == 0)
        return;

    const float tx = -(right + left) / dx;
    const float ty = -(top + bottom) / dy;
    const float tz = -(far + near)   / dz;

    dst[0 ] =  2 / dx;
    dst[5 ] =  2 / dy;
    dst[10] = -2 / dz;
    dst[12] = tx;
    dst[13] = ty;
    dst[14] = tz;
}

void ngli_mat4_perspective(float * restrict dst, float fov, float aspect, float near, float far)
{
    const float r = fov / 2.f * PI_F32 / 180.0f;
    const float s = sinf(r);
    const float z = far - near;

    ngli_mat4_identity(dst);

    if (z == 0 || s == 0 || aspect == 0) {
        return;
    }

    const float c = cosf(r) / s;

    dst[ 0] =  c / aspect;
    dst[ 5] =  c;
    dst[10] = -(far + near) / z;
    dst[11] = -1.f;
    dst[14] = -2.f * near * far / z;
    dst[15] =  0.f;
}

void ngli_mat4_rotate(float * restrict dst, float angle, float *axis, const float *anchor)
{
    const float a = cosf(angle);
    const float b = sinf(angle);
    const float c = 1.0f - a;

    dst[ 0] = a + axis[0] * axis[0] * c;
    dst[ 1] = axis[0] * axis[1] * c + axis[2] * b;
    dst[ 2] = axis[0] * axis[2] * c - axis[1] * b;
    dst[ 3] = 0.0f;

    dst[ 4] = axis[0] * axis[1] * c - axis[2] * b;
    dst[ 5] = a + axis[1] * axis[1] * c;
    dst[ 6] = axis[1] * axis[2] * c + axis[0] * b;
    dst[ 7] = 0.0f;

    dst[ 8] = axis[0] * axis[2] * c + axis[1] * b;
    dst[ 9] = axis[1] * axis[2] * c - axis[0] * b;
    dst[10] = a + axis[2] * axis[2] * c;
    dst[11] = 0.0f;

    if (anchor) {
        dst[12] = anchor[0] - anchor[0]*dst[0] - anchor[1]*dst[4] - anchor[2]*dst[ 8];
        dst[13] = anchor[1] - anchor[0]*dst[1] - anchor[1]*dst[5] - anchor[2]*dst[ 9];
        dst[14] = anchor[2] - anchor[0]*dst[2] - anchor[1]*dst[6] - anchor[2]*dst[10];
    } else {
        dst[12] = 0.0f;
        dst[13] = 0.0f;
        dst[14] = 0.0f;
    }
    dst[15] = 1.0f;
}

void ngli_mat4_from_quat(float * restrict dst, const float *q, const float *anchor)
{
    float tmp[4];
    const float *tmpp = q;

    float length = ngli_vec4_length(q);
    if (length > 1.0) {
        ngli_vec4_norm(tmp, q);
        tmpp = tmp;
    }

    const float x2  = tmpp[0] + tmpp[0];
    const float y2  = tmpp[1] + tmpp[1];
    const float z2  = tmpp[2] + tmpp[2];

    const float yy2 = tmpp[1] * y2;
    const float xy2 = tmpp[0] * y2;
    const float xz2 = tmpp[0] * z2;
    const float yz2 = tmpp[1] * z2;
    const float zz2 = tmpp[2] * z2;
    const float wz2 = tmpp[3] * z2;
    const float wy2 = tmpp[3] * y2;
    const float wx2 = tmpp[3] * x2;
    const float xx2 = tmpp[0] * x2;

    dst[ 0] = -yy2 - zz2 + 1.0f;
    dst[ 1] =  xy2 + wz2;
    dst[ 2] =  xz2 - wy2;
    dst[ 3] =  0.0f;
    dst[ 4] =  xy2 - wz2;
    dst[ 5] = -xx2 - zz2 + 1.0f;
    dst[ 6] =  yz2 + wx2;
    dst[ 7] =  0.0f;
    dst[ 8] =  xz2 + wy2;
    dst[ 9] =  yz2 - wx2;
    dst[10] = -xx2 - yy2 + 1.0f;
    dst[11] =  0.0f;
    if (anchor) {
        dst[12] = anchor[0] - anchor[0]*dst[0] - anchor[1]*dst[4] - anchor[2]*dst[ 8];
        dst[13] = anchor[1] - anchor[0]*dst[1] - anchor[1]*dst[5] - anchor[2]*dst[ 9];
        dst[14] = anchor[2] - anchor[0]*dst[2] - anchor[1]*dst[6] - anchor[2]*dst[10];
    } else {
        dst[12] = 0.0f;
        dst[13] = 0.0f;
        dst[14] = 0.0f;
    }
    dst[15] =  1.0f;
}

void ngli_mat4_translate(float * restrict dst, float x, float y, float z)
{
    memset(dst, 0, 4 * 4 * sizeof(*dst));
    dst[ 0] = 1.0f;
    dst[ 5] = 1.0f;
    dst[10] = 1.0f;
    dst[12] = x;
    dst[13] = y;
    dst[14] = z;
    dst[15] = 1.0f;
}

void ngli_mat4_scale(float * restrict dst, float x, float y, float z, const float *anchor)
{
    memset(dst, 0, 4 * 4 * sizeof(*dst));
    dst[ 0] =  x;
    dst[ 5] =  y;
    dst[10] =  z;
    if (anchor) {
        dst[12] = anchor[0] - anchor[0]*x;
        dst[13] = anchor[1] - anchor[1]*y;
        dst[14] = anchor[2] - anchor[2]*z;
    }
    dst[15] =  1.0f;
}

void ngli_mat4_skew(float * restrict dst, float x, float y, float z, const float *axis, const float *anchor)
{
    memset(dst, 0, 4 * 4 * sizeof(*dst));
    dst[ 0] = 1.f;
    dst[ 1] = axis[1] * x;  // X skew on Y-axis
    dst[ 2] = axis[2] * x;  // X skew on Z-axis
    dst[ 4] = axis[0] * y;  // Y skew on X-axis
    dst[ 5] = 1.f;
    dst[ 6] = axis[2] * y;  // Y skew on Z-axis
    dst[ 8] = axis[0] * z;  // Z skew on X-axis
    dst[ 9] = axis[1] * z;  // Z skew on Y-axis
    dst[10] = 1.f;
    if (anchor) {
        dst[12] = -anchor[1]*dst[4] - anchor[2]*dst[8];
        dst[13] = -anchor[0]*dst[1] - anchor[2]*dst[9];
        dst[14] = -anchor[0]*dst[2] - anchor[1]*dst[6];
    }
    dst[15] = 1.f;
}

#define COS_ALPHA_THRESHOLD 0.9995f

void ngli_quat_slerp(float * restrict dst, const float *q1, const float *q2, float t)
{
    NGLI_ALIGNED_VEC(tmp_q1);
    const float *tmp_q1p = q1;

    float cos_alpha = ngli_vec4_dot(q1, q2);

    if (cos_alpha < 0.0f) {
        cos_alpha = -cos_alpha;
        ngli_vec4_neg(tmp_q1, q1);
        tmp_q1p = tmp_q1;
    }

    if (cos_alpha > COS_ALPHA_THRESHOLD) {
        ngli_vec4_lerp(dst, tmp_q1p, q2, t);
        ngli_vec4_norm(dst, dst);
        return;
    }

    cos_alpha = NGLI_MIN(cos_alpha, 1.0f);

    const float alpha = acosf(cos_alpha);
    const float theta = alpha * t;

    NGLI_ALIGNED_VEC(tmp);
    ngli_vec4_scale(tmp, tmp_q1p, cos_alpha);
    ngli_vec4_sub(tmp, q2, tmp);
    ngli_vec4_norm(tmp, tmp);

    NGLI_ALIGNED_VEC(tmp1);
    NGLI_ALIGNED_VEC(tmp2);
    ngli_vec4_scale(tmp1, tmp_q1p, cosf(theta));
    ngli_vec4_scale(tmp2, tmp, sinf(theta));
    ngli_vec4_add(dst, tmp1, tmp2);
}
