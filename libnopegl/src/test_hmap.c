/*
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

#define HMAP_SIZE_NBIT 1
#include "utils/crc32.h"
#include "utils/hmap.h"
#include "utils/memory.h"
#include "utils/string.h"
#include "utils/utils.h"

#define PRINT_HMAP(...) do {                                    \
    printf(__VA_ARGS__);                                        \
    const struct hmap_entry *e = NULL;                          \
    while ((e = ngli_hmap_next(hm, e)))                         \
        printf("  %08X %s: %s\n", ngli_crc32(e->key.str),       \
               e->key.str, (const char *)e->data);              \
    printf("\n");                                               \
} while (0)

#define RSTR "replaced"

static void free_func(void *arg, void *data)
{
    ngli_free(data);
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
    /* the two following entries have the same CRC */
    {"codding", "data#0"},
    {"gnu",     "data#1"},
    {"last",    "samurai"},
};

static size_t get_key_index(const char *s)
{
    for (size_t i = 0; i < NGLI_ARRAY_NB(kvs); i++)
        if (!strcmp(kvs[i].key, s))
            return i;
    return SIZE_MAX;
}

static void check_order(const struct hmap *hm)
{
    size_t last_index = SIZE_MAX;
    const struct hmap_entry *e = NULL;
    while ((e = ngli_hmap_next(hm, e))) {
        const size_t index = get_key_index(e->key.str);
        ngli_assert(last_index == SIZE_MAX || index > last_index);
        last_index = index;
    }
}

static int test_bucket_delete_reuse(void)
{
    struct hmap *hm = ngli_hmap_create(NGLI_HMAP_TYPE_STR);
    if (!hm)
        return -1;
    ngli_hmap_set_str(hm, "foo", "bar");
    ngli_hmap_set_str(hm, "foo", NULL);
    ngli_hmap_set_str(hm, "foo", "bar");
    ngli_hmap_freep(&hm);
    return 0;
}

int main(void)
{
    ngli_assert(ngli_crc32("codding") == ngli_crc32("gnu"));

    int ret = test_bucket_delete_reuse();
    if (ret < 0)
        return 1;

    for (int custom_alloc = 0; custom_alloc <= 1; custom_alloc++) {
        struct hmap *hm = ngli_hmap_create(NGLI_HMAP_TYPE_STR);

        if (custom_alloc)
            ngli_hmap_set_free_func(hm, free_func, NULL);

        /* Test addition */
        for (size_t i = 0; i < NGLI_ARRAY_NB(kvs); i++) {
            void *data = custom_alloc ? ngli_strdup(kvs[i].val) : (void*)kvs[i].val;
            ngli_assert(ngli_hmap_set_str(hm, kvs[i].key, data) >= 0);
            const char *val = ngli_hmap_get_str(hm, kvs[i].key);
            ngli_assert(val);
            ngli_assert(!strcmp(val, kvs[i].val));
            check_order(hm);
        }

        PRINT_HMAP("init [%zu entries] [custom_alloc:%s]:\n",
                   ngli_hmap_count(hm), custom_alloc ? "yes" : "no");

        for (size_t i = 0; i < NGLI_ARRAY_NB(kvs) - 1; i++) {

            /* Test replace */
            if (i & 1) {
                void *data = custom_alloc ? ngli_strdup(RSTR) : RSTR;
                ngli_assert(ngli_hmap_set_str(hm, kvs[i].key, data) == 0);
                const char *val = ngli_hmap_get_str(hm, kvs[i].key);
                ngli_assert(val);
                ngli_assert(strcmp(val, RSTR) == 0);
                PRINT_HMAP("replace %s:\n", kvs[i].key);
                check_order(hm);
            }

            /* Test delete */
            ngli_assert(ngli_hmap_set_str(hm, kvs[i].key, NULL) == 1);
            ngli_assert(ngli_hmap_set_str(hm, kvs[i].key, NULL) == 0);
            PRINT_HMAP("drop %s (%zu remaining):\n", kvs[i].key, ngli_hmap_count(hm));
            check_order(hm);
        }

        ngli_hmap_freep(&hm);
    }

    return 0;
}
