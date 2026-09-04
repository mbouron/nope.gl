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

#ifndef RENDERPASS_H
#define RENDERPASS_H

#include <stddef.h>
#include <stdint.h>

struct ngl_node;

#define NGLI_RENDERPASS_USAGE_DEPTH   (1 << 0)
#define NGLI_RENDERPASS_USAGE_STENCIL (1 << 1)

struct renderpass_reqs {
    uint32_t usage;
};

void ngli_node_get_renderpass_reqs(struct ngl_node * const *children, size_t nb_children,
                                   struct renderpass_reqs *reqs);

#endif
