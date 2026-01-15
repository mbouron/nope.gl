/*
 * Copyright 2023-2025 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2016-2022 GoPro Inc.
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
#include <string.h>

#include "bstr.h"
#include "memory.h"
#include "string.h"

#define BUFFER_PADDING 1024

struct bstr {
    char *str;
    size_t len;
    size_t bufsize;
    int state;
};

struct bstr *ngpu_bstr_create(void)
{
    struct bstr *b = ngpu_calloc(1, sizeof(*b));

    if (!b)
        return NULL;
    b->bufsize = BUFFER_PADDING;
    b->str = ngpu_malloc(b->bufsize);
    if (!b->str) {
        ngpu_free(b);
        return NULL;
    }
    b->str[0] = 0;
    return b;
}

void ngpu_bstr_print(struct bstr *b, const char *str)
{
    const size_t len = strlen(str);
    const size_t avail = b->bufsize - b->len;
    if (len + 1 > avail) {
        const size_t new_size = b->len + len + 1 + BUFFER_PADDING;
        void *ptr = ngpu_realloc(b->str, new_size, 1);
        if (!ptr) {
            b->state = NGPU_ERROR_MEMORY;
            return;
        }
        b->str = ptr;
        b->bufsize = new_size;
    }
    memcpy(b->str + b->len, str, len + 1);
    b->len += len;
}

void ngpu_bstr_printf(struct bstr *b, const char *fmt, ...)
{
    va_list va;

    va_start(va, fmt);
    const size_t avail = b->bufsize - b->len;
    int len = vsnprintf(b->str + b->len, avail, fmt, va);
    va_end(va);
    if (len < 0) {
        b->str[b->len] = 0;
        b->state = NGPU_ERROR_MEMORY;
        return;
    }

    if (len + 1 > avail) {
        const size_t new_size = b->len + (size_t)len + 1 + BUFFER_PADDING;
        void *ptr = ngpu_realloc(b->str, new_size, 1);
        if (!ptr) {
            b->state = NGPU_ERROR_MEMORY;
            return;
        }
        b->str = ptr;
        b->bufsize = new_size;
        va_start(va, fmt);
        len = vsnprintf(b->str + b->len, (size_t)len + 1, fmt, va);
        va_end(va);
        if (len < 0) {
            b->str[b->len] = 0;
            b->state = NGPU_ERROR_MEMORY;
            return;
        }
    }

    b->len = b->len + (size_t)len;
}

void ngpu_bstr_clear(struct bstr *b)
{
    b->len = 0;
    b->str[0] = 0;
    b->state = 0;
}

int ngpu_bstr_truncate(struct bstr *b, size_t len)
{
    if (len > b->len)
        return NGPU_ERROR_INVALID_ARG;
    b->len = len;
    b->str[b->len] = 0;
    return 0;
}

char *ngpu_bstr_strdup(const struct bstr *b)
{
    return ngpu_strdup(b->str);
}

const char *ngpu_bstr_strptr(const struct bstr *b)
{
    return b->str;
}

size_t ngpu_bstr_len(const struct bstr *b)
{
    return b->len;
}

int ngpu_bstr_check(const struct bstr *b)
{
    return b->state;
}

void ngpu_bstr_freep(struct bstr **bp)
{
    struct bstr *b = *bp;
    if (!b)
        return;
    ngpu_free(b->str);
    ngpu_freep(bp);
}
