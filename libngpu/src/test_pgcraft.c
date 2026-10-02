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

#include <stdio.h>
#include "pgcraft.h"

int main(void)
{
    struct ngpu_pgcraft_iovar vars[16] = {0};
    for (size_t i = 0; i < 16; i++)
        vars[i].type = NGPU_TYPE_VEC2;
    const struct {
        uint32_t vertex, fragment;
        size_t count;
        bool explicit_locations;
        int expected;
    } cases[] = {
        {64, 64, 15, true, 0},
        {64, 64, 16, true, NGPU_ERROR_LIMIT_EXCEEDED},
        {64, 60, 15, true, 0},
        {64, 128, 15, true, 0},
        {64, 128, 16, true, NGPU_ERROR_LIMIT_EXCEEDED},
        {128, 40, 10, true, 0},
        {128, 40, 11, true, NGPU_ERROR_LIMIT_EXCEEDED},
        {0, 64, 1, true, NGPU_ERROR_LIMIT_EXCEEDED},
        {0, 0, 16, false, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        const struct ngpu_limits limits = {
            .max_vertex_output_components = cases[i].vertex,
            .max_fragment_input_components = cases[i].fragment,
        };
        if (ngpu_pgcraft_check_io_limits(&limits, vars, cases[i].count,
                                         cases[i].explicit_locations) != cases[i].expected) {
            fprintf(stderr, "interface limit case %zu failed\n", i);
            return 1;
        }
    }
    const struct ngpu_limits limits = {
        .max_vertex_output_components = 64,
        .max_fragment_input_components = 64,
    };
    vars[0].type = NGPU_TYPE_MAT4;
    vars[1].type = NGPU_TYPE_MAT3;
    if (ngpu_pgcraft_check_io_limits(&limits, vars, 10, true) != 0 ||
        ngpu_pgcraft_check_io_limits(&limits, vars, 11, true) != NGPU_ERROR_LIMIT_EXCEEDED)
        return 1;
    return 0;
}
