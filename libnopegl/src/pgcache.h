/*
 * Copyright 2020-2022 GoPro Inc.
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

#ifndef PGCACHE_H
#define PGCACHE_H

#include "hmap.h"
#include "gpu_program.h"

struct pgcache {
    struct gpu_ctx *gpu_ctx;
    struct hmap *graphics_cache;
    struct hmap *compute_cache;
};

int ngli_pgcache_init(struct pgcache *s, struct gpu_ctx *ctx);
int ngli_pgcache_get_graphics_program(struct pgcache *s, struct gpu_program **dstp, const struct gpu_program_params *params);
int ngli_pgcache_get_compute_program(struct pgcache *s, struct gpu_program **dstp, const struct gpu_program_params *params);
void ngli_pgcache_reset(struct pgcache *s);

#endif
