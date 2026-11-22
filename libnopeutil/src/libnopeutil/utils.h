/*
 * Copyright 2023-2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef NPRIV_UTILS_H
#define NPRIV_UTILS_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "error.h"

#if defined(__GNUC__) || defined(__clang__)
# ifdef __MINGW32__
#  define npriv_printf_format(fmtpos, attrpos) __attribute__((__format__(__MINGW_PRINTF_FORMAT, fmtpos, attrpos)))
# else
#  define npriv_printf_format(fmtpos, attrpos) __attribute__((__format__(__printf__, fmtpos, attrpos)))
# endif
#else
# define npriv_printf_format(fmtpos, attrpos)
#endif

#if defined(__GNUC__) || defined(__clang__)
# define npriv_unused __attribute__((unused))
# define npriv_may_alias __attribute__((may_alias))
#else
# define npriv_unused
# define npriv_may_alias
#endif

#define npriv_assert(cond) \
    ((void)((cond) || (fprintf(stderr, "Assert %s @ %s:%d\n", \
                               #cond, __FILE__, __LINE__),    \
                       abort(), 0)))

#define NPRIV_STATIC_ASSERT(c, id) static_assert(c, id)

#define NPRIV_FIELD_SIZEOF(name, field) (sizeof(((name *)0)->field))

#define NPRIV_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NPRIV_MAX(a, b) ((a) > (b) ? (a) : (b))
#define NPRIV_CLAMP(x, min, max) NPRIV_MAX(NPRIV_MIN(x, max), min)

#define NPRIV_ARRAY_MEMDUP(dst, src, name) do {                                 \
    const size_t nb = (src)->nb_##name;                                         \
    if (nb > 0) {                                                               \
        (dst)->name = npriv_try_memdup((src)->name, nb * sizeof(*(dst)->name)); \
        if (!(dst)->name)                                                       \
            return NPRIV_ERROR_MEMORY;                                          \
    } else {                                                                    \
        (dst)->name = NULL;                                                     \
    }                                                                           \
    (dst)->nb_##name = nb;                                                      \
} while (0)                                                                     \

#define NPRIV_ARRAY_NB(x) (sizeof(x)/sizeof(*(x)))

#define NPRIV_SWAP(a, b) do {               \
    __typeof__(a) npriv_swap_tmp_ = (b);    \
    (b) = (a);                              \
    (a) = npriv_swap_tmp_;                  \
} while (0)

#define NPRIV_ALIGN_MASK(v, mask) (((v) + (mask)) & ~(mask))
#define NPRIV_ALIGN(v, a) NPRIV_ALIGN_MASK(v, (__typeof__(v))(a) - 1)
#define NPRIV_ALIGN_VAL 16

#define NPRIV_IS_ALIGNED(v, a) (((v) & ((__typeof__(v))(a) - 1)) == 0)

#define NPRIV_ALIGNMENT(v) ((v) & ~((v) - 1))

#define NPRIV_IS_POW2(v) ((v) && ((v) & ((v) - 1)) == 0)

#define NPRIV_ATTR_ALIGNED _Alignas(NPRIV_ALIGN_VAL)

#define NPRIV_ALIGNED_VEC(vname) float NPRIV_ATTR_ALIGNED vname[4]
#define NPRIV_ALIGNED_MAT(mname) float NPRIV_ATTR_ALIGNED mname[4*4]

/* Format printf helpers */
#define NPRIV_FMT_F    "%12g"
#define NPRIV_FMT_VEC2 NPRIV_FMT_F " " NPRIV_FMT_F
#define NPRIV_FMT_VEC3 NPRIV_FMT_F " " NPRIV_FMT_VEC2
#define NPRIV_FMT_VEC4 NPRIV_FMT_F " " NPRIV_FMT_VEC3

#define NPRIV_FMT_MAT2 NPRIV_FMT_VEC2 "\n" \
                       NPRIV_FMT_VEC2
#define NPRIV_FMT_MAT3 NPRIV_FMT_VEC3 "\n" \
                       NPRIV_FMT_VEC3 "\n" \
                       NPRIV_FMT_VEC3
#define NPRIV_FMT_MAT4 NPRIV_FMT_VEC4 "\n" \
                       NPRIV_FMT_VEC4 "\n" \
                       NPRIV_FMT_VEC4 "\n" \
                       NPRIV_FMT_VEC4

/* Argument helpers associated with formats declared above */
#define NPRIV_ARG_F(v)    *(v)
#define NPRIV_ARG_VEC2(v) NPRIV_ARG_F(v), NPRIV_ARG_F((v)+1)
#define NPRIV_ARG_VEC3(v) NPRIV_ARG_F(v), NPRIV_ARG_VEC2((v)+1)
#define NPRIV_ARG_VEC4(v) NPRIV_ARG_F(v), NPRIV_ARG_VEC3((v)+1)

#define NPRIV_ARG_MAT2(v) NPRIV_ARG_VEC2((v)+2*0),    \
                          NPRIV_ARG_VEC2((v)+2*1)
#define NPRIV_ARG_MAT3(v) NPRIV_ARG_VEC3((v)+3*0),    \
                          NPRIV_ARG_VEC3((v)+3*1),    \
                          NPRIV_ARG_VEC3((v)+3*2)
#define NPRIV_ARG_MAT4(v) NPRIV_ARG_VEC4((v)+4*0),    \
                          NPRIV_ARG_VEC4((v)+4*1),    \
                          NPRIV_ARG_VEC4((v)+4*2),    \
                          NPRIV_ARG_VEC4((v)+4*3)

#define NPRIV_HAS_ALL_FLAGS(a, b) (((a) & (b)) == (b))

typedef void (*npriv_user_free_func_type)(void *user_arg, void *data);

#endif /* NPRIV_UTILS_H */
