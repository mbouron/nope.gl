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

#include <string.h>

#include "shader2d.h"
#include "utils/bstr.h"

static int is_glsl_ident(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

void ngli_shader2d_write_source(struct bstr *b, const char *source,
                                const char *entrypoint, const char *prefix)
{
    const char *segment = source;
    const char *p = source;
    while (*p) {
        if (*p == '$' && prefix) {
            ngli_bstr_write(b, segment, (size_t)(p - segment));
            ngli_bstr_print(b, prefix);
            segment = ++p;
            while (is_glsl_ident(*p))
                p++;
            continue;
        }

        if (!is_glsl_ident(*p)) {
            p++;
            continue;
        }

        const char *ident = p;
        while (is_glsl_ident(*p))
            p++;
        if (p - ident != 4 || memcmp(ident, "main", 4))
            continue;

        const char *q = p;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            q++;
        if (*q == '(') {
            ngli_bstr_write(b, segment, (size_t)(ident - segment));
            ngli_bstr_print(b, entrypoint);
            segment = p;
        }
    }
    ngli_bstr_print(b, segment);
}
