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

#ifndef GLSL_LEXER_H
#define GLSL_LEXER_H

#include <stdbool.h>
#include <stddef.h>

struct ngli_glsl_lexer;

struct ngli_glsl_token {
    const char *start;       /* Inclusive position in the original source */
    const char *end;         /* Exclusive position in the original source */
    const char *text;        /* NUL-terminated identifier with line splices removed */
    size_t length;           /* Length of text, excluding the terminator */
    size_t spliced_lines;    /* Physical newlines removed from text */
    bool directive;          /* Inside a preprocessor directive */
};

/* The source must remain valid until freep(). The lexer does not preprocess or
 * validate GLSL. It visits all branches, including inactive preprocessor code. */
struct ngli_glsl_lexer *ngli_glsl_lexer_create(const char *source);

/* Return the next identifier outside comments, or false at EOF. Punctuation,
 * whitespace and digit-led words are skipped. token->text remains valid until
 * the next call or freep(); start/end always refer to the original source. */
bool ngli_glsl_lexer_next(struct ngli_glsl_lexer *s, struct ngli_glsl_token *token);

void ngli_glsl_lexer_freep(struct ngli_glsl_lexer **sp);

#endif
