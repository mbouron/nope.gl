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

#ifndef NGPU_UTILS_H
#define NGPU_UTILS_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "config.h"

#include "ngpu/ngpu.h"

#ifdef __GNUC__
# ifdef TARGET_MINGW_W64
#  define ngpu_printf_format(fmtpos, attrpos) __attribute__((__format__(__MINGW_PRINTF_FORMAT, fmtpos, attrpos)))
# else
#  define ngpu_printf_format(fmtpos, attrpos) __attribute__((__format__(__printf__, fmtpos, attrpos)))
# endif
#else
# define ngpu_printf_format(fmtpos, attrpos)
#endif

#if defined(__GNUC__) || defined(__clang__)
# define ngpu_unused __attribute__((unused))
#else
# define ngpu_unused
#endif

#define ngpu_assert(cond) do {                          \
    if (!(cond)) {                                      \
        fprintf(stderr, "Assert %s @ %s:%d\n",          \
                #cond, __FILE__, __LINE__);             \
        abort();                                        \
    }                                                   \
} while (0)

#define NGPU_STATIC_ASSERT(c, id) static_assert(c, id)

#define NGPU_FIELD_SIZEOF(name, field) (sizeof(((name *)0)->field))

#define NGPU_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NGPU_MAX(a, b) ((a) > (b) ? (a) : (b))
#define NGPU_CLAMP(x, min, max) NGPU_MAX(NGPU_MIN(x, max), min)

#if HAVE_BUILTIN_OVERFLOW
#define NGPU_CHK_MUL(result, a, b) __builtin_mul_overflow(a, b, result)
#else
#define NGPU_CHK_MUL(result, a, b) (*(result) = (a) * (b), false)
#endif

#define NGPU_ARRAY_MEMDUP(dst, src, name) do {                             \
    const size_t nb = (src)->nb_##name;                                    \
    if (nb > 0) {                                                          \
        (dst)->name = ngpu_memdup((src)->name, nb * sizeof(*(dst)->name)); \
        if (!(dst)->name)                                                  \
            return NGPU_ERROR_MEMORY;                                       \
    } else {                                                               \
        (dst)->name = NULL;                                                \
    }                                                                      \
    (dst)->nb_##name = nb;                                                 \
} while (0)                                                                \

#define NGPU_ARRAY_NB(x) (sizeof(x)/sizeof(*(x)))
#define NGPU_SWAP(type, a, b) do { type tmp_swap = b; b = a; a = tmp_swap; } while (0)

#define NGPU_ALIGN_MASK(v, mask) (((v) + (mask)) & ~(mask))
#define NGPU_ALIGN(v, a) NGPU_ALIGN_MASK(v, (__typeof__(v))(a) - 1)
#define NGPU_ALIGN_VAL 16

#define NGPU_IS_ALIGNED(v, a) (((v) & ((__typeof__(v))(a) - 1)) == 0)

#define NGPU_ALIGNMENT(v) ((v) & ~((v) - 1))

#define NGPU_IS_POW2(v) ((v) && ((v) & ((v) - 1)) == 0)

#define NGPU_ATTR_ALIGNED _Alignas(NGPU_ALIGN_VAL)

#define NGPU_ALIGNED_VEC(vname) float NGPU_ATTR_ALIGNED vname[4]
#define NGPU_ALIGNED_MAT(mname) float NGPU_ATTR_ALIGNED mname[4*4]

/* Format printf helpers */
#define NGPU_FMT_F    "%12g"
#define NGPU_FMT_VEC2 NGPU_FMT_F " " NGPU_FMT_F
#define NGPU_FMT_VEC3 NGPU_FMT_F " " NGPU_FMT_VEC2
#define NGPU_FMT_VEC4 NGPU_FMT_F " " NGPU_FMT_VEC3

#define NGPU_FMT_MAT2 NGPU_FMT_VEC2 "\n" \
                      NGPU_FMT_VEC2
#define NGPU_FMT_MAT3 NGPU_FMT_VEC3 "\n" \
                      NGPU_FMT_VEC3 "\n" \
                      NGPU_FMT_VEC3
#define NGPU_FMT_MAT4 NGPU_FMT_VEC4 "\n" \
                      NGPU_FMT_VEC4 "\n" \
                      NGPU_FMT_VEC4 "\n" \
                      NGPU_FMT_VEC4

/* Argument helpers associated with formats declared above */
#define NGPU_ARG_F(v)    *(v)
#define NGPU_ARG_VEC2(v) NGPU_ARG_F(v), NGPU_ARG_F((v)+1)
#define NGPU_ARG_VEC3(v) NGPU_ARG_F(v), NGPU_ARG_VEC2((v)+1)
#define NGPU_ARG_VEC4(v) NGPU_ARG_F(v), NGPU_ARG_VEC3((v)+1)

#define NGPU_ARG_MAT2(v) NGPU_ARG_VEC2((v)+2*0),    \
                         NGPU_ARG_VEC2((v)+2*1)
#define NGPU_ARG_MAT3(v) NGPU_ARG_VEC3((v)+3*0),    \
                         NGPU_ARG_VEC3((v)+3*1),    \
                         NGPU_ARG_VEC3((v)+3*2)
#define NGPU_ARG_MAT4(v) NGPU_ARG_VEC4((v)+4*0),    \
                         NGPU_ARG_VEC4((v)+4*1),    \
                         NGPU_ARG_VEC4((v)+4*2),    \
                         NGPU_ARG_VEC4((v)+4*3)

#define NGPU_HAS_ALL_FLAGS(a, b) (((a) & (b)) == (b))

typedef void (*ngpu_user_free_func_type)(void *user_arg, void *data);

#endif /* UTILS_H */
