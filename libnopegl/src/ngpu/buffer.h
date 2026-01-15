/*
 * Copyright 2024 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef NGPU_BUFFER_H
#define NGPU_BUFFER_H

#include <stdint.h>
#include <stdlib.h>

#include "ngpu/utils/refcount.h"

struct ngpu_ctx;

struct ngpu_buffer {
    struct ngpu_rc rc;
    struct ngpu_ctx *gpu_ctx;
    size_t size;
    uint32_t usage;
};

NGPU_RC_CHECK_STRUCT(ngpu_buffer);

#endif
