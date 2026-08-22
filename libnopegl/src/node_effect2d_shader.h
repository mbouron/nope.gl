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

#ifndef NODE_EFFECT2D_SHADER_H
#define NODE_EFFECT2D_SHADER_H

struct hmap;
struct ngl_node;

struct effect2d_shader_info {
    const char *glsl_header;
    const char *glsl_color;
    struct hmap *resources;
    int premult;
    double start;
    double end;
};

struct effect2d_shader_info ngli_effect2d_shader_get_info(const struct ngl_node *node);

#endif
