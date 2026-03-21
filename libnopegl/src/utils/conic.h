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

#ifndef CONIC_H
#define CONIC_H

#define CONIC_MAX_SUBDIVISION_COUNT 5
#define CONIC_MAX_QUADRATIC_COUNT   (1 << CONIC_MAX_SUBDIVISION_COUNT)

struct point {
    float x;
    float y;
};

struct conic {
    struct point points[3];
    float weight;
};

struct conic_converter {
    int quadratic_count;
    struct point quadratics[1 + 2 * CONIC_MAX_QUADRATIC_COUNT];
};

int conic_compute_subdivision_count(const struct conic *conic, float tolerance);
int conic_split_into_quadratics(const struct conic *conic, struct point *points, int count);

void conic_converter_to_quadratics(struct conic_converter *converter, const struct conic *conic, float tolerance);

#endif
