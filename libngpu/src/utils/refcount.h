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

#ifndef NGPU_REFCOUNT_H
#define NGPU_REFCOUNT_H

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

typedef void (*ngpu_freep_func)(void **sp);

struct ngpu_rc {
    size_t count;
    ngpu_freep_func freep;
};

#define NGPU_RC_CHECK_STRUCT(name) static_assert(offsetof(struct name, rc) == 0, #name "_rc")
#define NGPU_RC_CREATE(fn) (struct ngpu_rc) { .count=1, .freep=(ngpu_freep_func)fn }
#define NGPU_RC_REF(s) (void *)ngpu_rc_ref((struct ngpu_rc *)(s))
#define NGPU_RC_UNREFP(sp) ngpu_rc_unrefp((struct ngpu_rc **)(sp))

struct ngpu_rc *ngpu_rc_ref(struct ngpu_rc *s);
void ngpu_rc_unrefp(struct ngpu_rc **sp);

#endif /* REFCOUNT_H */
