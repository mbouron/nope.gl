/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2018-2022 GoPro Inc.
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

#include "ctx.h"
#include "program.h"

static void program_freep(void **programp)
{
    struct ngpu_program **sp = (struct ngpu_program **)programp;
    if (!*sp)
        return;

    (*sp)->gpu_ctx->cls->program_freep(sp);
}

struct ngpu_program *ngpu_program_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngpu_program *s = gpu_ctx->cls->program_create(gpu_ctx);
    if (!s)
        return NULL;
    s->rc = NGPU_RC_CREATE(program_freep);
    return s;
}

int ngpu_program_init(struct ngpu_program *s, const struct ngpu_program_params *params)
{
    return s->gpu_ctx->cls->program_init(s, params);
}

void ngpu_program_freep(struct ngpu_program **sp)
{
    NGPU_RC_UNREFP(sp);
}

struct ngpu_ctx *ngpu_program_get_ctx(const struct ngpu_program *s)
{
    return s->gpu_ctx;
}
