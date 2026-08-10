/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2021-2022 GoPro Inc.
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

#ifndef BLEND_MODE_H
#define BLEND_MODE_H

#include <ngpu/ngpu.h>

enum ngli_blend_mode {
    NGLI_BLEND_MODE_DISABLED,
    NGLI_BLEND_MODE_SRC_OVER,
    NGLI_BLEND_MODE_DST_OVER,
    NGLI_BLEND_MODE_SRC_OUT,
    NGLI_BLEND_MODE_DST_OUT,
    NGLI_BLEND_MODE_SRC_IN,
    NGLI_BLEND_MODE_DST_IN,
    NGLI_BLEND_MODE_SRC_ATOP,
    NGLI_BLEND_MODE_DST_ATOP,
    NGLI_BLEND_MODE_XOR,
    NGLI_BLEND_MODE_ADD,
    NGLI_BLEND_MODE_MULTIPLY,
    NGLI_BLEND_MODE_SCREEN,
    NGLI_BLEND_MODE_DARKEN,
    NGLI_BLEND_MODE_LIGHTEN,
    NGLI_BLEND_MODE_SUBTRACT,
    NGLI_BLEND_MODE_MAX_ENUM = 0x7FFFFFFF
};

extern const struct param_choices ngli_blend_mode_choices;
int ngli_blend_mode_apply(struct ngpu_graphics_state *state, enum ngli_blend_mode mode);

#endif
