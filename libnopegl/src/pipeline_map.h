/*
 * Copyright 2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef PIPELINE_MAP_H
#define PIPELINE_MAP_H

#include "ngpu/pipeline.h"
#include "ngpu/rendertarget.h"
#include "utils/hmap.h"

struct pipeline_map_key {
    const struct ngpu_graphics_state *graphics_state;
    const struct ngpu_rendertarget_layout *rendertarget_layout;
};

struct hmap *ngli_pipeline_map_create(void);
struct hmap *ngli_pipeline_map_set(struct hmap *s, const struct pipeline_map_key *key, void *value);
void *ngli_pipeline_map_get(struct hmap *s, const struct pipeline_map_key *key);
struct hmap *ngli_pipeline_map_freep(struct hmap **sp);

#endif
