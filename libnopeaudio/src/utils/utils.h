/*
 * Copyright 2024-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef NAU_UTILS_H
#define NAU_UTILS_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "config.h"

#include "nopeaudio/nopeaudio.h"

#define NAU_PI_F64 3.14159265358979323846
#define NAU_HALF_PI_F32 1.57079632679489661923f

/* Exact floating-point upper bound for int64_t conversions (2^63, exclusive). */
#define NAU_INT64_MAX_EXCLUSIVE 0x1p63L

#if defined(__GNUC__) || defined(__clang__)
# ifdef TARGET_MINGW_W64
#  define nau_printf_format(fmtpos, attrpos) __attribute__((__format__(__MINGW_PRINTF_FORMAT, fmtpos, attrpos)))
# else
#  define nau_printf_format(fmtpos, attrpos) __attribute__((__format__(__printf__, fmtpos, attrpos)))
# endif
#else
# define nau_printf_format(fmtpos, attrpos)
#endif

#if defined(__GNUC__) || defined(__clang__)
# define nau_unused __attribute__((unused))
# define nau_may_alias __attribute__((may_alias))
#else
# define nau_unused
# define nau_may_alias
#endif

#define nau_assert(cond) \
    ((void)((cond) || (fprintf(stderr, "Assert %s @ %s:%d\n", \
                               #cond, __FILE__, __LINE__),    \
                       abort(), 0)))

#define NAU_STATIC_ASSERT(c, id) static_assert(c, id)

#define NAU_FIELD_SIZEOF(name, field) (sizeof(((name *)0)->field))

#define NAU_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NAU_MAX(a, b) ((a) > (b) ? (a) : (b))
#define NAU_CLAMP(x, min, max) NAU_MAX(NAU_MIN(x, max), min)

#define NAU_ARRAY_MEMDUP(dst, src, name, count_field) do {                                  \
    const size_t nau_array_count_ = (src)->count_field;                                     \
    if (nau_array_count_ > 0) {                                                             \
        (dst)->name = nau_try_memdup((src)->name, nau_array_count_ * sizeof(*(dst)->name)); \
        if (!(dst)->name)                                                                   \
            return NAU_ERROR_MEMORY;                                                        \
    } else {                                                                                \
        (dst)->name = NULL;                                                                 \
    }                                                                                       \
    (dst)->count_field = nau_array_count_;                                                  \
} while (0)

#define NAU_ARRAY_COUNT(x) (sizeof(x)/sizeof(*(x)))

#define NAU_SWAP(a, b) do {                \
    __typeof__(a) nau_swap_tmp_ = (b);     \
    (b) = (a);                              \
    (a) = nau_swap_tmp_;                   \
} while (0)

#define NAU_ALIGN_MASK(v, mask) (((v) + (mask)) & ~(mask))
#define NAU_ALIGN(v, a) NAU_ALIGN_MASK(v, (__typeof__(v))(a) - 1)
#define NAU_ALIGN_VAL 16

#define NAU_IS_ALIGNED(v, a) (((v) & ((__typeof__(v))(a) - 1)) == 0)

#define NAU_ALIGNMENT(v) ((v) & ~((v) - 1))

#define NAU_IS_POW2(v) ((v) && ((v) & ((v) - 1)) == 0)

#define NAU_ATTR_ALIGNED _Alignas(NAU_ALIGN_VAL)

#define NAU_ALIGNED_VEC(vname) float NAU_ATTR_ALIGNED vname[4]
#define NAU_ALIGNED_MAT(mname) float NAU_ATTR_ALIGNED mname[4*4]

/* Format printf helpers */
#define NAU_FMT_F    "%12g"
#define NAU_FMT_VEC2 NAU_FMT_F " " NAU_FMT_F
#define NAU_FMT_VEC3 NAU_FMT_F " " NAU_FMT_VEC2
#define NAU_FMT_VEC4 NAU_FMT_F " " NAU_FMT_VEC3

#define NAU_FMT_MAT2 NAU_FMT_VEC2 "\n" \
                      NAU_FMT_VEC2
#define NAU_FMT_MAT3 NAU_FMT_VEC3 "\n" \
                      NAU_FMT_VEC3 "\n" \
                      NAU_FMT_VEC3
#define NAU_FMT_MAT4 NAU_FMT_VEC4 "\n" \
                      NAU_FMT_VEC4 "\n" \
                      NAU_FMT_VEC4 "\n" \
                      NAU_FMT_VEC4

/* Argument helpers associated with formats declared above */
#define NAU_ARG_F(v)    *(v)
#define NAU_ARG_VEC2(v) NAU_ARG_F(v), NAU_ARG_F((v)+1)
#define NAU_ARG_VEC3(v) NAU_ARG_F(v), NAU_ARG_VEC2((v)+1)
#define NAU_ARG_VEC4(v) NAU_ARG_F(v), NAU_ARG_VEC3((v)+1)

#define NAU_ARG_MAT2(v) NAU_ARG_VEC2((v)+2*0),    \
                         NAU_ARG_VEC2((v)+2*1)
#define NAU_ARG_MAT3(v) NAU_ARG_VEC3((v)+3*0),    \
                         NAU_ARG_VEC3((v)+3*1),    \
                         NAU_ARG_VEC3((v)+3*2)
#define NAU_ARG_MAT4(v) NAU_ARG_VEC4((v)+4*0),    \
                         NAU_ARG_VEC4((v)+4*1),    \
                         NAU_ARG_VEC4((v)+4*2),    \
                         NAU_ARG_VEC4((v)+4*3)

#define NAU_HAS_ALL_FLAGS(a, b) (((a) & (b)) == (b))

typedef void (*nau_user_free_func_type)(void *user_arg, void *data);

#endif /* UTILS_H */
