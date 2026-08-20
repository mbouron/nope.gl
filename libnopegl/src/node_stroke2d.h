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

#ifndef NODE_STROKE2D_H
#define NODE_STROKE2D_H

#include "node_paint.h"

struct ngl_node;

enum stroke2d_alignment {
    NGLI_STROKE2D_ALIGNMENT_INSIDE,
    NGLI_STROKE2D_ALIGNMENT_CENTER,
    NGLI_STROKE2D_ALIGNMENT_OUTSIDE,
};

struct stroke2d_info {
    struct ngl_node *paint;
    struct ngl_node *width_node;
    float width;
    int alignment;
};

const struct stroke2d_info *ngli_stroke2d_get_info(const struct ngl_node *node);

float ngli_stroke2d_get_width(const struct stroke2d_info *info);

float ngli_stroke2d_get_outer_edge(const struct stroke2d_info *info);

#endif
