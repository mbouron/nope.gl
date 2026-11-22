/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2017-2022 GoPro Inc.
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

#define XXH_INLINE_ALL
#include <xxhash.h>

#define HMAP_SIZE_NBIT 1
#include "libnopeutil/hmap.h"
#include "libnopeutil/memory.h"
#include "libnopeutil/string.h"
#include "libnopeutil/utils.h"

#define PRINT_HMAP(...) do {                                    \
    printf(__VA_ARGS__);                                        \
    const struct hmap_entry *e = NULL;                          \
    while ((e = npriv_hmap_next(hm, e)))                        \
        printf("  %08X %s: %s\n", e->hash,                      \
               e->key.str, (const char *)e->data);              \
    printf("\n");                                               \
} while (0)

#define RSTR "replaced"

static void free_func(void *arg, void *data)
{
    npriv_free(data);
}

static const struct {
    const char *key;
    const char *val;
} kvs[] = {
    {"foo",     "bar"},
    {"hello",   "world"},
    {"lorem",   "ipsum"},
    {"bazbaz",  ""},
    {"abc",     "def"},
    /* the two following entries have the same truncated XXH3 hash */
    {"xxh-key-84663",  "data#0"},
    {"xxh-key-160767", "data#1"},
    {"last",    "samurai"},
};

static size_t get_key_index(const char *s)
{
    for (size_t i = 0; i < NPRIV_ARRAY_NB(kvs); i++)
        if (!strcmp(kvs[i].key, s))
            return i;
    return SIZE_MAX;
}

static void check_order(const struct hmap *hm)
{
    size_t last_index = SIZE_MAX;
    const struct hmap_entry *e = NULL;
    while ((e = npriv_hmap_next(hm, e))) {
        const size_t index = get_key_index(e->key.str);
        npriv_assert(last_index == SIZE_MAX || index > last_index);
        last_index = index;
    }
}

static int test_bucket_delete_reuse(void)
{
    struct hmap *hm = npriv_hmap_try_create(NPRIV_HMAP_TYPE_STR);
    if (!hm)
        return -1;
    npriv_hmap_try_set_str(hm, "foo", "bar");
    npriv_hmap_try_set_str(hm, "foo", NULL);
    npriv_hmap_try_set_str(hm, "foo", "bar");
    npriv_hmap_freep(&hm);
    return 0;
}

static int test_u64(void)
{
    struct hmap *hm = npriv_hmap_try_create(NPRIV_HMAP_TYPE_U64);
    if (!hm)
        return -1;

    for (uint64_t i = 0; i < 1024; i++) {
        const uint64_t key = i * 2654435761U;
        const void *data = (void *)(uintptr_t)(i + 1);
        npriv_assert(npriv_hmap_try_set_u64(hm, key, (void *)data) == 0);
        npriv_assert(npriv_hmap_get_u64(hm, key) == data);
    }

    for (uint64_t i = 0; i < 1024; i += 3) {
        const uint64_t key = i * 2654435761U;
        npriv_assert(npriv_hmap_try_set_u64(hm, key, NULL) == 1);
        npriv_assert(!npriv_hmap_get_u64(hm, key));
    }

    size_t count = 0;
    const struct hmap_entry *e = NULL;
    while ((e = npriv_hmap_next(hm, e))) {
        npriv_assert(e->key.u64 == (uint64_t)(count + count / 2 + 1) * 2654435761U);
        count++;
    }
    npriv_assert(count == npriv_hmap_count(hm));

    npriv_hmap_freep(&hm);
    npriv_assert(!hm);
    return 0;
}


int main(void)
{
    const uint32_t hash0 = (uint32_t)XXH3_64bits(kvs[5].key, strlen(kvs[5].key));
    const uint32_t hash1 = (uint32_t)XXH3_64bits(kvs[6].key, strlen(kvs[6].key));
    npriv_assert(hash0 == hash1);

    int ret = test_bucket_delete_reuse();
    if (ret < 0)
        return 1;
    ret = test_u64();
    if (ret < 0)
        return 1;
    for (int custom_alloc = 0; custom_alloc <= 1; custom_alloc++) {
        struct hmap *hm = npriv_hmap_try_create(NPRIV_HMAP_TYPE_STR);

        npriv_assert(!npriv_hmap_get_str(hm, NULL));

        if (custom_alloc)
            npriv_hmap_set_free_func(hm, free_func, NULL);

        /* Test addition */
        for (size_t i = 0; i < NPRIV_ARRAY_NB(kvs); i++) {
            void *data = custom_alloc ? npriv_strdup(kvs[i].val) : (void*)kvs[i].val;
            npriv_assert(npriv_hmap_try_set_str(hm, kvs[i].key, data) >= 0);
            const char *val = npriv_hmap_get_str(hm, kvs[i].key);
            npriv_assert(val);
            npriv_assert(!strcmp(val, kvs[i].val));
            check_order(hm);
        }

        PRINT_HMAP("init [%zu entries] [custom_alloc:%s]:\n",
                   npriv_hmap_count(hm), custom_alloc ? "yes" : "no");

        for (size_t i = 0; i < NPRIV_ARRAY_NB(kvs) - 1; i++) {

            /* Test replace */
            if (i & 1) {
                void *data = custom_alloc ? npriv_strdup(RSTR) : RSTR;
                npriv_assert(npriv_hmap_try_set_str(hm, kvs[i].key, data) == 0);
                const char *val = npriv_hmap_get_str(hm, kvs[i].key);
                npriv_assert(val);
                npriv_assert(strcmp(val, RSTR) == 0);
                PRINT_HMAP("replace %s:\n", kvs[i].key);
                check_order(hm);
            }

            /* Test delete */
            npriv_assert(npriv_hmap_try_set_str(hm, kvs[i].key, NULL) == 1);
            npriv_assert(npriv_hmap_try_set_str(hm, kvs[i].key, NULL) == 0);
            PRINT_HMAP("drop %s (%zu remaining):\n", kvs[i].key, npriv_hmap_count(hm));
            check_order(hm);
        }

        npriv_hmap_freep(&hm);
    }

    return 0;
}
