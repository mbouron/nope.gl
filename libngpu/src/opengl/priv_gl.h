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

#ifndef NGPU_PRIV_GL_H
#define NGPU_PRIV_GL_H

#define NGPU_PRIV_GL_FWD_(name)  \
    struct ngpu_##name;          \
    struct ngpu_##name##_gl;

NGPU_PRIV_GL_FWD_(bindgroup)
NGPU_PRIV_GL_FWD_(bindgroup_layout)
NGPU_PRIV_GL_FWD_(buffer)
NGPU_PRIV_GL_FWD_(ctx)
NGPU_PRIV_GL_FWD_(fence)
NGPU_PRIV_GL_FWD_(pipeline)
NGPU_PRIV_GL_FWD_(program)
NGPU_PRIV_GL_FWD_(rendertarget)
NGPU_PRIV_GL_FWD_(texture)

#define NGPU_PRIV_GL_ENTRY_(name, s)                                  \
    struct ngpu_##name *:       (struct ngpu_##name##_gl *)(s),       \
    const struct ngpu_##name *: (const struct ngpu_##name##_gl *)(s)

#define NGPU_PRIV_GL(s) _Generic((s),          \
    NGPU_PRIV_GL_ENTRY_(bindgroup,        s),  \
    NGPU_PRIV_GL_ENTRY_(bindgroup_layout, s),  \
    NGPU_PRIV_GL_ENTRY_(buffer,           s),  \
    NGPU_PRIV_GL_ENTRY_(ctx,              s),  \
    NGPU_PRIV_GL_ENTRY_(fence,            s),  \
    NGPU_PRIV_GL_ENTRY_(pipeline,         s),  \
    NGPU_PRIV_GL_ENTRY_(program,          s),  \
    NGPU_PRIV_GL_ENTRY_(rendertarget,     s),  \
    NGPU_PRIV_GL_ENTRY_(texture,          s)   \
)

#endif /* NGPU_PRIV_GL_H */
