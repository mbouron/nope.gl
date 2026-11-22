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

#ifndef NPRIV_ERROR_H
#define NPRIV_ERROR_H

#include <stdint.h>

#define NPRIV_FOURCC(a, b, c, d) \
   (((uint32_t)((uint8_t)(a)) << 24) | \
    ((uint32_t)((uint8_t)(b)) << 16) | \
    ((uint32_t)((uint8_t)(c)) << 8)  | \
    ((uint32_t)((uint8_t)(d)) << 0))

#define NPRIV_ERROR(a,b,c,d) (-(int32_t)NPRIV_FOURCC(a,b,c,d))

#define NPRIV_ERROR_INVALID_ARG     NPRIV_ERROR('E','a','r','g') /* Invalid argument specified */
#define NPRIV_ERROR_LIMIT_EXCEEDED  NPRIV_ERROR('E','l','i','m') /* Resource limit exceeded */
#define NPRIV_ERROR_MEMORY          NPRIV_ERROR('E','m','e','m') /* Memory/allocation error */

#endif /* NPRIV_ERROR_H */
