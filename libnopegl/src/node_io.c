/*
 * Copyright 2020-2022 GoPro Inc.
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

#include <stddef.h>

#include "internal.h"
#include "node_io.h"
#include "nopegl.h"
#include "params.h"
#include "precision.h"
#include "ngpu/type.h"
#include "utils/string.h"

#define OFFSET(x) offsetof(struct io_opts, x)
static const struct node_param io_params[] = {
    {"precision_out", NGLI_PARAM_TYPE_SELECT, OFFSET(precision_out), {.i32=NGPU_PRECISION_AUTO},
                      .choices=&ngli_precision_choices,
                      .desc=NGLI_DOCSTRING("precision qualifier for the output side (vertex)")},
    {"precision_in",  NGLI_PARAM_TYPE_SELECT, OFFSET(precision_in), {.i32=NGPU_PRECISION_AUTO},
                      .choices=&ngli_precision_choices,
                      .desc=NGLI_DOCSTRING("precision qualifier for the input side (fragment)")},
    {NULL}
};

#define DEFINE_IO_CLASS(class_id, class_name, type_id, dtype)   \
static int io##type_id##_init(struct ngl_node *node)            \
{                                                               \
    struct io_priv *s = node->priv_data;                        \
    s->type = dtype;                                            \
    return 0;                                                   \
}                                                               \
                                                                \
static char *io##type_id##_info_str(const struct ngl_node *node)\
{                                                               \
    return ngli_asprintf("%s", #type_id);                       \
}                                                               \
                                                                \
const struct node_class ngli_io##type_id##_class = {            \
    .id        = class_id,                                      \
    .category  = NGLI_NODE_CATEGORY_IO,                         \
    .name      = class_name,                                    \
    .init      = io##type_id##_init,                            \
    .info_str  = io##type_id##_info_str,                        \
    .opts_size = sizeof(struct io_opts),                        \
    .priv_size = sizeof(struct io_priv),                        \
    .params    = io_params,                                     \
    .params_id = "IOVar",                                       \
    .file      = __FILE__,                                      \
};

DEFINE_IO_CLASS(NGL_NODE_IOINT,    "IOInt",    int,    NGPU_TYPE_I32)
DEFINE_IO_CLASS(NGL_NODE_IOIVEC2,  "IOIVec2",  ivec2,  NGPU_TYPE_IVEC2)
DEFINE_IO_CLASS(NGL_NODE_IOIVEC3,  "IOIVec3",  ivec3,  NGPU_TYPE_IVEC3)
DEFINE_IO_CLASS(NGL_NODE_IOIVEC4,  "IOIVec4",  ivec4,  NGPU_TYPE_IVEC4)
DEFINE_IO_CLASS(NGL_NODE_IOUINT,   "IOUInt",   uint,   NGPU_TYPE_U32)
DEFINE_IO_CLASS(NGL_NODE_IOUIVEC2, "IOUIvec2", uivec2, NGPU_TYPE_UVEC2)
DEFINE_IO_CLASS(NGL_NODE_IOUIVEC3, "IOUIvec3", uivec3, NGPU_TYPE_UVEC3)
DEFINE_IO_CLASS(NGL_NODE_IOUIVEC4, "IOUIvec4", uivec4, NGPU_TYPE_UVEC4)
DEFINE_IO_CLASS(NGL_NODE_IOFLOAT,  "IOFloat",  float,  NGPU_TYPE_F32)
DEFINE_IO_CLASS(NGL_NODE_IOVEC2,   "IOVec2",   vec2,   NGPU_TYPE_VEC2)
DEFINE_IO_CLASS(NGL_NODE_IOVEC3,   "IOVec3",   vec3,   NGPU_TYPE_VEC3)
DEFINE_IO_CLASS(NGL_NODE_IOVEC4,   "IOVec4",   vec4,   NGPU_TYPE_VEC4)
DEFINE_IO_CLASS(NGL_NODE_IOMAT3,   "IOMat3",   mat3,   NGPU_TYPE_MAT3)
DEFINE_IO_CLASS(NGL_NODE_IOMAT4,   "IOMat4",   mat4,   NGPU_TYPE_MAT4)
DEFINE_IO_CLASS(NGL_NODE_IOBOOL,   "IOBool",   bool,   NGPU_TYPE_BOOL)
