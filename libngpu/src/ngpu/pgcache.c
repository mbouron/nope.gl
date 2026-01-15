/*
 * Copyright 2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <string.h>

#include "ngpu/pgcache.h"
#include "ngpu/utils/hmap.h"
#include "ngpu/utils/memory.h"
#include "ngpu/utils/utils.h"

struct ngpu_pgcache {
    struct ngpu_ctx *gpu_ctx;
    struct hmap *graphics_cache;
    struct hmap *compute_cache;
};

static void reset_cached_program(void *user_arg, void *data)
{
    struct ngpu_program *p = data;
    ngpu_program_freep(&p);
}

static void reset_cached_frag_map(void *user_arg, void *data)
{
    struct hmap *p = data;
    ngpu_hmap_freep(&p);
}

struct ngpu_pgcache *ngpu_pgcache_create(struct ngpu_ctx *ctx)
{
    struct ngpu_pgcache *s = ngpu_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->gpu_ctx = ctx;
    s->graphics_cache = ngpu_hmap_create(NGPU_HMAP_TYPE_STR);
    s->compute_cache = ngpu_hmap_create(NGPU_HMAP_TYPE_STR);
    if (!s->graphics_cache || !s->compute_cache)
        goto fail;
    ngpu_hmap_set_free_func(s->graphics_cache, reset_cached_frag_map, s);
    ngpu_hmap_set_free_func(s->compute_cache, reset_cached_program, s);
    return s;

fail:
    ngpu_pgcache_freep(&s);
    return NULL;
}

static int query_cache(struct ngpu_pgcache *s, struct ngpu_program **dstp,
                       struct hmap *cache, const char *cache_key,
                       const struct ngpu_program_params *params)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    struct ngpu_program *cached_program = ngpu_hmap_get_str(cache, cache_key);
    if (cached_program) {
        /* make sure the cached program has not been reset by the user */
        ngpu_assert(ngpu_program_get_ctx(cached_program));

        *dstp = cached_program;
        return 0;
    }

    /* this is free'd by the reset_cached_program() when destroying the cache */
    struct ngpu_program *new_program = ngpu_program_create(gpu_ctx);
    if (!new_program)
        return NGPU_ERROR_MEMORY;

    int ret = ngpu_program_init(new_program, params);
    if (ret < 0) {
        ngpu_program_freep(&new_program);
        return ret;
    }

    ret = ngpu_hmap_set_str(cache, cache_key, new_program);
    if (ret < 0) {
        ngpu_program_freep(&new_program);
        return ret;
    }

    *dstp = new_program;
    return 0;
}

int ngpu_pgcache_get_graphics_program(struct ngpu_pgcache *s, struct ngpu_program **dstp, const struct ngpu_program_params *params)
{
    /*
     * The first dimension of the graphics_cache hmap is another hmap: what we
     * do is basically graphics_cache[vert][frag] to obtain the program. If the
     * 2nd hmap is not yet allocated, we do create a new one here.
     */
    struct hmap *frag_map = ngpu_hmap_get_str(s->graphics_cache, params->vertex);
    if (!frag_map) {
        frag_map = ngpu_hmap_create(NGPU_HMAP_TYPE_STR);
        if (!frag_map)
            return NGPU_ERROR_MEMORY;
        ngpu_hmap_set_free_func(frag_map, reset_cached_program, s);

        int ret = ngpu_hmap_set_str(s->graphics_cache, params->vertex, frag_map);
        if (ret < 0) {
            ngpu_hmap_freep(&frag_map);
            return NGPU_ERROR_MEMORY;
        }
    }

    return query_cache(s, dstp, frag_map, params->fragment, params);
}

int ngpu_pgcache_get_compute_program(struct ngpu_pgcache *s, struct ngpu_program **dstp, const struct ngpu_program_params *params)
{
    return query_cache(s, dstp, s->compute_cache, params->compute, params);
}

void ngpu_pgcache_freep(struct ngpu_pgcache **sp)
{
    struct ngpu_pgcache *s = *sp;
    if (!s)
        return;
    ngpu_hmap_freep(&s->compute_cache);
    ngpu_hmap_freep(&s->graphics_cache);
    ngpu_freep(sp);
}
