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
#include <stdlib.h>
#include <string.h>

#include "glsl_lexer.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int test_tokens(void)
{
    const char *source =
        " \t/* prefix\n */ #define ma\\\nin /* main\n */ main\\\r\n_color\n"
        "// ma\\\nin\n"
        "vec4 ma\\\r\nin\\\n() { return domain + main_color + 1main; }\n"
        "main # main\n"
        "/*\n*/# main\n"
        "/* unterminated main";
    const struct {
        const char *raw;
        const char *text;
        size_t spliced_lines;
        bool directive;
    } expected[] = {
        {"define",              "define",     0, true},
        {"ma\\\nin",           "main",       1, true},
        {"main\\\r\n_color",   "main_color", 1, true},
        {"vec4",                "vec4",       0, false},
        {"ma\\\r\nin\\\n",   "main",       2, false},
        {"return",              "return",     0, false},
        {"domain",              "domain",     0, false},
        {"main_color",          "main_color", 0, false},
        {"main",                "main",       0, false},
        {"main",                "main",       0, false},
        {"main",                "main",       0, true},
    };
    struct ngli_glsl_lexer *lexer = ngli_glsl_lexer_create(source);
    struct ngli_glsl_token token;
    const char *end = source;
    for (size_t i = 0; i < sizeof(expected) / sizeof(*expected); i++) {
        CHECK(ngli_glsl_lexer_next(lexer, &token));
        CHECK(token.start >= end && token.end <= source + strlen(source));
        CHECK((size_t)(token.end - token.start) == strlen(expected[i].raw));
        CHECK(!memcmp(token.start, expected[i].raw, strlen(expected[i].raw)));
        CHECK(!strcmp(token.text, expected[i].text));
        CHECK(token.length == strlen(expected[i].text));
        CHECK(token.spliced_lines == expected[i].spliced_lines);
        CHECK(token.directive == expected[i].directive);
        end = token.end;
    }
    CHECK(!ngli_glsl_lexer_next(lexer, &token));
    CHECK(!ngli_glsl_lexer_next(lexer, &token));
    ngli_glsl_lexer_freep(&lexer);
    CHECK(!lexer);
    return 0;
}

static int test_long_identifier(void)
{
    const size_t size = 65536;
    char *source = malloc(size + sizeof(" main"));
    CHECK(source);
    memset(source, 'a', size);
    memcpy(source + size, " main", sizeof(" main"));
    struct ngli_glsl_lexer *lexer = ngli_glsl_lexer_create(source);
    struct ngli_glsl_token token;
    CHECK(ngli_glsl_lexer_next(lexer, &token));
    CHECK(token.length == size);
    CHECK(token.end == source + size);
    CHECK(!memcmp(token.text, source, size));
    CHECK(ngli_glsl_lexer_next(lexer, &token));
    CHECK(!strcmp(token.text, "main"));
    CHECK(!ngli_glsl_lexer_next(lexer, &token));
    ngli_glsl_lexer_freep(&lexer);
    free(source);
    return 0;
}

static int test_independent_scanners(void)
{
    struct ngli_glsl_lexer *first = ngli_glsl_lexer_create("#define main main\nmain");
    struct ngli_glsl_lexer *second = ngli_glsl_lexer_create("ma\\\nin");
    struct ngli_glsl_lexer *empty = ngli_glsl_lexer_create("");
    struct ngli_glsl_token token;
    CHECK(!ngli_glsl_lexer_next(empty, &token));
    CHECK(ngli_glsl_lexer_next(first, &token) && token.directive);
    CHECK(ngli_glsl_lexer_next(second, &token) && !token.directive);
    CHECK(!strcmp(token.text, "main") && token.spliced_lines == 1);
    CHECK(ngli_glsl_lexer_next(first, &token) && token.directive);
    CHECK(!strcmp(token.text, "main"));
    CHECK(!ngli_glsl_lexer_next(second, &token));
    CHECK(ngli_glsl_lexer_next(first, &token) && token.directive);
    CHECK(ngli_glsl_lexer_next(first, &token) && !token.directive);
    CHECK(!ngli_glsl_lexer_next(first, &token));
    ngli_glsl_lexer_freep(&first);
    ngli_glsl_lexer_freep(&second);
    ngli_glsl_lexer_freep(&empty);
    return 0;
}

int main(void)
{
    CHECK(test_tokens() == 0);
    CHECK(test_long_identifier() == 0);
    CHECK(test_independent_scanners() == 0);
    return 0;
}
