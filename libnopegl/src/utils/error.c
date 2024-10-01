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

#include <stdio.h>
#include <stdlib.h>

#include "nopegl/nopegl.h"
#include "error.h"


const char *ngli_error_to_string(int error)
{
  switch (error) {
  case NGL_ERROR_GENERIC:
    return "generic error";
  case NGL_ERROR_ACCESS:
    return "operation not allowed";
  case NGL_ERROR_BUG:
    return "a buggy code path was triggered, please report if it happens";
  case NGL_ERROR_EXTERNAL:
    return "an error occurred in an external dependency";
  case NGL_ERROR_INVALID_ARG:
    return "invalid user argument specified";
  case NGL_ERROR_INVALID_DATA:
    return "invalid input data";
  case NGL_ERROR_INVALID_USAGE:
    return "invalid public API usage";
  case NGL_ERROR_IO:
    return "input/Output error";
  case NGL_ERROR_LIMIT_EXCEEDED:
    return "hardware or resource limit exceeded";
  case NGL_ERROR_MEMORY:
    return "memory/allocation error";
  case NGL_ERROR_NOT_FOUND:
    return "target not found";
  case NGL_ERROR_UNSUPPORTED:
    return "unsupported operation";
  case NGL_ERROR_GRAPHICS_GENERIC:
    return "generic graphics error";
  case NGL_ERROR_GRAPHICS_LIMIT_EXCEEDED:
    return "graphics hardware or resource limit exceeded";
  case NGL_ERROR_GRAPHICS_MEMORY:
    return "graphics memory/allocation error";
  case NGL_ERROR_GRAPHICS_UNSUPPORTED:
    return "unsupported graphics operation";
  default:
    return "unknown error";
  }
}
