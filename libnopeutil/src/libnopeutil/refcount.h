/*
 * Copyright 2023-2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef NPRIV_REFCOUNT_H
#define NPRIV_REFCOUNT_H

#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

typedef void (*npriv_freep_func)(void **sp);

struct npriv_rc {
    _Atomic(size_t) count;
    npriv_freep_func freep;
};

#define NPRIV_RC_CHECK_STRUCT(name) static_assert(offsetof(struct name, rc) == 0, #name "_rc")
#define NPRIV_RC_CREATE(fn) (struct npriv_rc) { .count=1, .freep=(npriv_freep_func)fn }
#define NPRIV_RC_REF(s) ((__typeof__(s))npriv_rc_ref((struct npriv_rc *)(s)))
#define NPRIV_RC_UNREFP(sp) npriv_rc_unrefp((struct npriv_rc **)(sp))

struct npriv_rc *npriv_rc_ref(struct npriv_rc *s);
void npriv_rc_unrefp(struct npriv_rc **sp);

#endif /* NPRIV_REFCOUNT_H */
