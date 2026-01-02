/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef NGPU_BINDGROUP_H
#define NGPU_BINDGROUP_H

#include <stdlib.h>

#include "ngpu/ngpu.h"

#include "ngpu/buffer.h"
#include "ngpu/program.h"
#include "ngpu/texture.h"

struct ngpu_ctx;

struct ngpu_bindgroup_layout {
    struct ngli_rc rc;
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_bindgroup_layout_entry *textures;
    size_t nb_textures;
    struct ngpu_bindgroup_layout_entry *buffers;
    size_t nb_buffers;
    size_t nb_dynamic_offsets;
};

NGLI_RC_CHECK_STRUCT(ngpu_bindgroup_layout);

struct ngpu_bindgroup {
    struct ngli_rc rc;
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_bindgroup_layout *layout;
};

NGLI_RC_CHECK_STRUCT(ngpu_bindgroup);

#endif
