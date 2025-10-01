/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2022 GoPro Inc.
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

#include "pipeline_map.h"
#include "utils/crc32.h"

static uint32_t key_hash(union hmap_key x)
{
    const struct pipeline_map_key *key = x.ptr;
    uint32_t hash = NGLI_CRC32_INIT;
    hash = ngli_crc32_mem((uint8_t *)key->graphics_state, sizeof(*key->graphics_state), hash);
    hash = ngli_crc32_mem((uint8_t *)key->rendertarget_layout, sizeof(*key->rendertarget_layout), hash);
    return hash;
}

static int key_cmp(union hmap_key a, union hmap_key b)
{
    const struct pipeline_map_key *key_a = a.ptr;
    const struct pipeline_map_key *key_b = b.ptr;
    const int cmp_graphics = memcmp(key_a->graphics_state, key_b->graphics_state,
                                     sizeof(*key_a->graphics_state));
    if (cmp_graphics)
        return cmp_graphics;
    return memcmp(key_a->rendertarget_layout, key_b->rendertarget_layout,
                  sizeof(*key_a->rendertarget_layout));
}

static union hmap_key key_dup(union hmap_key x)
{
    return x;
}

static int key_check(union hmap_key x)
{
    return 1;
}

static void key_free(union hmap_key x)
{

}

static const struct hmap_key_funcs key_funcs = {
    .hash  = key_hash,
    .cmp   = key_cmp,
    .dup   = key_dup,
    .check = key_check,
    .free  = key_free,
};


struct hmap *ngli_pipeline_map_create(void)
{
    return ngli_hmap_create_ptr(&key_funcs);
}

struct hmap *ngli_pipeline_map_set(struct hmap *s, const struct pipeline_map_key *key, void *value)
{
    const int ret = ngli_hmap_set_ptr(s, key, value);
    return ret < 0 ? NULL : s;
}

void *ngli_pipeline_map_get(struct hmap *s, const struct pipeline_map_key *key)
{
    return ngli_hmap_get_ptr(s, key);
}

struct hmap *ngli_pipeline_map_freep(struct hmap **sp)
{
    ngli_hmap_freep(sp);
    return NULL;
}
