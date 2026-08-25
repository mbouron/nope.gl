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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "hmap.h"
#include "memory.h"
#include "utils.h"
#include "string.h"

#define HMAP_EMPTY_INDEX SIZE_MAX

/* Sparse hash index pointing into the dense, insertion-ordered entries. */
struct hmap {
    size_t *indices;
    size_t size;
    size_t mask;
    struct hmap_entry *entries;
    size_t count;
    size_t capacity;
    ngli_user_free_func_type user_free_func;
    void *user_arg;
    enum hmap_type type;
    struct hmap_key_funcs key_funcs;
};

static uint32_t key_hash_str(union hmap_key x)
{
    return (uint32_t)XXH3_64bits(x.str, strlen(x.str));
}

static uint32_t key_hash_u64(union hmap_key x)
{
    return (uint32_t)XXH3_64bits(&x.u64, sizeof(x.u64));
}

static int key_cmp_str(union hmap_key a, union hmap_key b) { return strcmp(a.str, b.str); }
static int key_cmp_u64(union hmap_key a, union hmap_key b) { return a.u64 != b.u64; }

static union hmap_key key_dup_str(union hmap_key x) { return (union hmap_key){.str=ngli_strdup(x.str)}; }
static union hmap_key key_dup_u64(union hmap_key x) { return x; }

static int key_check_str(union hmap_key x) { return !!x.str; }
static int key_check_u64(union hmap_key x) { return 1; }

static void key_free_str(union hmap_key x) { ngli_free(x.str); }
static void key_free_u64(union hmap_key x) { }

static const struct hmap_key_funcs key_funcs_map[] = {
    [NGLI_HMAP_TYPE_STR] = {key_hash_str, key_cmp_str, key_dup_str, key_check_str, key_free_str},
    [NGLI_HMAP_TYPE_U64] = {key_hash_u64, key_cmp_u64, key_dup_u64, key_check_u64, key_free_u64},
};

static void reset_indices(size_t *indices, size_t size)
{
    for (size_t i = 0; i < size; i++)
        indices[i] = HMAP_EMPTY_INDEX;
}

static void add_index(size_t *indices, size_t mask, uint32_t hash, size_t entry_index)
{
    size_t index = (size_t)hash & mask;
    while (indices[index] != HMAP_EMPTY_INDEX)
        index = (index + 1) & mask;
    indices[index] = entry_index;
}

static void rebuild_indices(struct hmap *hm)
{
    reset_indices(hm->indices, hm->size);
    for (size_t i = 0; i < hm->count; i++)
        add_index(hm->indices, hm->mask, hm->entries[i].hash, i);
}

static size_t find_entry(const struct hmap *hm, union hmap_key key, uint32_t hash)
{
    size_t index = (size_t)hash & hm->mask;
    for (size_t i = 0; i < hm->size; i++) {
        const size_t entry_index = hm->indices[index];
        if (entry_index == HMAP_EMPTY_INDEX)
            return HMAP_EMPTY_INDEX;
        const struct hmap_entry *e = &hm->entries[entry_index];
        if (e->hash == hash && !hm->key_funcs.cmp(e->key, key))
            return entry_index;
        index = (index + 1) & hm->mask;
    }
    return HMAP_EMPTY_INDEX;
}

static int grow_indices(struct hmap *hm)
{
    size_t new_size;
    if (NGLI_CHK_MUL(&new_size, hm->size, 2))
        return NGL_ERROR_LIMIT_EXCEEDED;

    size_t alloc_size;
    if (NGLI_CHK_MUL(&alloc_size, new_size, sizeof(*hm->indices)))
        return NGL_ERROR_LIMIT_EXCEEDED;

    size_t *new_indices = ngli_try_malloc(alloc_size);
    if (!new_indices)
        return NGL_ERROR_MEMORY;

    reset_indices(new_indices, new_size);
    const size_t new_mask = new_size - 1;
    for (size_t i = 0; i < hm->count; i++)
        add_index(new_indices, new_mask, hm->entries[i].hash, i);

    ngli_free(hm->indices);
    hm->indices = new_indices;
    hm->size = new_size;
    hm->mask = new_mask;
    return 0;
}

static int grow_entries(struct hmap *hm)
{
    if (hm->count < hm->capacity)
        return 0;

    size_t new_capacity = hm->capacity ? hm->capacity : (size_t)1 << HMAP_SIZE_NBIT;
    if (hm->capacity && NGLI_CHK_MUL(&new_capacity, hm->capacity, 2))
        return NGL_ERROR_LIMIT_EXCEEDED;

    struct hmap_entry *entries = ngli_try_realloc(hm->entries, new_capacity, sizeof(*entries));
    if (!entries)
        return NGL_ERROR_MEMORY;
    hm->entries = entries;
    hm->capacity = new_capacity;
    return 0;
}

static struct hmap *hmap_try_create(void)
{
    struct hmap *hm = ngli_try_calloc(1, sizeof(*hm));
    if (!hm)
        return NULL;

    hm->size = (size_t)1 << HMAP_SIZE_NBIT;
    hm->mask = hm->size - 1;
    hm->indices = ngli_try_malloc(hm->size * sizeof(*hm->indices));
    if (!hm->indices) {
        ngli_free(hm);
        return NULL;
    }
    reset_indices(hm->indices, hm->size);
    return hm;
}

struct hmap *ngli_hmap_try_create_ptr(const struct hmap_key_funcs *key_funcs)
{
    struct hmap *hm = hmap_try_create();
    if (!hm)
        return NULL;
    hm->type = NGLI_HMAP_TYPE_PTR;
    hm->key_funcs = *key_funcs;
    return hm;
}

struct hmap *ngli_hmap_try_create(enum hmap_type type)
{
    ngli_assert(type > NGLI_HMAP_TYPE_PTR && type < NGLI_HMAP_TYPE_NB);
    struct hmap *hm = hmap_try_create();
    if (!hm)
        return NULL;
    hm->type = type;
    hm->key_funcs = key_funcs_map[type];
    return hm;
}

struct hmap *ngli_hmap_create_ptr(const struct hmap_key_funcs *key_funcs)
{
    struct hmap *hm = ngli_hmap_try_create_ptr(key_funcs);
    if (!hm)
        ngli_oom();
    return hm;
}

struct hmap *ngli_hmap_create(enum hmap_type type)
{
    struct hmap *hm = ngli_hmap_try_create(type);
    if (!hm)
        ngli_oom();
    return hm;
}

void ngli_hmap_set_free_func(struct hmap *hm, ngli_user_free_func_type user_free_func, void *user_arg)
{
    ngli_assert(!hm->count);
    hm->user_free_func = user_free_func;
    hm->user_arg = user_arg;
}

size_t ngli_hmap_count(const struct hmap *hm)
{
    return hm->count;
}

static int delete_entry(struct hmap *hm, size_t entry_index)
{
    struct hmap_entry *e = &hm->entries[entry_index];
    hm->key_funcs.free(e->key);
    if (hm->user_free_func)
        hm->user_free_func(hm->user_arg, e->data);

    hm->count--;
    /* Deletion is deliberately O(n): it keeps the hot iteration path dense. */
    memmove(hm->entries + entry_index, hm->entries + entry_index + 1,
            (hm->count - entry_index) * sizeof(*hm->entries));
    rebuild_indices(hm);
    return 1;
}

static int hmap_try_set(struct hmap *hm, union hmap_key key, void *data)
{
    if (!hm->key_funcs.check(key))
        return NGL_ERROR_INVALID_ARG;

    const uint32_t hash = hm->key_funcs.hash(key);
    const size_t entry_index = find_entry(hm, key, hash);

    if (!data)
        return entry_index == HMAP_EMPTY_INDEX ? 0 : delete_entry(hm, entry_index);

    if (entry_index != HMAP_EMPTY_INDEX) {
        struct hmap_entry *e = &hm->entries[entry_index];
        if (hm->user_free_func)
            hm->user_free_func(hm->user_arg, e->data);
        e->data = data;
        return 0;
    }

    const size_t max_count = hm->size > 2 ? hm->size - hm->size / 4 : hm->size - 1;
    if (hm->count >= max_count) {
        int ret = grow_indices(hm);
        if (ret < 0)
            return ret;
    }

    int ret = grow_entries(hm);
    if (ret < 0)
        return ret;

    union hmap_key new_key = hm->key_funcs.dup(key);
    if (!hm->key_funcs.check(new_key))
        return NGL_ERROR_MEMORY;

    struct hmap_entry *e = &hm->entries[hm->count];
    e->key = new_key;
    e->data = data;
    e->hash = hash;
    add_index(hm->indices, hm->mask, hash, hm->count++);
    return 0;
}

int ngli_hmap_try_set_ptr(struct hmap *hm, const void *ptr, void *data)
{
    ngli_assert(hm->type == NGLI_HMAP_TYPE_PTR);
    const union hmap_key key = {.ptr=(void *)ptr};
    return hmap_try_set(hm, key, data);
}

int ngli_hmap_try_set_str(struct hmap *hm, const char *str, void *data)
{
    ngli_assert(hm->type == NGLI_HMAP_TYPE_STR);
    const union hmap_key key = {.str=(char *)str};
    return hmap_try_set(hm, key, data);
}

int ngli_hmap_try_set_u64(struct hmap *hm, uint64_t u64, void *data)
{
    ngli_assert(hm->type == NGLI_HMAP_TYPE_U64);
    const union hmap_key key = {.u64=u64};
    return hmap_try_set(hm, key, data);
}

void ngli_hmap_set_ptr(struct hmap *hm, const void *ptr, void *data)
{
    if (ngli_hmap_try_set_ptr(hm, ptr, data) < 0)
        ngli_oom();
}

void ngli_hmap_set_str(struct hmap *hm, const char *str, void *data)
{
    if (ngli_hmap_try_set_str(hm, str, data) < 0)
        ngli_oom();
}

void ngli_hmap_set_u64(struct hmap *hm, uint64_t u64, void *data)
{
    if (ngli_hmap_try_set_u64(hm, u64, data) < 0)
        ngli_oom();
}

struct hmap_entry *ngli_hmap_next(const struct hmap *hm, const struct hmap_entry *prev)
{
    size_t index = 0;
    if (prev)
        index = (size_t)(prev - hm->entries) + 1;
    return index < hm->count ? &hm->entries[index] : NULL;
}

static void *hmap_get(const struct hmap *hm, union hmap_key key)
{
    if (!hm->key_funcs.check(key))
        return NULL;
    const uint32_t hash = hm->key_funcs.hash(key);
    const size_t entry_index = find_entry(hm, key, hash);
    return entry_index != HMAP_EMPTY_INDEX ? hm->entries[entry_index].data : NULL;
}

void *ngli_hmap_get_ptr(const struct hmap *hm, const void *ptr)
{
    ngli_assert(hm->type == NGLI_HMAP_TYPE_PTR);
    const union hmap_key key = {.ptr=(void *)ptr};
    return hmap_get(hm, key);
}

void *ngli_hmap_get_str(const struct hmap *hm, const char *str)
{
    ngli_assert(hm->type == NGLI_HMAP_TYPE_STR);
    const union hmap_key key = {.str=(char *)str};
    return hmap_get(hm, key);
}

void *ngli_hmap_get_u64(const struct hmap *hm, uint64_t u64)
{
    ngli_assert(hm->type == NGLI_HMAP_TYPE_U64);
    const union hmap_key key = {.u64=u64};
    return hmap_get(hm, key);
}

void ngli_hmap_freep(struct hmap **hmp)
{
    struct hmap *hm = *hmp;
    if (!hm)
        return;

    for (size_t i = 0; i < hm->count; i++) {
        struct hmap_entry *e = &hm->entries[i];
        hm->key_funcs.free(e->key);
        if (hm->user_free_func)
            hm->user_free_func(hm->user_arg, e->data);
    }

    ngli_free(hm->entries);
    ngli_free(hm->indices);
    ngli_freep(hmp);
}
