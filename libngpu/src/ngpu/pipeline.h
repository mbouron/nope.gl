/*
 * Copyright 2019-2022 GoPro Inc.
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

#ifndef GPU_PIPELINE_H
#define GPU_PIPELINE_H

#include "ngpu/ngpu.h"

#include "ngpu/bindgroup.h"
#include "ngpu/buffer.h"
#include "ngpu/program.h"
#include "ngpu/texture.h"

#include "ngpu/utils/refcount.h"

struct ngpu_ctx;

struct ngpu_pipeline {
    struct ngpu_rc rc;
    struct ngpu_ctx *gpu_ctx;

    enum ngpu_pipeline_type type;
    struct ngpu_pipeline_graphics graphics;
    const struct ngpu_program *program;
    struct ngpu_pipeline_layout layout;
};

NGPU_RC_CHECK_STRUCT(ngpu_pipeline);

#endif
