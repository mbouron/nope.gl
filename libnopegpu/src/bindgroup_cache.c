/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include "bindgroup.h"
#include "utils/memory.h"

struct cache_entry {
    struct ngpu_bindgroup *bindgroup;
    uint64_t last_used;
};

struct ngpu_bindgroup_cache {
    struct ngpu_ctx *gpu_ctx;
    size_t capacity;
    uint64_t serial;
    struct cache_entry entries[];
};

struct ngpu_bindgroup_cache *ngpu_bindgroup_cache_create(struct ngpu_ctx *gpu_ctx, size_t capacity)
{
    if (!gpu_ctx || !capacity ||
        capacity > (SIZE_MAX - sizeof(struct ngpu_bindgroup_cache)) / sizeof(struct cache_entry))
        return NULL;
    const size_t size = sizeof(struct ngpu_bindgroup_cache) + capacity * sizeof(struct cache_entry);
    struct ngpu_bindgroup_cache *s = ngpu_try_calloc(1, size);
    if (!s)
        return NULL;
    s->gpu_ctx = gpu_ctx;
    s->capacity = capacity;
    return s;
}

static int matches(const struct ngpu_bindgroup *s, const struct ngpu_bindgroup_desc *desc)
{
    if (s->layout != desc->layout)
        return 0;
    for (size_t i = 0; i < desc->nb_textures; i++) {
        if (s->textures[i].texture != desc->textures[i].texture ||
            s->textures[i].immutable_sampler != desc->textures[i].immutable_sampler)
            return 0;
    }
    for (size_t i = 0; i < desc->nb_buffers; i++) {
        if (s->buffers[i].buffer != desc->buffers[i].buffer ||
            s->buffers[i].offset != desc->buffers[i].offset ||
            s->buffers[i].size != desc->buffers[i].size)
            return 0;
    }
    return 1;
}

struct ngpu_bindgroup *ngpu_bindgroup_cache_get(struct ngpu_bindgroup_cache *s, const struct ngpu_bindgroup_desc *desc)
{
    if (!s || !desc || !desc->layout || desc->layout->gpu_ctx != s->gpu_ctx ||
        desc->nb_textures != desc->layout->nb_textures ||
        desc->nb_buffers != desc->layout->nb_buffers ||
        (desc->nb_textures && !desc->textures) || (desc->nb_buffers && !desc->buffers))
        return NULL;

    /* Start a new LRU epoch if the access counter wraps. Returned references stay valid. */
    if (s->serial == UINT64_MAX)
        ngpu_bindgroup_cache_clear(s);

    struct cache_entry *victim = &s->entries[0];
    for (size_t i = 0; i < s->capacity; i++) {
        struct cache_entry *entry = &s->entries[i];
        if (entry->bindgroup && matches(entry->bindgroup, desc)) {
            entry->last_used = ++s->serial;
            return NGPU_RC_REF(entry->bindgroup);
        }
        if ((!entry->bindgroup && victim->bindgroup) ||
            (entry->bindgroup && victim->bindgroup && entry->last_used < victim->last_used))
            victim = entry;
    }

    /* Keep the old entry intact if validation or allocation fails. */
    struct ngpu_bindgroup *bindgroup = ngpu_bindgroup_create(s->gpu_ctx, desc);
    if (!bindgroup)
        return NULL;
    ngpu_bindgroup_freep(&victim->bindgroup);
    victim->bindgroup = NGPU_RC_REF(bindgroup);
    victim->last_used = ++s->serial;
    return bindgroup;
}

void ngpu_bindgroup_cache_clear(struct ngpu_bindgroup_cache *s)
{
    if (!s)
        return;
    for (size_t i = 0; i < s->capacity; i++) {
        ngpu_bindgroup_freep(&s->entries[i].bindgroup);
        s->entries[i].last_used = 0;
    }
    s->serial = 0;
}

void ngpu_bindgroup_cache_freep(struct ngpu_bindgroup_cache **sp)
{
    if (!*sp)
        return;
    ngpu_bindgroup_cache_clear(*sp);
    ngpu_freep(sp);
}
