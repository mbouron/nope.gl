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

#ifndef NGPU_BSTR_H
#define NGPU_BSTR_H

#include <stdarg.h>

#include "utils.h"

struct bstr;

struct bstr *ngpu_bstr_create(void);
void ngpu_bstr_print(struct bstr *b, const char *str);
void ngpu_bstr_printf(struct bstr *b, const char *fmt, ...) ngpu_printf_format(2, 3);
void ngpu_bstr_clear(struct bstr *b);
int ngpu_bstr_truncate(struct bstr *b, size_t len);
char *ngpu_bstr_strdup(const struct bstr *b);
const char *ngpu_bstr_strptr(const struct bstr *b);
size_t ngpu_bstr_len(const struct bstr *b);
int ngpu_bstr_check(const struct bstr *b);
void ngpu_bstr_freep(struct bstr **bp);

#endif
